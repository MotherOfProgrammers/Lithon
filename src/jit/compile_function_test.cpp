// The real, first end-to-end proof: compiles a typed ir::Function --
// the exact shape of tests/typed_regression/function.py's
// add(a: int[64], b: int[64]) -> int[64]: return a + b -- to genuine
// x86-64 machine code via compile_module, executes it, and compares
// against the known-correct result the interpreter already produces
// for the same program (7 for add(3, 4)).

#include "compile_function.h"
#include "ir/ir.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>

using namespace lithon::ir;
using namespace lithon::jit;

typedef int64_t (*AddFunc)(int64_t, int64_t);

int main() {
    Function fn;
    fn.name = "add";
    fn.params = {"a", "b"};

    BasicBlock block0;
    block0.label = "block0";
    { Instr i; i.op = Op::Load; i.result = 0; i.name = "a"; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Load; i.result = 1; i.name = "b"; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Add; i.result = 2; i.args = {0, 1}; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {2}; block0.instrs.push_back(i); }
    fn.blocks = {block0};

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

    size_t add_offset = compiled.function_offset.at("add");
    AddFunc compiled_add = reinterpret_cast<AddFunc>(
        reinterpret_cast<uint8_t*>(mem) + add_offset);

    int64_t r1 = compiled_add(3, 4);
    int64_t r2 = compiled_add(100, 200);
    int64_t r3 = compiled_add(-5, 5);

    std::printf("compiled add(3, 4) = %lld (expect 7, matches interpreter)\n", (long long)r1);
    std::printf("compiled add(100, 200) = %lld (expect 300)\n", (long long)r2);
    std::printf("compiled add(-5, 5) = %lld (expect 0)\n", (long long)r3);

    if (r1 != 7 || r2 != 300 || r3 != 0) {
        std::fprintf(stderr, "FAIL\n");
        return 1;
    }
    std::printf("PASS: real ir::Function compiled to genuine native machine code and executed correctly\n");
    return 0;
}
