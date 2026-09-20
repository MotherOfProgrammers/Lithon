// Verifies liveness computation against a hand-built IR shaped like
// tests/typed_regression/while.py:
//
//   i = 0            (%0)
//   total = 0        (%1)
//   header: %2=load i, %3=const 10, %4=lt %2,%3, branch %4, body, exit
//   body: %5=load total, %6=load i, %7=add %5,%6, store total,
//         %8=load i, %9=const 1, %10=add %8,%9, store i, jump header
//   exit: %11=load total, call print, return
//
// Confirms the CORRECT model for this IR: raw %N values are never
// loop-carried at the value-id level (every Load produces a fresh
// %N; loop-carrying happens through named Store/Load), so plain
// non-loop-aware liveness is correct here -- an earlier attempt to
// extend %N ranges across back-edges was solving a problem that
// doesn't exist in this IR, and incorrectly broke loop-local values
// like %2. This test locks in the correct expectations.

#include "liveness.h"
#include "ir/ir.h"
#include <cstdio>
#include <algorithm>
#include <vector>

using namespace lithon::ir;
using namespace lithon::jit;

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

    // %1 (total's init literal) is genuinely dead after its own
    // Store -- it is NOT loop-carried at the value-id level, because
    // "total" is carried via named Store/Load, and every Load
    // produces a fresh %N. This is correct, not a gap.
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

    // %2 (load i inside the header, used only by the immediate Lt
    // check) must stay LOCAL -- last_use=7, not extended across the
    // loop. This is the exact case the earlier flawed design broke.
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

    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
