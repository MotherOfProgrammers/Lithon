#include "liveness.h"
#include "ir/ir.h"
#include <cstdio>
#include <algorithm>
#include <vector>

using namespace lithon::ir;
using namespace lithon::jit;

// A constant defined BEFORE the loop and read on every iteration is the
// case the virtual-temp exclusion exists for. plan_function marks it Const
// (it is emitted as an immediate operand, so it has no run-time existence
// at all), and its id must therefore get no live range -- not a shortened
// one, none. Left in, extend_across_loops drags it to the loop's back edge
// purely because it is textually re-read, charging a register against the
// small temp pool for a value that generates zero instructions.
static Function loop_reading_preloop_const() {
    Function fn;
    fn.name = "preloop_const";

    auto konst = [](ValueId r, int64_t v) { Instr i; i.op = Op::ConstInt; i.result = r; i.int_imm = v; return i; };
    auto load = [](ValueId r, const char* n) { Instr i; i.op = Op::Load; i.result = r; i.name = n; return i; };
    auto store = [](ValueId a, const char* n) { Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {a}; i.name = n; return i; };
    auto jump = [](const char* t) { Instr i; i.op = Op::Jump; i.result = kInvalidValue; i.name = t; return i; };
    auto lt = [](ValueId r, ValueId a, ValueId b) { Instr i; i.op = Op::Lt; i.result = r; i.args = {a, b}; return i; };
    auto add = [](ValueId r, ValueId a, ValueId b) { Instr i; i.op = Op::Add; i.result = r; i.args = {a, b}; return i; };
    auto branch = [](ValueId c, const char* t) { Instr i; i.op = Op::Branch; i.result = kInvalidValue; i.args = {c}; i.name = t; return i; };

    BasicBlock entry; entry.label = "block0";
    entry.instrs = {konst(0, 100), store(0, "i"), konst(1, 0), store(1, "total"), jump("loop")};

    BasicBlock header; header.label = "loop";
    header.instrs = {load(2, "i"), lt(3, 2, 0), branch(3, "body,exit")};

    BasicBlock body; body.label = "body";
    body.instrs = {load(4, "total"), load(5, "i"), add(6, 4, 5), store(6, "total"), jump("loop")};

    BasicBlock exit_b; exit_b.label = "exit";
    Instr ret; ret.op = Op::Return; ret.result = kInvalidValue;
    exit_b.instrs = {ret};

    fn.blocks = {entry, header, body, exit_b};
    return fn;
}

