// Proves stack-relative addressing and prologue/epilogue work
// correctly together. Compiles, by hand:
//
//     int64_t sum3(int64_t a, int64_t b, int64_t c) {
//         int64_t tmp = a + b;   // spilled to [rbp-8]
//         return tmp + c;         // reloaded from [rbp-8]
//     }
//
// deliberately spilling `tmp` to the stack (rather than keeping it
// in a register) to exercise emit_store_rbp_offset /
// emit_load_rbp_offset and the prologue/epilogue together -- this is
// exactly the shape a real spilled value takes once the register
// allocator runs out of physical registers.

#include "x86_encoder.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>

using namespace lithon::jit;

typedef int64_t (*Sum3Func)(int64_t, int64_t, int64_t);

int main() {
    CodeBuffer code;

    // System V AMD64: a=rdi, b=rsi, c=rdx, return=rax.

    emit_prologue(code, 16);

    emit_mov_reg_reg(code, Reg::RAX, Reg::RDI);   // rax = a
    emit_add_reg_reg(code, Reg::RAX, Reg::RSI);   // rax = a + b
    emit_store_rbp_offset(code, Reg::RAX, -8);    // spill: [rbp-8] = tmp

    emit_load_rbp_offset(code, Reg::RCX, -8);     // reload: rcx = tmp
    emit_add_reg_reg(code, Reg::RCX, Reg::RDX);   // rcx = tmp + c
    emit_mov_reg_reg(code, Reg::RAX, Reg::RCX);   // rax = result

    emit_epilogue(code);
    emit_ret(code);

    std::printf("encoded %zu bytes:", code.size());
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

    Sum3Func fn = reinterpret_cast<Sum3Func>(mem);

    int64_t r = fn(3, 4, 5);
    std::printf("sum3(3, 4, 5) = %lld (expect 12)\n", (long long)r);

    if (r != 12) {
        std::fprintf(stderr, "FAIL\n");
        return 1;
    }
    std::printf("PASS: stack frame + spill/reload confirmed working\n");
    return 0;
}
