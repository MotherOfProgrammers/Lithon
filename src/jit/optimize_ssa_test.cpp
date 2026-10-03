// 2.4 Dead Store Elimination + Copy Propagation, exercised on SSA form.
//
// The passes only make sense once 2.3 has promoted a variable to a single SSA
// version: then "has no users" is an exact test for dead, and a trivial Phi is
// an exact test for a copy. These tests build that shape both through the real
// pipeline (parse -> optimize_ssa_function -> run) and by hand (a Function with
// an Op::Phi), so each rule is pinned on its own as well as end to end.

#include "compile_function.h"
#include "exec_memory.h"
#include "ir/ir.h"
#include "ir/text_parser.h"
#include "optimize.h"

#include <algorithm>
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

static size_t count_op(const Function& fn, Op op) {
    size_t n = 0;
    for (const auto& b : fn.blocks)
        for (const auto& in : b.instrs)
            if (in.op == op) ++n;
    return n;
}

static Instr Const(ValueId r, int64_t v) {
    Instr i;
    i.op = Op::ConstInt;
    i.result = r;
    i.int_imm = v;
    return i;
}

static Instr Phi(ValueId r, std::vector<ValueId> args) {
    Instr i;
    i.op = Op::Phi;
    i.result = r;
    i.args = std::move(args);
    return i;
}

static Instr Bin(Op op, ValueId r, ValueId a, ValueId b) {
    Instr i;
    i.op = op;
    i.result = r;
    i.args = {a, b};
    return i;
}

static Instr Jump(const char* target) {
    Instr i;
    i.op = Op::Jump;
    i.result = kInvalidValue;
    i.name = target;
    return i;
}

static Instr Ret(ValueId v) {
    Instr i;
    i.op = Op::Return;
    i.result = kInvalidValue;
    i.args = {v};
    return i;
}

// A Phi whose two incoming edges carry the same value is just an alias: copy
// propagation must replace the uses and delete the Phi.
static void test_copy_propagation() {
    Function fn;
    fn.name = "f";

    BasicBlock a;
    a.label = "block0";
    a.instrs = {Const(0, 5), Jump("block1")};

    BasicBlock b;
    b.label = "block1";
    b.instrs = {Phi(1, {0, 0}), Bin(Op::Add, 2, 1, 0), Ret(2)};
    fn.blocks = {a, b};

    Module m;
    m.functions = {fn};

    OptimizeStats stats;
    copy_propagate(m.functions[0], stats);

    check(stats.copies_propagated == 1, "copy_propagate: one alias folded");
    check(count_op(m.functions[0], Op::Phi) == 0, "copy_propagate: phi removed");
    check(m.functions[0].blocks[1].instrs[0].args[0] == 0,
          "copy_propagate: use points at the alias target");

    // The rewritten function still computes 5 + 5.
    check(run_module(m) == 10, "copy_propagate: result is 10");
}

// An unused Phi with DIFFERENT operands is not a copy, but it has no users, so
// dead store elimination must delete it (and not its operands, which stay live
// through the return).
static void test_dead_phi() {
    Function fn;
    fn.name = "f";

    BasicBlock a;
    a.label = "block0";
    a.instrs = {Const(0, 1), Const(1, 2), Jump("block1")};

    BasicBlock b;
    b.label = "block1";
    b.instrs = {Phi(2, {0, 1}), Const(3, 3), Ret(3)};
    fn.blocks = {a, b};

    Module m;
    m.functions = {fn};

    OptimizeStats stats;
    copy_propagate(m.functions[0], stats);
    dead_store_elimination(m.functions[0], stats);

    check(stats.copies_propagated == 0, "dead_phi: not a copy");
    check(count_op(m.functions[0], Op::Phi) == 0, "dead_phi: removed");
    check(count_op(m.functions[0], Op::ConstInt) == 1, "dead_phi: operands folded away");
    check(run_module(m) == 3, "dead_phi: result is 3");
}

