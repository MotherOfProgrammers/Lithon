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

    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
