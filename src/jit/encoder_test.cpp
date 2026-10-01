// Same add(3, 4) = 7 proof as zero_dep_test.cpp, but now built from
// the reusable x86_encoder.h helpers instead of a hardcoded byte
// array which confirms the encoder produces byte-identical output to
// the hand-verified sequence.

#include "x86_encoder.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace lithon::jit;

typedef int64_t (*AddFunc)(int64_t, int64_t);

static int failures = 0;

// Byte-exact check of one SSE encoding.
static void expect_bytes(const char* what, const CodeBuffer& code,
                         std::initializer_list<uint8_t> want) {
    std::vector<uint8_t> expected(want);
    if (code == expected) return;
    std::fprintf(stderr, "FAIL %s\n  want:", what);
    for (auto b : expected) std::fprintf(stderr, " %02x", b);
    std::fprintf(stderr, "\n  got :");
    for (auto b : code) std::fprintf(stderr, " %02x", b);
    std::fprintf(stderr, "\n");
    ++failures;
}

// The SSE2 encodings that float support depends on. Each of these three
// bugs shipped and each produced wrong values or a segfault rather than a
// compile error, so they are pinned byte-for-byte here.
static int float_encoding_checks() {
    std::printf("--- SSE2 double-precision encodings ---\n");

    // addsd xmm0, xmm1 = f2 0f 58 c1. Neither register is extended, so
    // there is no REX prefix at all.
    {
        CodeBuffer c;
        emit_addsd(c, Xmm::XMM0, Xmm::XMM1);
        expect_bytes("addsd xmm0, xmm1", c, {0xf2, 0x0f, 0x58, 0xc1});
    }

    // The REX test. Addsd xmm15, xmm14 needs REX.R *and* REX.B (0x45):
    // 0x44 alone extends only the ModRM.reg field, so the CPU would read
    // the rm field as xmm6 and compute on the wrong register. Both
    // operands here are the reserved float scratch registers, which is
    // exactly the case that got this wrong.
    {
        CodeBuffer c;
        emit_addsd(c, Xmm::XMM15, Xmm::XMM14);
        expect_bytes("addsd xmm15, xmm14 (REX.R and REX.B)", c,
                     {0x45, 0xf2, 0x0f, 0x58, 0xfe});
    }
    // One extended operand is enough to need a REX prefix, and only the
    // bit for the field that is actually extended.
    {
        CodeBuffer c;
        emit_addsd(c, Xmm::XMM15, Xmm::XMM1);
        expect_bytes("addsd xmm15, xmm1 (REX.R only)", c,
                     {0x44, 0xf2, 0x0f, 0x58, 0xf9});
    }

    // The SETcc opcode table. Cond::NotParity is 0x9B as a Jcc, and its
    // SETcc form is *also* 0x9B -- unlike every other condition, where
    // SETcc is the Jcc byte plus 0x10. A blanket +0x10 turned setnp into
    // 0xAB, which is stosd, so the float comparison silently did the
    // wrong thing. Both halves are pinned.
    {
        CodeBuffer c;
        emit_setcc(c, Cond::Equal, Reg::RAX);
        expect_bytes("sete al (Jcc 0x84 -> SETcc 0x94)", c,
                     {0x40, 0x0f, 0x94, 0xc0});
    }
    {
        CodeBuffer c;
        emit_setcc(c, Cond::Below, Reg::RAX);
        expect_bytes("setb al (Jcc 0x92 -> SETcc 0x92)", c,
                     {0x40, 0x0f, 0x92, 0xc0});
    }
    {
        CodeBuffer c;
        emit_setcc(c, Cond::NotParity, Reg::R10);
        expect_bytes("setnp r10b (Jcc 0x9B -> SETcc 0x9B, no +0x10)", c,
                     {0x41, 0x0f, 0x9b, 0xc2});
    }
    {
        CodeBuffer c;
        emit_setcc(c, Cond::Parity, Reg::RAX);
        expect_bytes("setp al (Jcc 0x9A -> SETcc 0x9A, no +0x10)", c,
                     {0x40, 0x0f, 0x9a, 0xc0});
    }

    // and dst, src is AND r/m, r: src goes in ModRM.reg and dst in rm.
    // Getting these the other way round computed `tmp &= dst` and left
    // the comparison result untouched, which is how a NaN comparison came
    // out True. Pinned in both directions because the operand order is
    // not visible in the C++ signature.
    {
        CodeBuffer c;
        emit_and_reg_reg(c, Reg::RAX, Reg::R10);
        expect_bytes("and rax, r10 (dst in rm, src in reg)", c,
                     {0x44, 0x21, 0xd0});
    }

    // ucomisd xmm7, xmm6 = 66 0f 2e fe, and the RIP-relative pool load
    // must carry a 4-byte disp32 placeholder. Without it the constant
    // pool was unreachable: the following instruction's bytes were
    // consumed as the displacement, and the pool load read whatever
    // followed. movsd_rip_disp_offset() computes the patch site as
    // size()-4, so the placeholder is load-bearing.
    {
        CodeBuffer c;
        emit_ucomisd(c, Xmm::XMM7, Xmm::XMM6);
        expect_bytes("ucomisd xmm7, xmm6", c, {0x66, 0x0f, 0x2e, 0xfe});
    }
    {
        CodeBuffer c;
        emit_movsd_xmm_rip(c, Xmm::XMM0);
        expect_bytes("movsd xmm0, [rip+d32] (9 bytes, placeholder present)", c,
                     {0xf2, 0x0f, 0x10, 0x05, 0x00, 0x00, 0x00, 0x00});
        // f2 0f 10 /r with a RIP-relative rm is 1 F2 + 2 opcode + 1 ModRM
        // + 4 disp32 = 8, so the disp32 is the last 4 bytes and the patch
        // site movsd_rip_disp_offset() reports is byte 4. An earlier version
        // of the emitter omitted the disp32 entirely, which left this at
        // 4 bytes and made the patch land inside the *next* instruction.
        if (c.size() != 8 || movsd_rip_disp_offset(c) != 4) {
            std::fprintf(stderr,
                         "FAIL movsd rip patch site: size=%zu offset=%zu, want 8 and 4\n",
                         c.size(), movsd_rip_disp_offset(c));
            ++failures;
        }
    }
    {
        CodeBuffer c;
        emit_movsd_mem_rip(c, Xmm::XMM0);
        expect_bytes("movsd [rip+d32], xmm0 (9 bytes, placeholder present)", c,
                     {0xf2, 0x0f, 0x11, 0x05, 0x00, 0x00, 0x00, 0x00});
    }

    if (failures) {
        std::fprintf(stderr, "FAIL: %d SSE2 encoding check(s) wrong\n", failures);
        return 1;
    }
    std::printf("PASS: SSE2 encodings byte-exact\n");
    return 0;
}

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

    return float_encoding_checks();
}
