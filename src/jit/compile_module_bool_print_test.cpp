// Verifies And/Or/Not codegen (matching interpreter VALUE semantics
// exactly) and print() calling real libc printf from JIT'd code.
//
//     def main():
//         a = 5   # any nonzero -> truthy
//         b = 0   # falsy
//         print(a and b)   # expect 0 (b, since a is truthy)
//         print(a or b)     # expect 5 (a, since a is truthy)
//         print(not a)      # expect 0 (a is truthy -> not a is falsy/0)
//         print(not b)      # expect 1 (b is falsy -> not b is truthy/1)
//         return

#include "compile_function.h"
#include "ir/ir.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>

using namespace lithon::ir;
using namespace lithon::jit;

typedef void (*VoidFunc)();

int main() {
    Module module;
    Function main_fn;
    main_fn.name = "main";

    BasicBlock b0;
    b0.label = "block0";
    { Instr i; i.op = Op::ConstInt; i.result = 0; i.int_imm = 5; b0.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 1; i.int_imm = 0; b0.instrs.push_back(i); }
    { Instr i; i.op = Op::And; i.result = 2; i.args = {0, 1}; b0.instrs.push_back(i); }
    { Instr i; i.op = Op::Call; i.result = kInvalidValue; i.args = {2}; i.name = "print"; b0.instrs.push_back(i); }
    { Instr i; i.op = Op::Or; i.result = 3; i.args = {0, 1}; b0.instrs.push_back(i); }
    { Instr i; i.op = Op::Call; i.result = kInvalidValue; i.args = {3}; i.name = "print"; b0.instrs.push_back(i); }
    { Instr i; i.op = Op::Not; i.result = 4; i.args = {0}; b0.instrs.push_back(i); }
    { Instr i; i.op = Op::Call; i.result = kInvalidValue; i.args = {4}; i.name = "print"; b0.instrs.push_back(i); }
    { Instr i; i.op = Op::Not; i.result = 5; i.args = {1}; b0.instrs.push_back(i); }
    { Instr i; i.op = Op::Call; i.result = kInvalidValue; i.args = {5}; i.name = "print"; b0.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; b0.instrs.push_back(i); }

    main_fn.blocks = {b0};
    module.functions = {main_fn};

    CompiledModule compiled = compile_module(module);

    void* mem = mmap(nullptr, compiled.code.size(), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { std::perror("mmap"); return 1; }
    std::memcpy(mem, compiled.code.data(), compiled.code.size());
    if (mprotect(mem, compiled.code.size(), PROT_READ | PROT_EXEC) != 0) {
        std::perror("mprotect");
        return 1;
    }

    VoidFunc compiled_main = reinterpret_cast<VoidFunc>(
        reinterpret_cast<uint8_t*>(mem) + compiled.function_offset.at("main"));

    printf("--- native output (expect 0, 5, 0, 1) ---\n");
    compiled_main();
    printf("--- if you see the four numbers above, PASS ---\n");
    return 0;
}
