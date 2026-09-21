// The real proof this design exists for: recursion, where a value
// (fib(n-1)'s result) must survive across a SECOND call (fib(n-2))
// without being clobbered -- exactly the case that requires the
// register allocator's call-spanning spill rule (register_alloc.h)
// to be correct, not just present.
//
//     def fib(n: int[64]) -> int[64]:
//         if n < 2:
//             return n
//         return fib(n - 1) + fib(n - 2)

#include "compile_function.h"
#include "ir/ir.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>

using namespace lithon::ir;
using namespace lithon::jit;

typedef int64_t (*FibFunc)(int64_t);

int main() {
    Module module;

    Function fib_fn;
    fib_fn.name = "fib";
    fib_fn.params = {"n"};

    BasicBlock block0;
    block0.label = "block0";
    { Instr i; i.op = Op::Load; i.result = 0; i.name = "n"; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 1; i.int_imm = 2; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Lt; i.result = 2; i.args = {0, 1}; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Branch; i.result = kInvalidValue; i.args = {2};
      i.name = "block1,block2"; block0.instrs.push_back(i); }

    BasicBlock block1;
    block1.label = "block1";
    { Instr i; i.op = Op::Load; i.result = 3; i.name = "n"; block1.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {3}; block1.instrs.push_back(i); }

    BasicBlock block2;
    block2.label = "block2";
    { Instr i; i.op = Op::Load; i.result = 4; i.name = "n"; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 5; i.int_imm = 1; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Sub; i.result = 6; i.args = {4, 5}; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Call; i.result = 7; i.args = {6}; i.name = "fib"; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Load; i.result = 8; i.name = "n"; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 9; i.int_imm = 2; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Sub; i.result = 10; i.args = {8, 9}; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Call; i.result = 11; i.args = {10}; i.name = "fib"; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Add; i.result = 12; i.args = {7, 11}; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {12}; block2.instrs.push_back(i); }

    fib_fn.blocks = {block0, block1, block2};
    module.functions = {fib_fn};

    CompiledModule compiled = compile_module(module);

    std::printf("compiled %zu bytes\n", compiled.code.size());

    void* mem = mmap(nullptr, compiled.code.size(), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { std::perror("mmap"); return 1; }

    std::memcpy(mem, compiled.code.data(), compiled.code.size());

    if (mprotect(mem, compiled.code.size(), PROT_READ | PROT_EXEC) != 0) {
        std::perror("mprotect");
        return 1;
    }

    size_t fib_offset = compiled.function_offset.at("fib");
    FibFunc compiled_fib = reinterpret_cast<FibFunc>(
        reinterpret_cast<uint8_t*>(mem) + fib_offset);

    int64_t expected[] = {0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55};
    bool ok = true;
    for (int n = 0; n <= 10; ++n) {
        int64_t r = compiled_fib(n);
        std::printf("compiled fib(%d) = %lld (expect %lld)\n", n, (long long)r, (long long)expected[n]);
        if (r != expected[n]) ok = false;
    }

    std::printf(ok ? "PASS: recursive function compiled and executed correctly\n" : "FAIL\n");
    return ok ? 0 : 1;
}
