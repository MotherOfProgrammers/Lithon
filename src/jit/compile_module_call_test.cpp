// First proof of function calls between compiled functions, kept
// deliberately non-recursive to isolate the call mechanism itself
// before attempting full recursion (fib). Compiles:
//
//     def add(a: int[64], b: int[64]) -> int[64]:
//         return a + b
//
//     def main() -> int[64]:
//         x = add(3, 4)
//         return x

#include "compile_function.h"
#include "ir/ir.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>

using namespace lithon::ir;
using namespace lithon::jit;

typedef int64_t (*MainFunc)();

int main() {
    Module module;

    Function add_fn;
    add_fn.name = "add";
    add_fn.params = {"a", "b"};
    BasicBlock add_block;
    add_block.label = "block0";
    { Instr i; i.op = Op::Load; i.result = 0; i.name = "a"; add_block.instrs.push_back(i); }
    { Instr i; i.op = Op::Load; i.result = 1; i.name = "b"; add_block.instrs.push_back(i); }
    { Instr i; i.op = Op::Add; i.result = 2; i.args = {0, 1}; add_block.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {2}; add_block.instrs.push_back(i); }
    add_fn.blocks = {add_block};

    Function main_fn;
    main_fn.name = "main";
    BasicBlock main_block;
    main_block.label = "block0";
    { Instr i; i.op = Op::ConstInt; i.result = 0; i.int_imm = 3; main_block.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 1; i.int_imm = 4; main_block.instrs.push_back(i); }
    { Instr i; i.op = Op::Call; i.result = 2; i.args = {0, 1}; i.name = "add"; main_block.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {2}; main_block.instrs.push_back(i); }
    main_fn.blocks = {main_block};

    module.functions = {add_fn, main_fn};

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

    size_t main_offset = compiled.function_offset.at("main");
    MainFunc compiled_main = reinterpret_cast<MainFunc>(
        reinterpret_cast<uint8_t*>(mem) + main_offset);

    int64_t r = compiled_main();
    std::printf("compiled main() -> add(3,4) = %lld (expect 7)\n", (long long)r);

    if (r != 7) {
        std::fprintf(stderr, "FAIL\n");
        return 1;
    }
    std::printf("PASS: function call between two compiled functions works\n");
    return 0;
}
