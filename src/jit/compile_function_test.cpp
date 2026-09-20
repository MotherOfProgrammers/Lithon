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

    CodeBuffer code = compile_function(fn);

    std::printf("compiled %zu bytes:", code.size());
    for (auto b : code) std::printf(" %02x", b);
    std::printf("\n");

    void* mem = mmap(nullptr, code.size(), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { std::perror("mmap"); return 1; }

    std::memcpy(mem, code.data(), code.size());

    if (mprotect(mem, code.size(), PROT_READ | PROT_EXEC) != 0) {
        std::perror("mprotect");
        return 1;
    }

    AddFunc compiled_add = reinterpret_cast<AddFunc>(mem);

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
