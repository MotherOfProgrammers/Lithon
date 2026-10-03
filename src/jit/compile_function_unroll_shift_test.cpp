// Regression test for unrolling a loop whose body contains a dynamic shift.
//
// Shl/Shr used to be excluded from the straight-line unroller because it
// shared a predicate (is_pure_op) with hoisting/DCE, and a trapping op must
// not be MOVED. But duplication is not movement: the unroller emits each
// iteration's body exactly once with the loop test inlined before every
// copy, so the sequence of executed shift instructions -- and therefore
// which one traps first -- is unchanged. This test pins that reasoning
// down end to end.
//
// The body is the shape the frontend produces for `acc += 1 << k`:
//
//     block0:  acc = 0; i = 0;                    jump  H
//     H:       i < n -> B, exit
//     B:       acc = acc + (1 << k); i = i + 1;   jump  H        (11 instrs)
//     exit:    return acc
//
// The count `k` comes from a Load, so codegen takes the dynamic path
// (`shl dst, cl`), which is the path that exercises the scratch register and
// the RCX-destination hazard. Three things are easy to get wrong:
//
//   1. The unroller has to have actually fired -- checked by compiling with
//      unroll_factor 1 vs 4 and requiring the code to grow. Without this,
//      the trip-count checks below would also pass against a plain
//      (non-unrolled) body and prove nothing about the split.
//   2. Trip counts that are not a multiple of the unroll factor (especially
//      5, one past a factor of 4) must stay exact.
//   3. A zero-trip loop must NOT execute the shift at all. `shift_sum(0, 99)`
//      must return 0 rather than trap on the out-of-range count; that is the
//      property that distinguishes duplication (safe) from hoisting (unsafe).

#include "compile_function.h"
#include "ir/ir.h"
#include "exec_memory.h"
#include <cstdio>

using namespace lithon::ir;
using namespace lithon::jit;

typedef int64_t (*ShiftFunc)(int64_t, int64_t);

int main() {
    Function fn;
    fn.name = "shift_sum";
    fn.params = {"n", "k"};

    BasicBlock entry;
    entry.label = "block0";
    { Instr i; i.op = Op::ConstInt; i.result = 0; i.int_imm = 0; entry.instrs.push_back(i); }
    { Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {0}; i.name = "acc"; entry.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 1; i.int_imm = 0; entry.instrs.push_back(i); }
    { Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {1}; i.name = "i"; entry.instrs.push_back(i); }
    { Instr i; i.op = Op::Jump; i.result = kInvalidValue; i.name = "block1"; entry.instrs.push_back(i); }

    BasicBlock header;
    header.label = "block1";
    { Instr i; i.op = Op::Load; i.result = 2; i.name = "i"; header.instrs.push_back(i); }
    { Instr i; i.op = Op::Load; i.result = 3; i.name = "n"; header.instrs.push_back(i); }
    { Instr i; i.op = Op::Lt; i.result = 4; i.args = {2, 3}; header.instrs.push_back(i); }
    { Instr i; i.op = Op::Branch; i.result = kInvalidValue; i.args = {4};
      i.name = "block2,block3"; header.instrs.push_back(i); }

    BasicBlock body;
    body.label = "block2";
    { Instr i; i.op = Op::Load; i.result = 5; i.name = "acc"; body.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 6; i.int_imm = 1; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Load; i.result = 7; i.name = "k"; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Shl; i.result = 8; i.args = {6, 7}; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Add; i.result = 9; i.args = {5, 8}; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {9}; i.name = "acc"; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Load; i.result = 10; i.name = "i"; body.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 11; i.int_imm = 1; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Add; i.result = 12; i.args = {10, 11}; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {12}; i.name = "i"; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Jump; i.result = kInvalidValue; i.name = "block1"; body.instrs.push_back(i); }

    BasicBlock exit_block;
    exit_block.label = "block3";
    { Instr i; i.op = Op::Load; i.result = 13; i.name = "acc"; exit_block.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {13}; exit_block.instrs.push_back(i); }

    fn.blocks = {entry, header, body, exit_block};

    Module module;
    module.functions = {fn};

    // The unroller must have fired, or the trip-count checks below would pass
    // against a plain body. Compare unroll off (1 copy) against the default
    // factor of 4: duplicating a body that contains a shift must grow the code.
    CompileOptions off;
    off.unroll_factor = 1;
    CompileOptions on;   // default unroll_factor = 4
    const size_t size_off = compile_module(module, off).code.size();
    const size_t size_on = compile_module(module, on).code.size();
    std::printf("code size: unroll_factor=1 %zu, =4 %zu\n", size_off, size_on);
    if (size_on <= size_off) {
        std::fprintf(stderr, "FAIL: shift body did not unroll (code did not grow)\n");
        return 1;
    }

    CompiledModule compiled = compile_module(module, on);
    ExecutableBuffer exec_mem(compiled.code);
    ShiftFunc fnptr = reinterpret_cast<ShiftFunc>(
        reinterpret_cast<uint8_t*>(exec_mem.data()) + compiled.function_offset.at("shift_sum"));

    // acc = n * (1 << k). Counts straddling the unroll factor boundary (4) are
    // the interesting ones; 5 is the first count that needs the remainder.
    const int64_t counts[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 1000, 1001};
    const int64_t shifts[] = {0, 1, 5, 31, 62};
    int failures = 0;
    for (int64_t k : shifts) {
        const int64_t unit = static_cast<int64_t>(static_cast<uint64_t>(1) << k);
        for (int64_t n : counts) {
            const int64_t want = n * unit;
            const int64_t got = fnptr(n, k);
            if (got != want) {
                std::fprintf(stderr, "FAIL: shift_sum(%lld, %lld) = %lld, expected %lld\n",
                             (long long)n, (long long)k, (long long)got, (long long)want);
                ++failures;
            }
        }
    }
    std::printf("checked %zu trip counts x %zu shift counts\n",
                sizeof(counts) / sizeof(counts[0]), sizeof(shifts) / sizeof(shifts[0]));

    // The whole point of the split: zero iterations must not execute the
    // shift, so an out-of-range count in a loop that never runs is not a trap.
    const int64_t zero = fnptr(0, 99);
    std::printf("shift_sum(0, 99) = %lld (expect 0, no trap)\n", (long long)zero);
    if (zero != 0) {
        std::fprintf(stderr, "FAIL: zero-trip loop with bad shift count returned %lld\n",
                     (long long)zero);
        ++failures;
    }

    if (failures) return 1;
    std::printf("PASS: shift-containing loop unrolls with exact trip counts and no spurious trap\n");
    return 0;
}
