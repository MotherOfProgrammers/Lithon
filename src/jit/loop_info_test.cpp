// Unit tests for the 2.1 CFG + dominator + natural-loop analysis.
//
// Every function here is built by hand so the expected loop structure is
// stated once, in the test, rather than inferred from a running frontend.
// The cases pin the properties later SSA passes depend on:
//   - a back edge is an edge A->B with B dominating A (so a backward jump
//     from an unreachable block is NOT a loop -- the old index heuristic's
//     false positive),
//   - bodies are computed by reverse reachability from the latch,
//   - nesting, latches, exits, and preheaders are identified,
//   - an inner-loop preheader inside an enclosing loop is still a valid
//     preheader (it runs once per outer iteration, which is what "once before
//     the header" means for the inner loop).

#include <cstdio>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "jit/loop_info.h"

using namespace lithon;
using jit::Loop;
using jit::LoopInfo;

static int g_failures = 0;

static void expect(bool ok, const std::string& what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

static ir::Instr jump(const std::string& target) {
    ir::Instr i;
    i.op = ir::Op::Jump;
    i.result = ir::kInvalidValue;
    i.name = target;
    return i;
}

static ir::Instr branch(const std::string& a, const std::string& b) {
    ir::Instr i;
    i.op = ir::Op::Branch;
    i.result = ir::kInvalidValue;
    i.name = a + ", " + b;
    return i;
}

static ir::Instr ret() {
    ir::Instr i;
    i.op = ir::Op::Return;
    i.result = ir::kInvalidValue;
    return i;
}

// Small builder: add_block("name") then add an Instr, returns the function.
struct FnBuilder {
    ir::Function fn;
    explicit FnBuilder(const std::string& name) { fn.name = name; }
    FnBuilder& block(const std::string& label) {
        ir::BasicBlock b;
        b.label = label;
        fn.blocks.push_back(std::move(b));
        return *this;
    }
    FnBuilder& operator<<(const ir::Instr& in) {
        fn.blocks.back().instrs.push_back(in);
        return *this;
    }
};

static const Loop* loop_by_header(const LoopInfo& info, size_t header) {
    return info.by_header(header);
}

static std::string blocks_str(const ir::Function& fn, const Loop& loop) {
    std::string s;
    for (size_t b : loop.blocks) s += (s.empty() ? "" : ",") + fn.blocks[b].label;
    return s;
}

static void test_forward_jump_is_not_a_loop() {
    // b2 is unreachable and jumps back to b0. b0 does not dominate b2, so it
    // is not a back edge -- the old index-based find_loops() called this a loop.
    FnBuilder f("forward");
    f.block("b0") << branch("b1", "b3");
    f.block("b1") << jump("b3");
    f.block("b2") << jump("b0");
    f.block("b3") << ret();

    LoopInfo info = jit::compute_loop_info(f.fn);
    expect(info.loops.empty(), "forward jump to a non-dominating block is not a loop");
}

static void test_simple_while() {
    // P -> H; H branches to body B or exit X; B -> H.
    FnBuilder f("while");
    f.block("P") << jump("H");
    f.block("H") << branch("B", "X");
    f.block("B") << jump("H");
    f.block("X") << ret();

    LoopInfo info = jit::compute_loop_info(f.fn);
    expect(info.loops.size() == 1, "while: exactly one loop");
    const Loop* l = loop_by_header(info, 1);
    expect(l != nullptr, "while: header H found");
    if (!l) return;
    expect(f.fn.blocks[l->header].label == "H", "while: header is H");
    expect(l->preheader != jit::kNoBlock && f.fn.blocks[l->preheader].label == "P",
           "while: preheader is P");
    expect(l->latches.size() == 1 && f.fn.blocks[l->latches[0]].label == "B",
           "while: single latch B");
    expect(blocks_str(f.fn, *l) == "H,B", "while: body is {H,B}");
    expect(l->exit_blocks.size() == 1 && f.fn.blocks[l->exit_blocks[0]].label == "X",
           "while: single exit X");
    expect(l->parent == jit::kNoBlock, "while: outermost");
}

static void test_multi_exit() {
    // H branches to B or X; B branches back to H or out to Y.
    FnBuilder f("multiexit");
    f.block("P") << jump("H");
    f.block("H") << branch("B", "X");
    f.block("B") << branch("H", "Y");
    f.block("X") << ret();
    f.block("Y") << ret();

    LoopInfo info = jit::compute_loop_info(f.fn);
    expect(info.loops.size() == 1, "multi-exit: one loop");
    const Loop* l = loop_by_header(info, 1);
    expect(l != nullptr, "multi-exit: header found");
    if (!l) return;
    expect(l->exit_blocks.size() == 2, "multi-exit: two exit blocks");
    expect(blocks_str(f.fn, *l) == "H,B", "multi-exit: body is {H,B}");
    expect(l->exits.size() == 2, "multi-exit: two exit edges");
}

static void test_two_latches() {
    // H branches to A or B, and both jump back to H.
    FnBuilder f("twolatch");
    f.block("P") << jump("H");
    f.block("H") << branch("A", "B");
    f.block("A") << jump("H");
    f.block("B") << jump("H");

    LoopInfo info = jit::compute_loop_info(f.fn);
    expect(info.loops.size() == 1, "two-latch: one loop");
    const Loop* l = loop_by_header(info, 1);
    expect(l != nullptr, "two-latch: header found");
    if (!l) return;
    expect(l->latches.size() == 2, "two-latch: two latches");
    expect(blocks_str(f.fn, *l) == "H,A,B", "two-latch: body is {H,A,B}");
    expect(l->preheader != jit::kNoBlock && f.fn.blocks[l->preheader].label == "P",
           "two-latch: preheader is P");
}

static void test_no_preheader_when_header_is_entry() {
    FnBuilder f("entryloop");
    f.block("H") << branch("B", "X");
    f.block("B") << jump("H");
    f.block("X") << ret();

    LoopInfo info = jit::compute_loop_info(f.fn);
    expect(info.loops.size() == 1, "entryloop: one loop");
    const Loop* l = loop_by_header(info, 0);
    expect(l != nullptr, "entryloop: header is the entry block");
    if (!l) return;
    expect(l->preheader == jit::kNoBlock, "entryloop: no preheader exists");
}

static void test_nested() {
    // Outer O, inner I. The inner exit (IX) flows back to O, making IX the
    // outer latch. The inner loop's only outside predecessor is the outer
    // header O: that is its preheader, because it executes exactly once
    // before each entry to the inner loop (once per outer iteration).
    FnBuilder f("nested");
    f.block("P") << jump("O");
    f.block("O") << branch("I", "OX");
    f.block("I") << branch("IB", "IX");
    f.block("IB") << jump("I");
    f.block("IX") << jump("O");
    f.block("OX") << ret();

    LoopInfo info = jit::compute_loop_info(f.fn);
    expect(info.loops.size() == 2, "nested: two loops");
    const Loop* outer = loop_by_header(info, 1);
    const Loop* inner = loop_by_header(info, 2);
    expect(outer != nullptr, "nested: outer header O");
    expect(inner != nullptr, "nested: inner header I");
    if (!outer || !inner) return;
    expect(blocks_str(f.fn, *outer) == "O,I,IB,IX", "nested: outer body is {O,I,IB,IX}");
    expect(blocks_str(f.fn, *inner) == "I,IB", "nested: inner body is {I,IB}");
    expect(outer->latches.size() == 1 && f.fn.blocks[outer->latches[0]].label == "IX",
           "nested: outer latch IX");
    expect(inner->latches.size() == 1 && f.fn.blocks[inner->latches[0]].label == "IB",
           "nested: inner latch IB");
    expect(inner->parent != jit::kNoBlock, "nested: inner has a parent");
    if (inner->parent != jit::kNoBlock)
        expect(f.fn.blocks[info.loops[inner->parent].header].label == "O",
               "nested: inner parent is outer");
    expect(outer->parent == jit::kNoBlock, "nested: outer is outermost");
    expect(inner->preheader != jit::kNoBlock && f.fn.blocks[inner->preheader].label == "O",
           "nested: inner preheader is O (runs once per outer iteration)");
}

static void test_non_contiguous_loop_still_has_depth() {
    // The flat projection this replaces scored loop depth from a CONTIGUOUS
    // block range, so a loop whose body is split by an unrelated block scored
    // as depth 0 -- and, worse, a loop emitted out of order (as
    // accumulator_unroll does, jamming the main loop after the remainder) was
    // not a loop at all under a textual back-edge rule. Both follow from using
    // dominators, so the proof is that a non-contiguous body is still depth 1.
    FnBuilder f("split");
    f.block("H") << branch("B", "X");
    f.block("B") << jump("M");          // an unrelated block wedged into the body
    f.block("M") << jump("H");          // the latch, textually after B
    f.block("X") << ret();

    const jit::LoopInfo info = jit::compute_loop_info(f.fn);
    expect(info.loops.size() == 1, "non-contiguous body: still exactly one loop");
    if (info.loops.empty()) return;

    const jit::Loop& l = info.loops[0];
    expect(l.blocks.size() == 3, "non-contiguous body: all three blocks belong to the loop");
    expect(l.contains(2), "the wedged block is in the loop");

    // Depth, as the promotion weighting computes it: every block of the loop
    // counts once, including the one a block-range test would have missed.
    int max_depth = 0;
    for (size_t b = 0; b < f.fn.blocks.size(); ++b) {
        int d = 0;
        for (const auto& loop : info.loops) if (loop.contains(b)) ++d;
        max_depth = std::max(max_depth, d);
    }
    expect(max_depth == 1, "non-contiguous body: its blocks are still depth 1");
    expect(!l.contains(3), "the exit block is not in the loop");
}

int main() {
    test_forward_jump_is_not_a_loop();
    test_simple_while();
    test_multi_exit();
    test_two_latches();
    test_no_preheader_when_header_is_entry();
    test_nested();
    test_non_contiguous_loop_still_has_depth();

    if (g_failures) {
        std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
        return 1;
    }
    std::printf("loop_info_test: all assertions passed\n");
    return 0;
}
