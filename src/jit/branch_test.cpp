// Proves the jump-patching mechanism works: compiles
//
//     int64_t max(int64_t a, int64_t b) {
//         if (a > b) return a;
//         else return b;
//     }
//
// by hand, using cmp + jg (conditional jump) with a forward patch
// resolved once the target block's position is known -- the first
// real control-flow test of the zero-dependency encoder.

#include "x86_encoder.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>

using namespace lithon::jit;

typedef int64_t (*MaxFunc)(int64_t, int64_t);

int main() {
    CodeBuffer code;

    // System V AMD64: a in rdi, b in rsi, return in rax.
    //
    //   cmp rdi, rsi
    //   jg  then_block          ; a > b -> jump to "return a"
    //   mov rax, rsi            ; else: rax = b
    //   ret
    // then_block:
    //   mov rax, rdi            ; rax = a
    //   ret

    emit_cmp_reg_reg(code, Reg::RDI, Reg::RSI);
    JumpPatch to_then = emit_jcc_rel32(code, Cond::Greater);

    // else block
    emit_mov_reg_reg(code, Reg::RAX, Reg::RSI);
    emit_ret(code);

    // then block -- record its position, then resolve the patch
    size_t then_block_pos = code.size();
    resolve_jump_patch(code, to_then, then_block_pos);

    emit_mov_reg_reg(code, Reg::RAX, Reg::RDI);
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

    MaxFunc fn = reinterpret_cast<MaxFunc>(mem);

    int64_t r1 = fn(3, 7);
    int64_t r2 = fn(10, 2);
    int64_t r3 = fn(5, 5);

    std::printf("max(3, 7) = %lld (expect 7)\n", (long long)r1);
    std::printf("max(10, 2) = %lld (expect 10)\n", (long long)r2);
    std::printf("max(5, 5) = %lld (expect 5)\n", (long long)r3);

    if (r1 != 7 || r2 != 10 || r3 != 5) {
        std::fprintf(stderr, "FAIL\n");
        return 1;
    }
    std::printf("PASS: control flow (cmp + conditional jump + patch) confirmed working\n");
    return 0;
}
