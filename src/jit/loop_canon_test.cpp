// 2.1 Loop canonicalization: after canonicalize_loops(), every loop (except an
// entry-header loop, which has nowhere to put a block) has exactly one
// preheader and one latch -- the shape LICM and SCEV require.
//
// Structure is checked with compute_loop_info(); semantics are checked by
// compiling and running the canonicalized module and comparing to the value of
// the original. Canonicalization only adds jumps and blocks, so if it is
// correct the two runs must agree.

#include "compile_function.h"
#include "exec_memory.h"
#include "ir/ir.h"
#include "ir/text_parser.h"
#include "loop_info.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace lithon::ir;
using namespace lithon::jit;

typedef int64_t (*IntFunc)();

static int failures = 0;

static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

static int64_t run_module(Module m) {
    CompileOptions opt;
    CompiledModule c = compile_module(m, opt);
    auto it = c.function_offset.find(m.functions[0].name);
    if (it == c.function_offset.end()) throw std::runtime_error("entry not compiled");
    ExecutableBuffer mem(c.code);
    auto fn = reinterpret_cast<IntFunc>(
        reinterpret_cast<uint8_t*>(mem.data()) + it->second);
    return fn();
}

// Two distinct blocks (block1, block2) enter the header block3, so there is no
// unique preheader for the canonicalizer to find.
static const char* kTwoEntryLoop =
    "function f():\n"
    "block0:\n"
    "    %0 = const_i64 0\n"
    "    store total, %0\n"
    "    store i, %0\n"
    "    %1 = const_i64 1\n"
    "    %2 = const_i64 2\n"
    "    %3 = lt %1, %2\n"
    "    branch %3, block1, block2\n"
    "block1:\n"
    "    jump block3\n"
    "block2:\n"
    "    jump block3\n"
    "block3:\n"
    "    %4 = load i\n"
    "    %5 = const_i64 5\n"
    "    %6 = lt %4, %5\n"
    "    branch %6, block4, block5\n"
    "block4:\n"
    "    %7 = load total\n"
    "    %8 = load i\n"
    "    %9 = add %7, %8\n"
    "    store total, %9\n"
    "    %10 = load i\n"
    "    %11 = const_i64 1\n"
    "    %12 = add %10, %11\n"
    "    store i, %12\n"
    "    jump block3\n"
    "block5:\n"
    "    %13 = load total\n"
    "    return %13\n";

static const char* kTwoLatchLoop =
    "function f():\n"
    "block0:\n"
    "    %0 = const_i64 0\n"
    "    store total, %0\n"
    "    store i, %0\n"
    "    jump block1\n"
    "block1:\n"
    "    %1 = load i\n"
    "    %2 = const_i64 4\n"
    "    %3 = lt %1, %2\n"
    "    branch %3, block2, block4\n"
    "block2:\n"
    "    %4 = load total\n"
    "    %5 = load i\n"
    "    %6 = add %4, %5\n"
    "    store total, %6\n"
    "    %7 = load i\n"
    "    %8 = const_i64 1\n"
    "    %9 = add %7, %8\n"
    "    store i, %9\n"
    "    %10 = load i\n"
    "    %11 = const_i64 2\n"
    "    %12 = lt %10, %11\n"
    "    branch %12, block3, block1\n"
    "block3:\n"
    "    jump block1\n"
    "block4:\n"
    "    %13 = load total\n"
    "    return %13\n";

static const char* kNestedLoop =
    "function f():\n"
    "block0:\n"
    "    %0 = const_i64 0\n"
    "    store i, %0\n"
    "    store total, %0\n"
    "    store j, %0\n"
    "    jump block1\n"
    "block1:\n"
    "    %1 = load i\n"
    "    %2 = const_i64 3\n"
    "    %3 = lt %1, %2\n"
    "    branch %3, block2, block6\n"
    "block2:\n"
    "    store j, %0\n"
    "    jump block3\n"
    "block3:\n"
    "    %4 = load j\n"
    "    %5 = const_i64 3\n"
    "    %6 = lt %4, %5\n"
    "    branch %6, block4, block5\n"
    "block4:\n"
    "    %7 = load total\n"
    "    %8 = load j\n"
    "    %9 = add %7, %8\n"
    "    store total, %9\n"
    "    %10 = load j\n"
    "    %11 = const_i64 1\n"
    "    %12 = add %10, %11\n"
    "    store j, %12\n"
    "    jump block3\n"
    "block5:\n"
    "    %13 = load i\n"
    "    %14 = const_i64 1\n"
    "    %15 = add %13, %14\n"
    "    store i, %15\n"
    "    jump block1\n"
    "block6:\n"
    "    %16 = load total\n"
    "    return %16\n";

