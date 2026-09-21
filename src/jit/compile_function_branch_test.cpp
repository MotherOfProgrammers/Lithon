// Proves multi-block control flow and comparison codegen work
// together via the real compile_module bridge. Compiles a real
// ir::Function equivalent to:
//
//     int64_t max(int64_t a, int64_t b) {
//         if (a > b) return a;
//         else return b;
//     }

#include "compile_function.h"
#include "ir/ir.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>

using namespace lithon::ir;
using namespace lithon::jit;

typedef int64_t (*MaxFunc)(int64_t, int64_t);

int main() {
    Function fn;
    fn.name = "max";
    fn.params = {"a", "b"};

    BasicBlock block0;
    block0.label = "block0";
    { Instr i; i.op = Op::Load; i.result = 0; i.name = "a"; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Load; i.result = 1; i.name = "b"; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Gt; i.result = 2; i.args = {0, 1}; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Branch; i.result = kInvalidValue; i.args = {2};
      i.name = "block1,block2"; block0.instrs.push_back(i); }

    BasicBlock then_block;
    then_block.label = "block1";
    { Instr i; i.op = Op::Load; i.result = 3; i.name = "a"; then_block.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {3}; then_block.instrs.push_back(i); }

    BasicBlock else_block;
    else_block.label = "block2";
    { Instr i; i.op = Op::Load; i.result = 4; i.name = "b"; else_block.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {4}; else_block.instrs.push_back(i); }

    fn.blocks = {block0, then_block, else_block};

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

    size_t max_offset = compiled.function_offset.at("max");
    MaxFunc compiled_max = reinterpret_cast<MaxFunc>(
        reinterpret_cast<uint8_t*>(mem) + max_offset);

    int64_t r1 = compiled_max(3, 7);
    int64_t r2 = compiled_max(10, 2);
    int64_t r3 = compiled_max(5, 5);

    std::printf("compiled max(3, 7) = %lld (expect 7)\n", (long long)r1);
    std::printf("compiled max(10, 2) = %lld (expect 10)\n", (long long)r2);
    std::printf("compiled max(5, 5) = %lld (expect 5)\n", (long long)r3);

    if (r1 != 7 || r2 != 10 || r3 != 5) {
        std::fprintf(stderr, "FAIL\n");
        return 1;
    }
    std::printf("PASS: real ir::Function with control flow + comparisons compiled and executed correctly\n");
    return 0;
}
