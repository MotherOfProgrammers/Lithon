#include "liveness.h"
#include "ir/ir.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace lithon::ir;
using namespace lithon::jit;

static bool ok = true;

static void check(bool cond, const std::string& what) {
    printf("%s: %s\n", cond ? "OK" : "FAIL", what.c_str());
    if (!cond) ok = false;
}

static Instr konst(ValueId r, int64_t v) {
    Instr i; i.op = Op::ConstInt; i.result = r; i.int_imm = v; return i;
}
static Instr ld(ValueId r, const char* n) {
    Instr i; i.op = Op::Load; i.result = r; i.name = n; return i;
}
static Instr st(ValueId a, const char* n) {
    Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {a}; i.name = n; return i;
}
static Instr add(ValueId r, ValueId a, ValueId b) {
    Instr i; i.op = Op::Add; i.result = r; i.args = {a, b}; return i;
}
static Instr lt(ValueId r, ValueId a, ValueId b) {
    Instr i; i.op = Op::Lt; i.result = r; i.args = {a, b}; return i;
}
static Instr jmp(const char* t) {
    Instr i; i.op = Op::Jump; i.result = kInvalidValue; i.name = t; return i;
}
static Instr br(ValueId c, const char* t) {
    Instr i; i.op = Op::Branch; i.result = kInvalidValue; i.args = {c}; i.name = t; return i;
}
static Instr ret() { Instr i; i.op = Op::Return; i.result = kInvalidValue; return i; }
static Instr phi(ValueId r, ValueId a, ValueId b) {
    Instr i; i.op = Op::Phi; i.result = r; i.args = {a, b}; return i;
}
static Instr ret(ValueId v) {
    Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {v}; return i;
}
static Instr call(ValueId a, const char* n) {
    Instr i; i.op = Op::Call; i.result = kInvalidValue; i.args = {a}; i.name = n; return i;
}

// ---------------------------------------------------------------------------
// 1. Liveness across a back edge, in both directions.
// ---------------------------------------------------------------------------
//
// entry computes %2, the header compares it, and the body reads %2 again and
// loops. %2 is therefore live at the header's ENTRY and through the body --
// even though in flat instruction order its last read (in the body) sits
// before its own definition in the header is even reached.
//
// %7 is the mirror image: computed inside the body, used only there. It must
// NOT be live at the header, and must not interfere with anything there. A
// linear "drag every overlapping range out to the loop's flat end" fixup
// cannot tell these two apart: both are re-read inside the loop.
static void test_loop_carried() {
    Function fn;
    fn.name = "loop_carried";

    BasicBlock entry; entry.label = "entry";
    entry.instrs = {konst(0, 10), konst(1, 0), st(1, "i"), st(0, "limit"), jmp("header")};

    BasicBlock header; header.label = "header";
    header.instrs = {ld(2, "i"), ld(3, "limit"), lt(4, 2, 3), br(4, "body,exit")};

    BasicBlock body; body.label = "body";
    // %6 = %2 + 1, used only here: body-local.
    body.instrs = {ld(6, "limit"), add(7, 6, 2), st(7, "scratch"), jmp("header")};

    BasicBlock exit_b; exit_b.label = "exit";
    exit_b.instrs = {ret()};

    fn.blocks = {entry, header, body, exit_b};
    LivenessAnalysis lv(fn);

    const auto& r = lv.ranges();

    // %2 crosses the back edge: it is used in the body, which the header can
    // reach only by looping.
    check(r.count(2) != 0, "%2 (read in the body) has a range");
    check(lv.interferes(2, 6),
          "%2 is live at the body's entry, so it interferes with what is read there");
    // %7 is body-local and must not reach the header.
    // Note %7 and %2 DO interfere: %2 is live at the body's entry and the add
    // that defines %7 is where both are needed. What must not happen is %7
    // reaching the header -- the block-locality claim, tested below.
    check(lv.interferes(7, 2), "%7 is defined by the instruction that reads %2, so they interfere");
    check(!lv.interferes(7, 4), "%7 does not interfere with the header's comparison");
    check(!lv.interferes(7, 3), "%7 never coexists with the header's own load");
    check(!lv.interferes(7, 0), "%7 does not reach back to the entry block's constant");
}

