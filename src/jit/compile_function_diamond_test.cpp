// Regression test for the aggressive diamond unroller in compile_module.
//
// Compiles the loop shape it claims to handle:
//
//     block0:  t = 0; i = 0;                      jump  H
//     H:       i < n -> D, exit
//     D:       i < k -> A, E                      (the diamond)
//     A:       t = t + 1;                          jump  B
//     E:       t = t + 2;                          jump  B
//     B:       i = i + 1;                          jump  H
//     exit:    return t
//
// The unroller inlines D, A, E and B U times, duplicates the latch and the
// exit test in every copy, and closes the loop with a jump back to the entry
// test. Two things are easy to get wrong and are checked here: the trip count
// must stay exact for counts that are not a multiple of U (including 0 and 1,
// which is what the entry test exists for), and the diamond must not run its
// arms twice for the same iteration.

#include "compile_function.h"
#include "ir/ir.h"
#include "exec_memory.h"
#include <cstdio>

using namespace lithon::ir;
using namespace lithon::jit;

typedef int64_t (*DiamondFunc)(int64_t, int64_t);

int main() {
    Function fn;
    fn.name = "diamond_sum";
    fn.params = {"n", "k"};

    BasicBlock entry;
    entry.label = "block0";
    { Instr i; i.op = Op::ConstInt; i.result = 0; i.int_imm = 0; entry.instrs.push_back(i); }
    { Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {0}; i.name = "t"; entry.instrs.push_back(i); }
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

    BasicBlock diamond;
    diamond.label = "block2";
    { Instr i; i.op = Op::Load; i.result = 5; i.name = "i"; diamond.instrs.push_back(i); }
    { Instr i; i.op = Op::Load; i.result = 6; i.name = "k"; diamond.instrs.push_back(i); }
    { Instr i; i.op = Op::Lt; i.result = 7; i.args = {5, 6}; diamond.instrs.push_back(i); }
    { Instr i; i.op = Op::Branch; i.result = kInvalidValue; i.args = {7};
      i.name = "block4,block5"; diamond.instrs.push_back(i); }

    BasicBlock arm_then;
    arm_then.label = "block4";
    { Instr i; i.op = Op::Load; i.result = 8; i.name = "t"; arm_then.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 9; i.int_imm = 1; arm_then.instrs.push_back(i); }
    { Instr i; i.op = Op::Add; i.result = 10; i.args = {8, 9}; arm_then.instrs.push_back(i); }
    { Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {10}; i.name = "t"; arm_then.instrs.push_back(i); }
    { Instr i; i.op = Op::Jump; i.result = kInvalidValue; i.name = "block6"; arm_then.instrs.push_back(i); }

    BasicBlock arm_else;
    arm_else.label = "block5";
    { Instr i; i.op = Op::Load; i.result = 11; i.name = "t"; arm_else.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 12; i.int_imm = 2; arm_else.instrs.push_back(i); }
    { Instr i; i.op = Op::Add; i.result = 13; i.args = {11, 12}; arm_else.instrs.push_back(i); }
    { Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {13}; i.name = "t"; arm_else.instrs.push_back(i); }
    { Instr i; i.op = Op::Jump; i.result = kInvalidValue; i.name = "block6"; arm_else.instrs.push_back(i); }

    BasicBlock latch;
    latch.label = "block6";
    { Instr i; i.op = Op::Load; i.result = 14; i.name = "i"; latch.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 15; i.int_imm = 1; latch.instrs.push_back(i); }
    { Instr i; i.op = Op::Add; i.result = 16; i.args = {14, 15}; latch.instrs.push_back(i); }
    { Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {16}; i.name = "i"; latch.instrs.push_back(i); }
    { Instr i; i.op = Op::Jump; i.result = kInvalidValue; i.name = "block1"; latch.instrs.push_back(i); }

    BasicBlock exit_block;
    exit_block.label = "block3";
    { Instr i; i.op = Op::Load; i.result = 17; i.name = "t"; exit_block.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {17}; exit_block.instrs.push_back(i); }

    fn.blocks = {entry, header, diamond, arm_then, arm_else, latch, exit_block};

    Module module;
    module.functions = {fn};

    // Off by default: unrolling a diamond cannot remove its irreducible
    // if/else test, and on Sandy Bridge it measured slower than not unrolling
    // (see CompileOptions::unroll_diamonds). Straight-line unrolling is the
    // variant that pays, so the default build must leave this off.
    const size_t size_default = compile_module(module).code.size();
    CompileOptions off;
    off.unroll_diamonds = false;
    const size_t size_off = compile_module(module, off).code.size();
    if (size_default != size_off) {
        std::fprintf(stderr, "FAIL: unroll_diamonds defaults to on (%zu != %zu bytes)\n",
                     size_default, size_off);
        return 1;
    }

    // Opted in, the unroller has to have actually fired, or the checks below
    // would pass against the plain body unroller and prove nothing.
    CompileOptions on;
    on.unroll_diamonds = true;
    const size_t size_on = compile_module(module, on).code.size();
    std::printf("code size: unroll_diamonds=off %zu, on %zu\n", size_off, size_on);
    if (size_on <= size_off) {
        std::fprintf(stderr, "FAIL: diamond unroll did not grow the code, so it did not fire\n");
        return 1;
    }

    CompiledModule compiled = compile_module(module, on);
    ExecutableBuffer exec_mem(compiled.code);
    DiamondFunc fnptr = reinterpret_cast<DiamondFunc>(
        reinterpret_cast<uint8_t*>(exec_mem.data()) + compiled.function_offset.at("diamond_sum"));

    // t = sum over i in [0, n) of (i < k ? 1 : 2) = k + 2 * (n - k).
    // Counts straddling the unroll factor boundary are the interesting ones:
    // 0 and 1 need the entry test, the rest need every copy's test.
    const int64_t counts[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 1000, 1001};
    int failures = 0;
    for (int64_t n : counts) {
        const int64_t k = n / 2;
        const int64_t want = k + 2 * (n - k);
        const int64_t got = fnptr(n, k);
        std::printf("diamond_sum(%lld, %lld) = %lld (expect %lld)\n",
                    (long long)n, (long long)k, (long long)got, (long long)want);
        if (got != want) {
            std::fprintf(stderr, "FAIL: diamond_sum(%lld, %lld) = %lld, expected %lld\n",
                         (long long)n, (long long)k, (long long)got, (long long)want);
            ++failures;
        }
    }

    if (failures) return 1;
    std::printf("PASS: aggressive diamond unroll is exact for every trip count\n");
    return 0;
}