// A store overwritten before any load defines a value with no users, so the
// promoted SSA form drops both the store and the now-unused constant.
static void test_dead_store() {
    Module m = parse_ir_text(
        "function f():\n"
        "block0:\n"
        "    %0 = const_i64 5\n"
        "    store x, %0\n"
        "    %1 = const_i64 7\n"
        "    store x, %1\n"
        "    %2 = load x\n"
        "    return %2\n");

    const int64_t before = run_module(m);
    OptimizeStats stats = optimize_ssa_function(m.functions[0]);

    check(count_op(m.functions[0], Op::ConstInt) == 1, "dead_store: one constant left");
    check(stats.dead_removed >= 1, "dead_store: something died");
    check(before == 7 && run_module(m) == 7, "dead_store: result is 7");
}

// Semantics of the full promote -> optimise -> lower round trip on control
// flow that needs real Phis.
static void test_roundtrip() {
    const char* loop_ir =
        "function f():\n"
        "block0:\n"
        "    %0 = const_i64 0\n"
        "    store x, %0\n"
        "    %1 = const_i64 0\n"
        "    store i, %1\n"
        "    %2 = const_i64 5\n"
        "    store n, %2\n"
        "    jump block1\n"
        "block1:\n"
        "    %3 = load i\n"
        "    %4 = load n\n"
        "    %5 = lt %3, %4\n"
        "    branch %5, block2, block3\n"
        "block2:\n"
        "    %6 = load x\n"
        "    %7 = load i\n"
        "    %8 = add %6, %7\n"
        "    store x, %8\n"
        "    %9 = load i\n"
        "    %10 = const_i64 1\n"
        "    %11 = add %9, %10\n"
        "    store i, %11\n"
        "    jump block1\n"
        "block3:\n"
        "    %12 = load x\n"
        "    return %12\n";

    const char* diamond_ir =
        "function f():\n"
        "block0:\n"
        "    %0 = const_i64 1\n"
        "    store x, %0\n"
        "    %1 = const_i64 3\n"
        "    %2 = const_i64 4\n"
        "    %3 = lt %1, %2\n"
        "    branch %3, block1, block2\n"
        "block1:\n"
        "    %4 = const_i64 2\n"
        "    store x, %4\n"
        "    jump block3\n"
        "block2:\n"
        "    %5 = const_i64 5\n"
        "    store x, %5\n"
        "    jump block3\n"
        "block3:\n"
        "    %6 = load x\n"
        "    %7 = const_i64 8\n"
        "    %8 = add %6, %7\n"
        "    return %8\n";

    struct Case { const char* name; const char* ir; int64_t expect; };
    const Case cases[] = {
        {"loop", loop_ir, 10},
        {"diamond", diamond_ir, 10},
    };
    for (const Case& c : cases) {
        Module original = parse_ir_text(c.ir);
        const int64_t direct = run_module(original);
        Module ssa = parse_ir_text(c.ir);
        optimize_ssa_function(ssa.functions[0]);
        const int64_t opt = run_module(ssa);
        check(direct == c.expect, std::string(c.name) + ": direct == expected");
        check(opt == c.expect, std::string(c.name) + ": optimized == expected");
    }
}