// ---------------------------------------------------------------------------
// 2. Two uses of the same name in one block are ONE value, and the second
//    definition frees the first.
// ---------------------------------------------------------------------------
static void test_redefinition_in_one_block() {
    Function fn;
    fn.name = "redef";

    BasicBlock b0; b0.label = "b0";
    // %0 lives over [0,1] and is dead before %1 is even born at 2. Note the
    // contrast with reusing both operands across two adds, which WOULD make
    // them interfere: sharing one value twice extends its life, and it is that
    // extended life -- not the second add -- that forces them apart.
    b0.instrs = {konst(0, 1), add(2, 0, 0), konst(1, 2), add(4, 1, 1), ret(4)};
    fn.blocks = {b0};

    LivenessAnalysis lv(fn);
    check(!lv.interferes(0, 1), "two values in disjoint spans of one block do not interfere");
    check(lv.interferes(0, 2), "a value and the instruction consuming it do interfere");
    check(lv.interferes(1, 4), "and likewise for the second pair");
}

// ---------------------------------------------------------------------------
// 3. Call clobbering.
//
// Three shapes, because the block-level answer alone gets two of them wrong:
//
//   %a straddles the call  -- defined before it, used after it. Must survive.
//   %arg is the argument   -- defined before it, used only by it. Must NOT
//                             survive: the callee cannot touch a value the
//                             caller has already handed over.
//   %after is born after it -- cannot possibly be clobbered.
static void test_call_spanning() {
    Function fn;
    fn.name = "calls";

    BasicBlock b0; b0.label = "b0";
    b0.instrs = {
        ld(0, "a"),        // 0
        ld(1, "n"),        // 1
        add(2, 0, 1),     // 2   %a = a + n, lives past the call
        ld(3, "n"),        // 3
        konst(4, 1),       // 4
        add(5, 3, 4),     // 5   %arg = n + 1, consumed BY the call
        call(5, "fib"),    // 6   <- the call
        ld(6, "b"),        // 7   %after is born after the call
        add(7, 2, 6),     // 8   consumes %a, defined before the call
        add(8, 6, 6),     // 9   deliberately does NOT reuse %5
        ret(7),
    };
    fn.blocks = {b0};

    LivenessAnalysis lv(fn);
    const auto& r = lv.ranges();

    check(r.count(2) && r.at(2).spans_call,
          "%2 is defined before the call and used after it, so it must survive the call");
    check(r.count(5) && !r.at(5).spans_call,
          "%5 is only the call's argument, so it need not survive the call");
    check(r.count(6) && !r.at(6).spans_call,
          "%6 is born after the call, so it cannot be clobbered by it");
}

// ---------------------------------------------------------------------------
// 4. Virtual temps are excluded before anything is computed.
//
// plan_function marks a constant that is emitted as an immediate operand as
// having no run-time existence. It is read again on every iteration here, so
// leaving it in would charge a register against the small temp pool for
// instructions it never generates -- and, worse, would add interference edges
// from a value that has no location at all.
static void test_virtual_temps_excluded() {
    Function fn;
    fn.name = "virtual";

    BasicBlock entry; entry.label = "entry";
    entry.instrs = {konst(0, 100), st(0, "i"), jmp("header")};

    BasicBlock header; header.label = "header";
    header.instrs = {ld(1, "i"), lt(2, 1, 0), br(2, "body,exit")};

    BasicBlock body; body.label = "body";
    body.instrs = {ld(3, "i"), add(4, 3, 0), st(4, "i"), jmp("header")};

    BasicBlock exit_b; exit_b.label = "exit";
    exit_b.instrs = {ret()};

    fn.blocks = {entry, header, body, exit_b};

    LivenessAnalysis plain(fn);
    check(plain.ranges().count(0) != 0, "without the exclusion list %0 is live across the loop");

    VirtualTemps virtuals = {0, 2};   // %0 Const, %2 FusedCmp
    LivenessAnalysis filtered(fn, virtuals);

    for (ValueId id : virtuals) {
        check(filtered.ranges().count(id) == 0,
              "%%%u has no run-time existence, so it gets no live range");
    }
    // Excluding them must not perturb a real value.
    check(filtered.ranges().count(4) && plain.ranges().count(4) &&
          filtered.ranges().at(4).birth == plain.ranges().at(4).birth,
          "excluding virtual temps leaves a real value's range untouched");
}

