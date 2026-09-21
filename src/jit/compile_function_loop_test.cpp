// Proves the backward-jump path in compile_module's two-pass patch
// resolution actually works. Compiles a real loop:
//
//     def sum_to_n(n: int[64]) -> int[64]:
//         total = 0
//         i = 0
//         while i < n:
//             total = total + i
//             i = i + 1
//         return total

#include "compile_function.h"
#include "ir/ir.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>

using namespace lithon::ir;
using namespace lithon::jit;

typedef int64_t (*SumFunc)(int64_t);

int main() {
    Function fn;
    fn.name = "sum_to_n";
    fn.params = {"n"};

    BasicBlock entry;
    entry.label = "block0";
    { Instr i; i.op = Op::ConstInt; i.result = 0; i.int_imm = 0; entry.instrs.push_back(i); }
    { Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {0}; i.name = "total"; entry.instrs.push_back(i); }
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
    { Instr i; i.op = Op::Load; i.result = 5; i.name = "total"; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Load; i.result = 6; i.name = "i"; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Add; i.result = 7; i.args = {5, 6}; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {7}; i.name = "total"; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Load; i.result = 8; i.name = "i"; body.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 9; i.int_imm = 1; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Add; i.result = 10; i.args = {8, 9}; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Store; i.result = kInvalidValue; i.args = {10}; i.name = "i"; body.instrs.push_back(i); }
    { Instr i; i.op = Op::Jump; i.result = kInvalidValue; i.name = "block1"; body.instrs.push_back(i); }

    BasicBlock exit_block;
    exit_block.label = "block3";
    { Instr i; i.op = Op::Load; i.result = 11; i.name = "total"; exit_block.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {11}; exit_block.instrs.push_back(i); }

    fn.blocks = {entry, header, body, exit_block};

    Module module;
    module.functions = {fn};
    CompiledModule compiled = compile_module(module);

    std::printf("compiled %zu bytes:", compiled.code.size());
    for (auto b : compiled.code) std::printf(" %02x", b);
    std::printf("\n");

    void* mem = mmap(nullptr, compiled.code.size(), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { std::perror("mmap"); return 1; }

    std::memcpy(mem, compiled.code.data(), compiled.code.size());

    if (mprotect(mem, compiled.code.size(), PROT_READ | PROT_EXEC) != 0) {
        std::perror("mprotect");
        return 1;
    }

    size_t sum_offset = compiled.function_offset.at("sum_to_n");
    SumFunc compiled_sum = reinterpret_cast<SumFunc>(
        reinterpret_cast<uint8_t*>(mem) + sum_offset);

    int64_t r1 = compiled_sum(5);
    int64_t r2 = compiled_sum(0);
    int64_t r3 = compiled_sum(1);
    int64_t r4 = compiled_sum(10);

    std::printf("compiled sum_to_n(5) = %lld (expect 10)\n", (long long)r1);
    std::printf("compiled sum_to_n(0) = %lld (expect 0)\n", (long long)r2);
    std::printf("compiled sum_to_n(1) = %lld (expect 0)\n", (long long)r3);
    std::printf("compiled sum_to_n(10) = %lld (expect 45)\n", (long long)r4);

    if (r1 != 10 || r2 != 0 || r3 != 0 || r4 != 45) {
        std::fprintf(stderr, "FAIL\n");
        return 1;
    }
    std::printf("PASS: backward jump (loop) compiled and executed correctly\n");
    return 0;
}
