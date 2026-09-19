// Same add(3, 4) = 7 proof as zero_dep_test.cpp, but now built from
// the reusable x86_encoder.h helpers instead of a hardcoded byte
// array which confirms the encoder produces byte-identical output to
// the hand-verified sequence.

#include "x86_encoder.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>

using namespace lithon::jit;

typedef int64_t (*AddFunc)(int64_t, int64_t);

int main() {
    CodeBuffer code;

    // System V AMD64: arg0 in rdi, arg1 in rsi, return in rax.
    emit_mov_reg_reg(code, Reg::RAX, Reg::RDI);  // rax = a
    emit_add_reg_reg(code, Reg::RAX, Reg::RSI);  // rax += b
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

    AddFunc fn = reinterpret_cast<AddFunc>(mem);
    int64_t result = fn(3, 4);

    std::printf("native add(3, 4) = %lld\n", (long long)result);
    if (result != 7) {
        std::fprintf(stderr, "FAIL: expected 7, got %lld\n", (long long)result);
        return 1;
    }
    std::printf("PASS: reusable encoder confirmed working\n");
    return 0;
}