// ---------------------------------------------------------------------------
// 5. A self-loop, the degenerate case the fixpoint must survive.
//
// The header branches to itself. %0 is read in the header and defined in the
// header, so it is live at its own block's entry -- the case a definition-order
// range model cannot represent at all.
static void test_self_loop() {
    Function fn;
    fn.name = "selfloop";

    BasicBlock b0; b0.label = "b0";
    b0.instrs = {konst(0, 0), st(0, "i"), jmp("b0")};
    fn.blocks = {b0};

    LivenessAnalysis lv(fn);
    check(lv.ranges().count(0) != 0, "a self-loop does not lose the value it carries");
    check(lv.instruction_count() == 3, "instruction count is still the flat total");
}

// ---------------------------------------------------------------------------
// 6. A value's neighbours are the ones it is live alongside, and not one more.
//    Guards the sweep against reporting every value in a block as interfering
//    with every other, which is what block-granularity alone would do.
// ---------------------------------------------------------------------------
static void test_no_spurious_interference() {
    Function fn;
    fn.name = "chain";

    BasicBlock b0; b0.label = "b0";
    // Two independent pairs joined at the end. Each pair is self-contained, so
    // the first pair is entirely dead before the second is born. Block-granular
    // liveness would call all four live throughout and force four registers
    // for a block that ever needs two.
    b0.instrs = {konst(0, 1), konst(1, 2), add(2, 0, 1),
                 konst(3, 4), konst(4, 8), add(5, 3, 4),
                 add(6, 2, 5), ret(6)};
    fn.blocks = {b0};

    LivenessAnalysis lv(fn);
    // The two pairs only meet at the final add, and only its operands and its
    // result are live across that point.
    check(!lv.interferes(0, 3), "%0 is consumed at pos 2; %3 is not born until pos 3");
    check(!lv.interferes(1, 4), "%1 is consumed at pos 2; %4 is not born until pos 4");
    check(!lv.interferes(0, 4), "%0 is dead long before %4 exists");
    // %2 is NOT one of those: it is read again by the final add, so it spans
    // the whole second pair and must overlap it. A model that let it through
    // would reuse %3's register and have the add read %3 instead of %2.
    check(lv.interferes(2, 3), "%2 is read by the final add, so it spans the second pair");
    check(lv.interferes(2, 5), "the final add needs both pair results at once");
    check(lv.interferes(2, 6), "and its result is live with its own operands");
    check(lv.interferes(2, 0), "a value interferes with what produces its operand");
}