static void test_two_entry() {
    Module before = parse_ir_text(kTwoEntryLoop);
    const int64_t direct = run_module(before);

    Module after = parse_ir_text(kTwoEntryLoop);
    CanonStats cs = canonicalize_loops(after.functions[0]);
    LoopInfo info = compute_loop_info(after.functions[0]);

    check(cs.preheaders == 1, "two-entry: one preheader inserted");
    check(info.loops.size() == 1, "two-entry: one loop");
    if (info.loops.size() == 1) {
        check(info.loops[0].preheader != kNoBlock, "two-entry: preheader exists");
        check(info.loops[0].latches.size() == 1, "two-entry: single latch");
    }
    const int64_t opt = run_module(after);
    check(direct == 10 && opt == 10, "two-entry: result stays 10");
}

static void test_two_latch() {
    Module before = parse_ir_text(kTwoLatchLoop);
    const int64_t direct = run_module(before);

    Module after = parse_ir_text(kTwoLatchLoop);
    CanonStats cs = canonicalize_loops(after.functions[0]);
    LoopInfo info = compute_loop_info(after.functions[0]);

    check(cs.latches_merged == 1, "two-latch: one latch merged");
    check(info.loops.size() == 1, "two-latch: one loop");
    if (info.loops.size() == 1) {
        check(info.loops[0].preheader != kNoBlock, "two-latch: preheader exists");
        check(info.loops[0].latches.size() == 1, "two-latch: single latch");
    }
    const int64_t opt = run_module(after);
    check(direct == 6 && opt == 6, "two-latch: result stays 6");
}

static void test_nested_and_idempotent() {
    Module after = parse_ir_text(kNestedLoop);
    canonicalize_loops(after.functions[0]);
    LoopInfo info = compute_loop_info(after.functions[0]);

    check(info.loops.size() == 2, "nested: two loops");
    for (size_t i = 0; i < info.loops.size(); ++i) {
        check(info.loops[i].preheader != kNoBlock,
              "nested: loop " + std::to_string(i) + " has a preheader");
        check(info.loops[i].latches.size() == 1,
              "nested: loop " + std::to_string(i) + " has one latch");
    }
    check(run_module(after) == 9, "nested: result is 9");

    // Running it again must be a no-op.
    CanonStats again = canonicalize_loops(after.functions[0]);
    check(again.preheaders == 0 && again.latches_merged == 0,
          "nested: canonicalization is idempotent");
}

