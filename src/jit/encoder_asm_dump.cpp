// Emits "<intel-syntax text>|<hex bytes>" lines covering every instruction form
// the encoder supports, across all 16 registers. tools/check_encoder_vs_as.py
// assembles the same text with GNU as and compares the bytes.
//
//   g++ -std=c++20 -Isrc/jit -o /tmp/encoder_asm_dump src/jit/encoder_asm_dump.cpp

#include <cstdio>
#include <string>
#include <vector>
#include "x86_encoder.h"

using namespace lithon::jit;

static const char* R64[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                              "r8","r9","r10","r11","r12","r13","r14","r15"};
static const char* R32[16] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi",
                              "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
static const char* R8B[16] = {"al","cl","dl","bl","spl","bpl","sil","dil",
                              "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"};

static void line(const std::string& text, const CodeBuffer& b) {
    std::printf("%s|", text.c_str());
    for (auto x : b) std::printf("%02x", x);
    std::printf("\n");
}

int main() {
    const int64_t imm64s[] = {0, 1, -1, 0x123456789abcdefLL, INT64_MIN};
    const int32_t imms[] = {0, 1, -1, 127, 128, -128, -129, 200000000, -2000000000, INT32_MAX, INT32_MIN};
    // emit_mov_reg_imm picks mov r32 (zero-extend) / mov r/m64 imm32 (sign-extend) / movabs
    const int64_t mov_imms[] = {0, 1, 127, 0x7fffffffLL, 0x80000000LL, 0xffffffffLL, 0x100000000LL,
                                -1, -2147483648LL, -2147483649LL, INT64_MAX, INT64_MIN};
    const int32_t rsp_imms[] = {128, 1024, 4096};
    const int32_t imul_imms[] = {3, -5, 127, 128, -129, 100000};
    // Every legal count, including both ends. 0 and 64 are the interesting
    // ones: x86 masks the count to 6 bits, so 64 would execute as 0, which is
    // why compile_function.h range-checks before emitting.
    const int32_t shift_imms[] = {0, 1, 7, 31, 32, 63};
    const int32_t disps[] = {-300, 1000, -100000};
    struct CC { Cond c; const char* n; } ccs[] = {
        {Cond::Less,"l"},{Cond::GreaterEq,"ge"},{Cond::LessEq,"le"},{Cond::Greater,"g"},
        {Cond::Equal,"e"},{Cond::NotEqual,"ne"}};

    for (int d = 0; d < 16; ++d) {
        Reg rd = static_cast<Reg>(d);
        std::string D = R64[d];
        { CodeBuffer b; emit_test_reg_reg(b, rd); line("test " + D + ", " + D, b); }
        { CodeBuffer b; emit_call_reg(b, rd); line("call " + D, b); }
        { CodeBuffer b; emit_push_reg(b, rd); line("push " + D, b); }
        { CodeBuffer b; emit_pop_reg(b, rd); line("pop " + D, b); }
        { CodeBuffer b; emit_xor_zero(b, rd);
          line(std::string("xor ") + R32[d] + ", " + R32[d], b); }
        for (auto v : imm64s) {
            CodeBuffer b; emit_mov_reg_imm64(b, rd, v);
            line("movabs " + D + ", " + std::to_string(v), b);
        }
        for (auto v : mov_imms) {
            CodeBuffer b; emit_mov_reg_imm(b, rd, v);
            // 0..0xFFFFFFFF is emitted as `mov r32, imm32` (zero-extends to 64 bits)
            if (v >= 0 && v <= 0xFFFFFFFFLL) line(std::string("mov ") + R32[d] + ", " + std::to_string(v), b);
            else                              line("mov " + D + ", " + std::to_string(v), b);
        }
        for (auto v : imms) {
            { CodeBuffer b; emit_add_reg_imm32(b, rd, v); line("add " + D + ", " + std::to_string(v), b); }
            { CodeBuffer b; emit_sub_reg_imm32(b, rd, v); line("sub " + D + ", " + std::to_string(v), b); }
            { CodeBuffer b; emit_cmp_reg_imm32(b, rd, v); line("cmp " + D + ", " + std::to_string(v), b); }
        }
        for (auto v : disps) {
            { CodeBuffer b; emit_store_rbp_offset(b, rd, v);
              line("mov qword ptr [rbp" + std::string(v < 0 ? "" : "+") + std::to_string(v) + "], " + D, b); }
            { CodeBuffer b; emit_load_rbp_offset(b, rd, v);
              line("mov " + D + ", qword ptr [rbp" + std::string(v < 0 ? "" : "+") + std::to_string(v) + "]", b); }
        }
        for (auto& cc : ccs) {
            CodeBuffer b; emit_setcc(b, cc.c, rd);
            line(std::string("set") + cc.n + " " + R8B[d], b);
        }
        { CodeBuffer b; emit_shl_reg_1(b, rd); line("shl " + D + ", 1", b); }
        { CodeBuffer b; emit_sar_reg_1(b, rd); line("sar " + D + ", 1", b); }
        for (auto v : shift_imms) {
            if (v == 1) continue;   // covered by the D1 short form above
            { CodeBuffer b; emit_shl_reg_imm8(b, rd, static_cast<uint8_t>(v));
              line("shl " + D + ", " + std::to_string(v), b); }
            // sar, not shr: shr is the /5 LOGICAL form and would make `>>` on a
            // negative value wrong. Cross-checking this against as is what
            // keeps the two from being confused again.
            { CodeBuffer b; emit_sar_reg_imm8(b, rd, static_cast<uint8_t>(v));
              line("sar " + D + ", " + std::to_string(v), b); }
        }
        { CodeBuffer b; emit_shl_reg_cl(b, rd); line("shl " + D + ", cl", b); }
        { CodeBuffer b; emit_sar_reg_cl(b, rd); line("sar " + D + ", cl", b); }
        for (int s = 0; s < 16; ++s) {
            Reg rs = static_cast<Reg>(s);
            std::string S = R64[s];
            { CodeBuffer b; emit_mov_reg_reg(b, rd, rs); line("mov " + D + ", " + S, b); }
            { CodeBuffer b; emit_add_reg_reg(b, rd, rs); line("add " + D + ", " + S, b); }
            { CodeBuffer b; emit_sub_reg_reg(b, rd, rs); line("sub " + D + ", " + S, b); }
            { CodeBuffer b; emit_cmp_reg_reg(b, rd, rs); line("cmp " + D + ", " + S, b); }
            { CodeBuffer b; emit_imul_reg_reg(b, rd, rs); line("imul " + D + ", " + S, b); }
            // The 64-bit bitwise ALU. These carry REX.W; the 32-bit
            // emit_and_reg_reg below deliberately does not, and listing both
            // here is what makes that difference checkable against as instead
            // of being a claim in a comment.
            { CodeBuffer b; emit_and_reg_reg64(b, rd, rs); line("and " + D + ", " + S, b); }
            { CodeBuffer b; emit_or_reg_reg64(b, rd, rs); line("or " + D + ", " + S, b); }
            { CodeBuffer b; emit_xor_reg_reg64(b, rd, rs); line("xor " + D + ", " + S, b); }
            { CodeBuffer b; emit_and_reg_reg(b, rd, rs);
              line(std::string("and ") + R32[d] + ", " + R32[s], b); }
            { CodeBuffer b; emit_movzx_reg_reg8(b, rd, rs);
              line("movzx " + D + ", " + R8B[s], b); }
            for (auto v : imul_imms) {
                CodeBuffer b; emit_imul_reg_reg_imm32(b, rd, rs, v);
                line("imul " + D + ", " + S + ", " + std::to_string(v), b);
            }
        }
    }
    for (auto v : rsp_imms) {
        { CodeBuffer b; emit_sub_rsp_imm32(b, v); line("sub rsp, " + std::to_string(v), b); }
        { CodeBuffer b; emit_add_rsp_imm32(b, v); line("add rsp, " + std::to_string(v), b); }
    }
    return 0;
}