// ---------------------------------------------------------------------------
// 8. A Phi's operands live on the incoming EDGE.
// ---------------------------------------------------------------------------
//
// This is the case block-granular use/def gets wrong in both directions at
// once. The operand of a Phi in the join is not live *in* the join -- it is read
// at the instant the predecessor's branch is taken, which is later than every
// ordinary use in that block. Filing it under use_[join] therefore (a) drags the
// value's range backwards across the whole join, and (b) never records the one
// thing that matters: that the value must stay alive to the END of the
// predecessor. A value whose last real use precedes the branch can then look
// dead, have its register reused by the very next instruction, and be read
// clobbered by the copy.
//
// The shape below is the parallel copy the emitter will have to emit: two
// merges, four sources, two predecessors. %4 and %9 must BOTH survive to the end
// of b1, and %5 and %10 to the end of b2.
static void test_phi_operands_live_at_edge() {
    Function fn;
    fn.name = "edge";
    BasicBlock b;
    auto push = [&](const char* label) { b = BasicBlock{}; b.label = label; fn.blocks.push_back(b); };

    push("b0"); fn.blocks.back().instrs = {konst(0, 1), br(0, "b1")};
    push("b1"); fn.blocks.back().instrs = {konst(4, 7), konst(9, 8), jmp("b3")};
    push("b2"); fn.blocks.back().instrs = {konst(5, 9), konst(10, 10), jmp("b3")};
    push("b3"); fn.blocks.back().instrs = {phi(6, 4, 5), phi(11, 9, 10),
                                           konst(7, 2), add(12, 6, 7), jmp("b4")};
    push("b4"); fn.blocks.back().instrs = {ret(12)};

    LivenessAnalysis L(fn);

    // The load-bearing pair: both operands of b1's copies are needed at its end.
    check(L.interferes(4, 9), "phi edge: %4 and %9 both live at the end of b1");
    check(L.interferes(5, 10), "phi edge: %5 and %10 both live at the end of b2");

    // And the merge results are ordinary values at the join.
    check(L.interferes(6, 7), "phi join: %6 interferes with a join-local value");

    // The operand is dead by the time the join runs, so it does NOT clash with
    // the result. This is exactly the non-interference that would let a
    // coalescer hand %6 the register %4 is dying in -- the property 2.8 needs
    // and the reason Phis are worth emitting directly.
    check(!L.interferes(4, 6), "phi edge: operand and its own merge result do not clash");

    // Operands from different predecessors are never needed at the same moment.
    check(!L.interferes(4, 5), "phi edge: operands of different edges do not clash");
}

// The loop case, where the naive flat model has no answer at all: the back-edge
// operand is defined in the BODY, textually after the header that consumes it,
// and it is the body's last act before jumping back. If it is not held to the
// end of the body, the loop's carried value is whatever the body's tail left
// behind.
static void test_loop_carried_phi_operand() {
    Function fn;
    fn.name = "loopphi";
    BasicBlock b;
    auto push = [&](const char* label) { b = BasicBlock{}; b.label = label; fn.blocks.push_back(b); };

    push("b0"); fn.blocks.back().instrs = {konst(1, 10), jmp("b1")};
    push("b1"); fn.blocks.back().instrs = {phi(2, 1, 5), konst(3, 1), lt(4, 3, 0),
                                           br(4, "b2")};
    // %5 is produced in the body, then a call runs, and only then does the back
    // edge hand %5 to the header's Phi. So the call must not be allowed to
    // clobber it.
    push("b2"); fn.blocks.back().instrs = {add(5, 2, 3), konst(6, 99),
                                           call(6, "print"), jmp("b1")};
    push("b3"); fn.blocks.back().instrs = {ret(2)};

    LivenessAnalysis L(fn);

    // The load-bearing one. Without the edge term, nothing in b2 references %5
    // after the call -- the Phi that reads it lives in b1 -- so the call looks
    // harmless and %5 goes in a caller-saved register. The loop then carries
    // whatever print() left behind.
    check(L.ranges().count(5) == 1, "loop phi: the back-edge operand has a live range");
    check(L.ranges().at(5).spans_call,
          "loop phi: the back-edge operand survives the call in its body");

    // The merge itself is carried around the loop, so its extent is wider than
    // the one block that defines it.
    const LiveRange& merged = L.ranges().at(2);
    check(merged.lo < merged.hi, "loop phi: the loop-carried merge spans blocks");

    // And the entry operand is held to the end of b0, where the edge is taken.
    check(L.ranges().count(1) == 1, "loop phi: the entry operand has a live range");
}

int main() {
    test_loop_carried();
    test_redefinition_in_one_block();
    test_call_spanning();
    test_virtual_temps_excluded();
    test_self_loop();
    test_no_spurious_interference();
    test_phi_operands_live_at_edge();
    test_loop_carried_phi_operand();
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}