// 2.6. A loop exit that lands on a block something ELSE can also reach gets its
// own block; one that already has a single predecessor is left alone.
//
// The second half matters as much as the first. Splitting unconditionally would
// add a block that jumps to a block that jumps nowhere, on every loop in the
// language, for no gain -- and "no gain" would be measured as a regression
// nobody could attribute.
static void test_exit_block_synthesis() {
    // block0 branches either into the loop or straight to block5, so block5 --
    // the loop's exit target -- has two predecessors. That is the shape
    // synthesis exists for. The loop increments `i`, so the program terminates.
    const char* ir = R"(
function __main__():
block0:
    %0 = const_i64 1
    branch %0, block1, block5
block1:
    %1 = const_i64 4
    %2 = const_i64 0
    store i, %2
    jump block2
block2:
    %3 = load i
    %4 = const_i64 4
    %5 = lt %3, %4
    branch %5, block3, block5
block3:
    %7 = load i
    %8 = const_i64 1
    %9 = add %7, %8
    store i, %9
    jump block2
block5:
    %6 = const_i64 9
    call print, %6
    return %6
)";
    Module m = parse_ir_text(ir);
    check(m.functions.size() == 1, "exit synthesis: parsed one function");
    if (m.functions.empty()) return;
    Function& fn = m.functions[0];

    const Cfg before = build_cfg(fn);
    check(before.pred[before.index.at("block5")].size() == 2,
          "exit synthesis: the exit block really does start with two predecessors");

    const LoopInfo info = compute_loop_info(fn);
    check(info.loops.size() == 1, "exit synthesis: exactly one loop");
    if (info.loops.empty()) return;
    check(info.loops[0].exits.size() == 1, "exit synthesis: one exit edge");
    if (info.loops[0].exits.empty()) return;
    const LoopExit e = info.loops[0].exits[0];
    check(e.to == before.index.at("block5") && e.from == before.index.at("block2"),
          "exit synthesis: the loop's exit edge is the one expected");

    const ExitSplitStats st = synthesize_loop_exits(fn);
    check(st.blocks_added == 1,
          "exit synthesis: exactly one block added (got " + std::to_string(st.blocks_added) + ")");

    // The loop must still be the same loop, and block5 must still be outside it:
    // an exit block that got absorbed would be a miscompile, not a split.
    const LoopInfo after = compute_loop_info(fn);
    check(after.loops.size() == 1, "exit synthesis: still exactly one loop");
    const Cfg g = build_cfg(fn);
    if (!after.loops.empty())
        check(!after.loops[0].contains(g.index.at("block5")),
              "exit synthesis: the post-loop code is not absorbed into the loop");

    // Every terminator must still name a block that exists, and the fresh block
    // must actually reach block5 -- otherwise the graph is well-formed and
    // useless, which is the shape a retarget bug leaves behind.
    bool has_exit_block = false;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const std::string& label = fn.blocks[b].label;
        if (label.find("block5_exit") != std::string::npos) {
            has_exit_block = true;
            bool reaches_block5 = false;
            for (size_t s : g.succ[b]) if (g.labels[s] == "block5") reaches_block5 = true;
            check(reaches_block5, "exit synthesis: the dedicated exit block reaches block5");
        }
        if (fn.blocks[b].instrs.empty()) continue;
        const Instr& term = fn.blocks[b].instrs.back();
        if (term.op == Op::Jump) check(g.index.count(term.name) != 0, "jump target resolves");
        if (term.op == Op::Branch)
            for (const auto& t : branch_targets(term))
                check(g.index.count(t) != 0, "branch target resolves");
    }
    check(has_exit_block, "exit synthesis: the dedicated exit block exists");

    // Semantics: the transform only redirects an edge and adds a jump, so the
    // program must still print 9.
    check(run_module(m) == 9, "exit synthesis: the program still computes the same answer");
}

// A loop whose exit block only the loop reaches needs no second block.
static void test_single_predecessor_exit_is_left_alone() {
    const char* ir = R"(
function __main__():
block0:
    %0 = const_i64 1
    jump block1
block1:
    %1 = const_i64 4
    %2 = const_i64 0
    store i, %2
    jump block2
block2:
    %3 = load i
    %4 = const_i64 4
    %5 = lt %3, %4
    branch %5, block3, block4
block3:
    %7 = load i
    %8 = const_i64 1
    %9 = add %7, %8
    store i, %9
    jump block2
block4:
    %6 = const_i64 9
    call print, %6
    return %6
)";
    Module m = parse_ir_text(ir);
    if (m.functions.empty()) { check(false, "single-pred exit: parsed one function"); return; }

    const size_t blocks_before = m.functions[0].blocks.size();
    const ExitSplitStats st = synthesize_loop_exits(m.functions[0]);
    check(st.blocks_added == 0,
          "single-pred exit: no block added (got " + std::to_string(st.blocks_added) + ")");
    check(m.functions[0].blocks.size() == blocks_before, "single-pred exit: block count unchanged");
}

int main() {
    test_two_entry();
    test_two_latch();
    test_nested_and_idempotent();
    test_exit_block_synthesis();
    test_single_predecessor_exit_is_left_alone();

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS: loop canonicalization (preheader + single latch)\n");
    return 0;
}