// Two trivial Phis that alias each other, with the alias *target* textually
// first. This is the case the fixpoint alone does not save.
//
//   block1: %1 = phi [%0]        -> trivial, folds to %0, then DELETED
//   block4: %4 = phi [%1, %1]    -> trivial, folds to %1
//
// The second fold names %1, which the first fold has already deleted. Applying
// the round's replacements in block order therefore rewrites %4's users to
// point at a dead id, and the function reads an uninitialised register. It is
// not reachable by accident: it needs a loop-invariant value reaching a join
// through an earlier join, and it was found by inspection, not by the fuzzer,
// because the result is garbage rather than a visible wrong number.
static void test_chained_alias_order() {
    // Several id pairs, because whether the dangling reference appears depends on
    // the order the two rewrites happen to be applied in, and that order is not
    // something the pass controls. Sweeping the ids makes the test assert the
    // property rather than one lucky iteration.
    for (ValueId q = 1; q <= 12; ++q) {
        for (ValueId p = q + 1; p <= q + 6 && p <= 20; ++p) {
            Function fn;
            fn.name = "f";

            BasicBlock b0;
            b0.label = "block0";
            b0.instrs = {Const(0, 5), Jump("block1")};

            BasicBlock b1;
            b1.label = "block1";
            Instr br;
            br.op = Op::Branch;
            br.result = kInvalidValue;
            br.args = {0};
            br.name = "block2, block3";
            b1.instrs = {Phi(q, {0}), br};

            BasicBlock b2;
            b2.label = "block2";
            b2.instrs = {Jump("block4")};

            BasicBlock b3;
            b3.label = "block3";
            b3.instrs = {Const(p, 5), Jump("block4")};

            BasicBlock b4;
            b4.label = "block4";
            b4.instrs = {Phi(30, {q, q}), Bin(Op::Add, 31, 30, 0), Ret(31)};

            fn.blocks = {b0, b1, b2, b3, b4};

            Module m;
            m.functions = {fn};

            OptimizeStats stats;
            copy_propagate(m.functions[0], stats);

            check(count_op(m.functions[0], Op::Phi) == 0,
                  "chained alias: both phis removed");

            // The real assertion: every operand still names a value this
            // function defines. A dangling id compiles fine and reads whatever
            // was in that virtual register, so only this check tells the two
            // outcomes apart.
            std::vector<ValueId> defined;
            for (const auto& b : m.functions[0].blocks)
                for (const auto& in : b.instrs)
                    if (in.result != kInvalidValue) defined.push_back(in.result);
            size_t dangling = 0;
            for (const auto& b : m.functions[0].blocks)
                for (const auto& in : b.instrs)
                    for (ValueId a : in.args)
                        if (a != kInvalidValue &&
                            std::find(defined.begin(), defined.end(), a) == defined.end())
                            ++dangling;
            check(dangling == 0, "chained alias: no operand names a deleted phi");

            // Only worth executing if the IR still refers to defined values.
            // Running it otherwise does not report the bug, it crashes inside
            // the backend, which is a worse failure to read.
            if (dangling == 0)
                check(run_module(m) == 10, "chained alias: result is 10");
        }
    }
}

// validate_ssa is the last thing standing between a rewrite bug and a silently
// wrong answer, so it has to reject the one shape the old version waved
// through: an operand naming a definition that is not there. Phi arity was
// checked; "is this value defined anywhere in the function" was not.
static void test_validate_rejects_dangling_operand() {
    Function fn;
    fn.name = "f";

    BasicBlock b0;
    b0.label = "block0";
    // %4 is never defined by anything in this function.
    b0.instrs = {Const(0, 5), Bin(Op::Add, 5, 4, 0), Ret(5)};
    fn.blocks = {b0};

    std::string err;
    check(!validate_ssa(fn, &err), "validate_ssa: rejects an undefined operand");
    check(err.find("names no definition") != std::string::npos,
          "validate_ssa: says which operand and where");

    // The same function with %4 defined is fine, so the check is not simply
    // rejecting every multi-operand instruction.
    Function ok;
    ok.name = "f";
    BasicBlock ob;
    ob.label = "block0";
    ob.instrs = {Const(0, 5), Const(4, 5), Bin(Op::Add, 5, 4, 0), Ret(5)};
    ok.blocks = {ob};
    check(validate_ssa(ok, &err), "validate_ssa: accepts a defined operand");
}

int main() {
    test_copy_propagation();
    test_dead_phi();
    test_dead_store();
    test_roundtrip();
    test_chained_alias_order();
    test_validate_rejects_dangling_operand();

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS: DSE + copy propagation on SSA\n");
    return 0;
}
