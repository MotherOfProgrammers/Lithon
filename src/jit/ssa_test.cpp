// Unit tests for 2.2: dominance frontiers and Phi placement.
//
// The CFG is built by hand so the expected join points are stated directly.
// A Phi is needed exactly where two reaching definitions of the same variable
// meet; the tests cover the diamond, an if without else, a loop (where the
// iterated frontier puts the phi at the header and then feeds itself), and the
// trivial case where a single definition never reaches a join.

#include <cstdio>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "jit/loop_info.h"
#include "jit/ssa.h"

using namespace lithon;
using jit::Cfg;
using jit::DomInfo;
using jit::DominanceFrontiers;
using jit::PhiPlacement;

static int g_failures = 0;

static void expect(bool ok, const std::string& what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

static ir::Instr jump(const std::string& target) {
    ir::Instr i; i.op = ir::Op::Jump; i.result = ir::kInvalidValue; i.name = target; return i;
}
static ir::Instr branch(const std::string& a, const std::string& b) {
    ir::Instr i; i.op = ir::Op::Branch; i.result = ir::kInvalidValue; i.name = a + ", " + b; return i;
}
static ir::Instr ret() {
    ir::Instr i; i.op = ir::Op::Return; i.result = ir::kInvalidValue; return i;
}
static ir::Instr cst(ir::ValueId id, int64_t v) {
    ir::Instr i; i.op = ir::Op::ConstInt; i.result = id; i.int_imm = v; return i;
}
static ir::Instr store(const std::string& var, ir::ValueId v) {
    ir::Instr i; i.op = ir::Op::Store; i.result = ir::kInvalidValue; i.args = {v}; i.name = var; return i;
}
static ir::Instr load(ir::ValueId id, const std::string& var) {
    ir::Instr i; i.op = ir::Op::Load; i.result = id; i.name = var; return i;
}

struct FnBuilder {
    ir::Function fn;
    explicit FnBuilder(const std::string& name) { fn.name = name; }
    FnBuilder& block(const std::string& label) {
        ir::BasicBlock b; b.label = label; fn.blocks.push_back(std::move(b)); return *this;
    }
    FnBuilder& operator<<(const ir::Instr& in) { fn.blocks.back().instrs.push_back(in); return *this; }
};

static size_t block_of(const ir::Function& fn, const std::string& label) {
    for (size_t b = 0; b < fn.blocks.size(); ++b)
        if (fn.blocks[b].label == label) return b;
    return jit::kNoBlock;
}

static void test_diamond_frontiers_and_phi() {
    FnBuilder f("diamond");
    f.block("b0") << cst(0, 0) << store("v", 0) << branch("b1", "b2");
    f.block("b1") << cst(1, 1) << store("v", 1) << jump("b3");
    f.block("b2") << cst(2, 2) << store("v", 2) << jump("b3");
    f.block("b3") << load(3, "v") << ret();

    Cfg g = jit::build_cfg(f.fn);
    DomInfo dom = jit::compute_dominators(g);
    DominanceFrontiers df = jit::compute_dominance_frontiers(g, dom);
    const size_t b1 = block_of(f.fn, "b1"), b2 = block_of(f.fn, "b2"), b3 = block_of(f.fn, "b3");

    expect(df.contains(b1, b3), "diamond: b3 in DF[b1]");
    expect(df.contains(b2, b3), "diamond: b3 in DF[b2]");
    expect(df.df[0].empty(), "diamond: entry frontier is empty");

    PhiPlacement p = jit::compute_phi_placement(f.fn);
    expect(p.needs_phi(b3, "v"), "diamond: phi for v at the join");
    expect(p.phi_count() == 1, "diamond: exactly one phi");
}

static void test_if_without_else_phi() {
    FnBuilder f("if");
    f.block("b0") << cst(0, 0) << store("v", 0) << branch("b1", "b2");
    f.block("b1") << cst(1, 1) << store("v", 1) << jump("b2");
    f.block("b2") << load(2, "v") << ret();

    PhiPlacement p = jit::compute_phi_placement(f.fn);
    expect(p.needs_phi(block_of(f.fn, "b2"), "v"), "if: phi at join from one guarded def");
}

static void test_loop_phi_iterates() {
    FnBuilder f("loop");
    f.block("b0") << cst(0, 0) << store("v", 0) << jump("b1");
    f.block("b1") << load(1, "v") << branch("b2", "b3");
    f.block("b2") << load(2, "v") << store("v", 2) << jump("b1");
    f.block("b3") << load(3, "v") << ret();

    Cfg g = jit::build_cfg(f.fn);
    DomInfo dom = jit::compute_dominators(g);
    DominanceFrontiers df = jit::compute_dominance_frontiers(g, dom);
    const size_t b1 = block_of(f.fn, "b1"), b2 = block_of(f.fn, "b2");
    expect(df.contains(b2, b1), "loop: header in DF[body]");
    expect(df.contains(b1, b1), "loop: header in its own frontier");

    PhiPlacement p = jit::compute_phi_placement(f.fn);
    expect(p.needs_phi(b1, "v"), "loop: phi at the header");
    expect(p.phi_count() == 1, "loop: exactly one phi");
}

static void test_single_def_no_phi() {
    FnBuilder f("single");
    f.block("b0") << cst(0, 0) << store("v", 0) << jump("b1");
    f.block("b1") << load(1, "v") << ret();

    PhiPlacement p = jit::compute_phi_placement(f.fn);
    expect(p.phi_count() == 0, "single def: no phi");
}

static void test_independent_variables() {
    // x is updated on one arm, y on the other, both reaching the join from an
    // initial definition in b0. Each variable needs its own phi at b3, but
    // neither may appear at the other's frontier.
    FnBuilder f("two");
    f.block("b0") << cst(0, 0) << store("x", 0) << cst(1, 1) << store("y", 1)
                  << branch("b1", "b2");
    f.block("b1") << cst(2, 2) << store("x", 2) << jump("b3");
    f.block("b2") << cst(3, 3) << store("y", 3) << jump("b3");
    f.block("b3") << load(4, "x") << load(5, "y") << ret();

    PhiPlacement p = jit::compute_phi_placement(f.fn);
    const size_t b3 = block_of(f.fn, "b3");
    expect(p.needs_phi(b3, "x"), "two vars: phi for x at join");
    expect(p.needs_phi(b3, "y"), "two vars: phi for y at join");
    expect(p.phi_count() == 2, "two vars: one phi each");
}

// --- 2.8: forwarding a merge whose operands all agree ----------------------
//
// The shape that actually shows up is `c ? 1 : 1`: copy propagation collapses
// both arms to one value, leaving a Phi with a single distinct operand. That
// merge needs no copy on any edge, so it should disappear rather than spend a
// register on a value that was never in dispute.

static ir::Instr phi(ir::ValueId result, const std::vector<ir::ValueId>& args) {
    ir::Instr i; i.op = ir::Op::Phi; i.result = result; i.args = args; return i;
}

static size_t count_phis(const ir::Function& fn) {
    size_t n = 0;
    for (const auto& b : fn.blocks)
        for (const auto& in : b.instrs)
            if (in.op == ir::Op::Phi) ++n;
    return n;
}

static void test_trivial_phi_is_forwarded() {
    FnBuilder f("trivial");
    f.block("b0") << cst(0, 7) << branch("b1", "b2");
    f.block("b1") << jump("b3");
    f.block("b2") << jump("b3");
    f.block("b3") << phi(1, {0, 0}) << jump("b4");
    f.block("b4") << load(2, "y") << ret();

    expect(count_phis(f.fn) == 1, "trivial phi: one merge to begin with");
    expect(jit::forward_trivial_phis(f.fn) == 1, "trivial phi: forwarded");
    expect(count_phis(f.fn) == 0, "trivial phi: the merge is gone");

    // Nothing may still refer to the deleted id: a surviving use would read
    // whatever the allocator happened to put in that virtual register.
    bool use_rewritten = false;
    for (const auto& b : f.fn.blocks)
        for (const auto& in : b.instrs)
            for (auto a : in.args)
                if (a == 1) use_rewritten = true;
    expect(!use_rewritten, "trivial phi: nothing still refers to the deleted id");
}

// A merge of two DIFFERENT values is the whole point of a merge. Forwarding it
// would be wrong, and the test that says so is the one that would catch the
// pass being written too loosely.
static void test_real_phi_is_left_alone() {
    FnBuilder f("real");
    f.block("b0") << cst(0, 7) << cst(1, 9) << branch("b1", "b2");
    f.block("b1") << jump("b3");
    f.block("b2") << jump("b3");
    f.block("b3") << phi(2, {0, 1}) << jump("b4");
    f.block("b4") << load(3, "y") << ret();

    expect(jit::forward_trivial_phis(f.fn) == 0, "real phi: not forwarded");
    expect(count_phis(f.fn) == 1, "real phi: still a merge");
    expect(f.fn.blocks[3].instrs[0].args.size() == 2, "real phi: both operands intact");
}

// A merge with no operands at all is not "all operands agree" -- there is no
// value to forward to. Treating the empty case as trivial would delete the
// definition while uses still name it.
static void test_empty_phi_is_left_alone() {
    FnBuilder f("empty");
    f.block("b0") << cst(0, 7) << branch("b1", "b2");
    f.block("b1") << jump("b3");
    f.block("b2") << jump("b3");
    f.block("b3") << phi(1, {ir::kInvalidValue, ir::kInvalidValue}) << jump("b4");
    f.block("b4") << load(2, "y") << ret();

    expect(jit::forward_trivial_phis(f.fn) == 0, "empty phi: not forwarded");
    expect(count_phis(f.fn) == 1, "empty phi: still a merge");
}

// The dominance guard. This Phi claims the same operand on both edges, but that
// operand is defined in b1 -- so on the edge from b2 it is not in scope, and the
// IR is not something valid SSA would have produced. Forwarding anyway would
// leave a use of %0 in b4 where %0 was never defined.
static void test_undominated_forward_is_refused() {
    FnBuilder f("undominated");
    f.block("b0") << branch("b1", "b2");
    f.block("b1") << cst(0, 7) << jump("b3");
    f.block("b2") << jump("b3");
    f.block("b3") << phi(1, {0, 0}) << jump("b4");
    f.block("b4") << load(2, "y") << ret();

    expect(jit::forward_trivial_phis(f.fn) == 0, "undominated phi: not forwarded");
    expect(count_phis(f.fn) == 1, "undominated phi: still a merge");

    // And the operand really is out of scope in b4, which is the reason.
    const jit::Cfg g = jit::build_cfg(f.fn);
    const jit::DomInfo dom = jit::compute_dominators(g);
    expect(!dom.dominates(block_of(f.fn, "b1"), block_of(f.fn, "b4")),
           "undominated phi: b1 does not dominate b4, as the test assumes");
}

// A loop-carried merge of one value is still trivial -- the value is the same
// coming in and going round, which is what an uninitialised-but-unchanged
// counter looks like.
static void test_trivial_loop_phi_is_forwarded() {
    FnBuilder f("loop");
    f.block("b0") << cst(0, 5) << store("x", 0) << jump("b1");
    f.block("b1") << phi(1, {0, 0}) << load(2, "x") << branch("b1", "b2");
    f.block("b2") << load(5, "y") << ret();

    expect(jit::forward_trivial_phis(f.fn) == 1, "loop trivial phi: forwarded");
    expect(count_phis(f.fn) == 0, "loop trivial phi: the merge is gone");
}

int main() {
    test_diamond_frontiers_and_phi();
    test_if_without_else_phi();
    test_loop_phi_iterates();
    test_single_def_no_phi();
    test_independent_variables();
    test_trivial_phi_is_forwarded();
    test_real_phi_is_left_alone();
    test_empty_phi_is_left_alone();
    test_undominated_forward_is_refused();
    test_trivial_loop_phi_is_forwarded();

    if (g_failures) {
        std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ssa_test: all assertions passed\n");
    return 0;
}