int main() {
    Function fn;
    fn.name = "main";

    BasicBlock entry;
    entry.label = "block0";
    {
        Instr i0; i0.op = Op::ConstInt; i0.result = 0; i0.int_imm = 0;
        entry.instrs.push_back(i0);
        Instr i1; i1.op = Op::Store; i1.result = kInvalidValue; i1.args = {0}; i1.name = "i";
        entry.instrs.push_back(i1);
        Instr i2; i2.op = Op::ConstInt; i2.result = 1; i2.int_imm = 0;
        entry.instrs.push_back(i2);
        Instr i3; i3.op = Op::Store; i3.result = kInvalidValue; i3.args = {1}; i3.name = "total";
        entry.instrs.push_back(i3);
        Instr i4; i4.op = Op::Jump; i4.result = kInvalidValue; i4.name = "block1";
        entry.instrs.push_back(i4);
    }

    BasicBlock header;
    header.label = "block1";
    {
        Instr i2; i2.op = Op::Load; i2.result = 2; i2.name = "i";
        header.instrs.push_back(i2);
        Instr i3; i3.op = Op::ConstInt; i3.result = 3; i3.int_imm = 10;
        header.instrs.push_back(i3);
        Instr i4; i4.op = Op::Lt; i4.result = 4; i4.args = {2, 3};
        header.instrs.push_back(i4);
        Instr i5; i5.op = Op::Branch; i5.result = kInvalidValue; i5.args = {4}; i5.name = "block2,block3";
        header.instrs.push_back(i5);
    }

    BasicBlock body;
    body.label = "block2";
    {
        Instr i5; i5.op = Op::Load; i5.result = 5; i5.name = "total";
        body.instrs.push_back(i5);
        Instr i6; i6.op = Op::Load; i6.result = 6; i6.name = "i";
        body.instrs.push_back(i6);
        Instr i7; i7.op = Op::Add; i7.result = 7; i7.args = {5, 6};
        body.instrs.push_back(i7);
        Instr i8; i8.op = Op::Store; i8.result = kInvalidValue; i8.args = {7}; i8.name = "total";
        body.instrs.push_back(i8);
        Instr i9; i9.op = Op::Load; i9.result = 8; i9.name = "i";
        body.instrs.push_back(i9);
        Instr i10; i10.op = Op::ConstInt; i10.result = 9; i10.int_imm = 1;
        body.instrs.push_back(i10);
        Instr i11; i11.op = Op::Add; i11.result = 10; i11.args = {8, 9};
        body.instrs.push_back(i11);
        Instr i12; i12.op = Op::Store; i12.result = kInvalidValue; i12.args = {10}; i12.name = "i";
        body.instrs.push_back(i12);
        Instr i13; i13.op = Op::Jump; i13.result = kInvalidValue; i13.name = "block1";
        body.instrs.push_back(i13);
    }

    BasicBlock exit_block;
    exit_block.label = "block3";
    {
        Instr i11; i11.op = Op::Load; i11.result = 11; i11.name = "total";
        exit_block.instrs.push_back(i11);
        Instr i12; i12.op = Op::Call; i12.result = kInvalidValue; i12.args = {11}; i12.name = "print";
        exit_block.instrs.push_back(i12);
        Instr i13; i13.op = Op::Return; i13.result = kInvalidValue;
        exit_block.instrs.push_back(i13);
    }

    fn.blocks = {entry, header, body, exit_block};

    LivenessAnalysis liveness(fn);

    printf("total instructions: %d\n", liveness.instruction_count());

    std::vector<ValueId> ids;
    for (const auto& kv : liveness.ranges()) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    for (auto id : ids) {
        const auto& r = liveness.ranges().at(id);
        printf("%%%u: birth=%d last_use=%d\n", id, r.birth, r.last_use);
    }

    bool ok = true;

    auto it1 = liveness.ranges().find(1);
    if (it1 == liveness.ranges().end()) {
        printf("FAIL: %%1 missing entirely\n");
        ok = false;
    } else if (it1->second.last_use != 3) {
        printf("FAIL: %%1 last_use=%d, expected 3 (dead after its own store)\n",
               it1->second.last_use);
        ok = false;
    } else {
        printf("OK: %%1 correctly dead after its own store (last_use=3)\n");
    }

    auto it2 = liveness.ranges().find(2);
    if (it2 == liveness.ranges().end()) {
        printf("FAIL: %%2 missing\n");
        ok = false;
    } else if (it2->second.last_use != 7) {
        printf("FAIL: %%2 last_use=%d, expected 7 (must stay loop-local)\n",
               it2->second.last_use);
        ok = false;
    } else {
        printf("OK: %%2 correctly stays loop-local (last_use=7)\n");
    }

    // ---- virtual temps are excluded BEFORE ranges are computed ----
    {
        Function fn = loop_reading_preloop_const();

        // No exclusion list: %0 (read every iteration) is dragged to the
        // loop's back edge at flat index 12, and %3 (the fused compare) is
        // live to its branch. This is the pressure the fix removes.
        LivenessAnalysis plain(fn);
        bool dragged = plain.ranges().count(0) && plain.ranges().at(0).last_use == 12;
        printf(dragged ? "OK: without the exclusion list, %%0 is dragged to the back edge (12)\n"
                       : "FAIL: %%0 was not dragged to the back edge, so this test proves nothing\n");
        ok = ok && dragged;

        // plan_function's set: %0 is Const (emitted as an immediate) and
        // %3 is FusedCmp (folded into the branch that consumes it).
        VirtualTemps virtuals = {0, 3};
        LivenessAnalysis filtered(fn, virtuals);

        for (ValueId id : virtuals) {
            bool absent = filtered.ranges().count(id) == 0;
            if (absent) {
                printf("OK: %%%u gets no live range at all\n", id);
            } else {
                const LiveRange& r = filtered.ranges().at(id);
                printf("FAIL: %%%u still has a range (birth=%d last_use=%d)\n", id, r.birth, r.last_use);
            }
            ok = ok && absent;
        }

        // The exclusion must not perturb any real value's range.
        bool others_unchanged = plain.ranges().count(6) && filtered.ranges().count(6) &&
                                plain.ranges().at(6).last_use == filtered.ranges().at(6).last_use &&
                                plain.ranges().at(6).birth == filtered.ranges().at(6).birth;
        printf(others_unchanged ? "OK: real values keep identical ranges\n"
                                : "FAIL: excluding virtual temps changed a real value's range\n");
        ok = ok && others_unchanged;
    }

    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
