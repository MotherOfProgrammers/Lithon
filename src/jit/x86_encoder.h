#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

// Zero-dependency x86-64 instruction encoder. Hand-written, no
// external assembler or library.
//
// All sixteen general-purpose registers are supported. r8-r15 are
// reached through the REX.R / REX.B extension bits, which rex() below
// computes from the operands, so every emitter works uniformly for
// both register banks.

namespace lithon::jit {

enum class Reg : uint8_t {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3,
    RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8 = 8, R9 = 9, R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15
};

inline uint8_t reg_low3(Reg r) { return static_cast<uint8_t>(static_cast<uint8_t>(r) & 7); }
inline bool reg_is_extended(Reg r) { return static_cast<uint8_t>(r) >= 8; }

// REX prefix. W selects 64-bit operand size, R extends the ModRM.reg
// field, B extends the ModRM.rm field (or the opcode-embedded register).
inline uint8_t rex(bool w, Reg reg_field, Reg rm_field) {
    return static_cast<uint8_t>(0x40 | (w ? 8 : 0) |
                                (reg_is_extended(reg_field) ? 4 : 0) |
                                (reg_is_extended(rm_field) ? 1 : 0));
}

using CodeBuffer = std::vector<uint8_t>;

inline void emit_u8(CodeBuffer& buf, uint8_t byte) {
    buf.push_back(byte);
}

inline void emit_u32_le(CodeBuffer& buf, uint32_t bits) {
    for (int i = 0; i < 4; ++i) emit_u8(buf, static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
}

// ModRM byte for register-direct addressing: mod=11, reg field,
// rm field. Used by every reg-to-reg instruction below.
inline uint8_t modrm_reg_reg(Reg reg_field, Reg rm_field) {
    return static_cast<uint8_t>(0xC0 | (reg_low3(reg_field) << 3) | reg_low3(rm_field));
}

// mov dst, src  (64-bit register to register)
// Encoding: REX.W + 89 /r   (MOV r/m64, r64 -- src is the "reg" field,
// dst is the "r/m" field). Verified: mov rax, rdi = 48 89 f8.
inline void emit_mov_reg_reg(CodeBuffer& buf, Reg dst, Reg src) {
    emit_u8(buf, rex(true, src, dst));
    emit_u8(buf, 0x89);
    emit_u8(buf, modrm_reg_reg(src, dst));
}

// mov dst, imm64  (always the 10-byte movabs form)
// Encoding: REX.W + B8+r io.
inline void emit_mov_reg_imm64(CodeBuffer& buf, Reg dst, int64_t imm) {
    emit_u8(buf, static_cast<uint8_t>(0x48 | (reg_is_extended(dst) ? 1 : 0)));
    emit_u8(buf, static_cast<uint8_t>(0xB8 + reg_low3(dst)));
    uint64_t bits;
    std::memcpy(&bits, &imm, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        emit_u8(buf, static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
    }
}

inline bool fits_imm32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }
inline bool fits_imm8(int64_t v) { return v >= -128 && v <= 127; }

// mov dst, imm -- picks the shortest correct encoding:
//   0 <= imm <= 0xFFFFFFFF : mov r32, imm32   (5 bytes; zero-extends to 64)
//   fits signed 32 bits    : mov r/m64, imm32 (7 bytes; sign-extends)
//   otherwise              : movabs           (10 bytes)
inline void emit_mov_reg_imm(CodeBuffer& buf, Reg dst, int64_t imm) {
    if (imm >= 0 && imm <= 0xFFFFFFFFLL) {
        if (reg_is_extended(dst)) emit_u8(buf, 0x41);
        emit_u8(buf, static_cast<uint8_t>(0xB8 + reg_low3(dst)));
        emit_u32_le(buf, static_cast<uint32_t>(imm));
    } else if (fits_imm32(imm)) {
        emit_u8(buf, rex(true, Reg::RAX, dst));
        emit_u8(buf, 0xC7);
        emit_u8(buf, static_cast<uint8_t>(0xC0 | reg_low3(dst)));
        emit_u32_le(buf, static_cast<uint32_t>(static_cast<int32_t>(imm)));
    } else {
        emit_mov_reg_imm64(buf, dst, imm);
    }
}

// add dst, src  (64-bit register to register)
// Encoding: REX.W + 01 /r   (ADD r/m64, r64)
inline void emit_add_reg_reg(CodeBuffer& buf, Reg dst, Reg src) {
    emit_u8(buf, rex(true, src, dst));
    emit_u8(buf, 0x01);
    emit_u8(buf, modrm_reg_reg(src, dst));
}

// sub dst, src  (64-bit register to register)
// Encoding: REX.W + 29 /r   (SUB r/m64, r64)
inline void emit_sub_reg_reg(CodeBuffer& buf, Reg dst, Reg src) {
    emit_u8(buf, rex(true, src, dst));
    emit_u8(buf, 0x29);
    emit_u8(buf, modrm_reg_reg(src, dst));
}

// imul dst, src  (64-bit signed multiply, register to register)
// Encoding: REX.W + 0F AF /r   (IMUL r64, r/m64 -- operand order is
// REVERSED from add/sub: dst is the "reg" field here)
inline void emit_imul_reg_reg(CodeBuffer& buf, Reg dst, Reg src) {
    emit_u8(buf, rex(true, dst, src));
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0xAF);
    emit_u8(buf, modrm_reg_reg(dst, src));
}

// imul dst, src, imm32  (three-operand form, REX.W + 69 /r id).
// Works even when dst == src.
inline void emit_imul_reg_reg_imm32(CodeBuffer& buf, Reg dst, Reg src, int32_t imm) {
    emit_u8(buf, rex(true, dst, src));
    emit_u8(buf, 0x69);
    emit_u8(buf, modrm_reg_reg(dst, src));
    emit_u32_le(buf, static_cast<uint32_t>(imm));
}

// Group-1 ALU op with an immediate: REX.W + 83 /n ib (imm8) or
// REX.W + 81 /n id (imm32). /n: add=0, sub=5, cmp=7.
inline void emit_alu_reg_imm(CodeBuffer& buf, uint8_t digit, Reg dst, int32_t imm) {
    emit_u8(buf, rex(true, Reg::RAX, dst));
    if (fits_imm8(imm)) {
        emit_u8(buf, 0x83);
        emit_u8(buf, static_cast<uint8_t>(0xC0 | (digit << 3) | reg_low3(dst)));
        emit_u8(buf, static_cast<uint8_t>(static_cast<int8_t>(imm)));
    } else {
        emit_u8(buf, 0x81);
        emit_u8(buf, static_cast<uint8_t>(0xC0 | (digit << 3) | reg_low3(dst)));
        emit_u32_le(buf, static_cast<uint32_t>(imm));
    }
}

inline void emit_add_reg_imm32(CodeBuffer& buf, Reg dst, int32_t imm) { emit_alu_reg_imm(buf, 0, dst, imm); }
inline void emit_sub_reg_imm32(CodeBuffer& buf, Reg dst, int32_t imm) { emit_alu_reg_imm(buf, 5, dst, imm); }
inline void emit_cmp_reg_imm32(CodeBuffer& buf, Reg lhs, int32_t imm) { emit_alu_reg_imm(buf, 7, lhs, imm); }

// cmp lhs, rhs  (64-bit register to register)
// Encoding: REX.W + 39 /r   (CMP r/m64, r64)
inline void emit_cmp_reg_reg(CodeBuffer& buf, Reg lhs, Reg rhs) {
    emit_u8(buf, rex(true, rhs, lhs));
    emit_u8(buf, 0x39);
    emit_u8(buf, modrm_reg_reg(rhs, lhs));
}

// test reg, reg  (64-bit) -- ANDs the operand with itself purely to
// set flags (ZF set iff reg == 0), without modifying either operand.
// Encoding: REX.W + 85 /r (TEST r/m64, r64).
// Verified: test rax, rax = 48 85 c0.
inline void emit_test_reg_reg(CodeBuffer& buf, Reg reg) {
    emit_u8(buf, rex(true, reg, reg));
    emit_u8(buf, 0x85);
    emit_u8(buf, modrm_reg_reg(reg, reg));
}

// ret
inline void emit_ret(CodeBuffer& buf) {
    emit_u8(buf, 0xC3);
}

// A patch point: the byte offset within the buffer where a jump's
// 4-byte rel32 displacement lives, and the offset marking the END of
// that jump/call instruction (relative displacements are always
// computed from the address immediately following the instruction).
struct JumpPatch {
    size_t rel32_offset;
    size_t instr_end_offset;
};

inline void patch_u32_at(CodeBuffer& buf, size_t offset, uint32_t value) {
    buf[offset + 0] = static_cast<uint8_t>(value & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buf[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buf[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

inline void resolve_jump_patch(CodeBuffer& buf, const JumpPatch& patch, size_t target_offset) {
    int32_t rel = static_cast<int32_t>(
        static_cast<int64_t>(target_offset) - static_cast<int64_t>(patch.instr_end_offset));
    patch_u32_at(buf, patch.rel32_offset, static_cast<uint32_t>(rel));
}

// jmp rel32 (unconditional). Encoding: E9 cd.
inline JumpPatch emit_jmp_rel32(CodeBuffer& buf) {
    emit_u8(buf, 0xE9);
    size_t rel32_offset = buf.size();
    emit_u32_le(buf, 0);
    return JumpPatch{rel32_offset, buf.size()};
}

// Conditional jumps, following a preceding cmp or test. Encoding:
// 0F 8x cd. The x86 condition codes come in complementary pairs that
// differ only in the lowest bit, which invert() exploits.
enum class Cond : uint8_t {
    Less = 0x8C, GreaterEq = 0x8D, LessEq = 0x8E, Greater = 0x8F,
    Equal = 0x84, NotEqual = 0x85, NotZero = 0x85,
    // Unsigned-style conditions, named after the flags they read. These are
    // what comisd/ucomisd need: they treat the compared pair as if it were
    // an unsigned integer, which is exactly the ordering a double has.
    //   Below    = CF   (lhs <  rhs)
    //   Above    = !CF && !ZF  (lhs > rhs)
    //   Parity   = PF   (set when the operands were unordered, i.e. NaN)
    //   NotParity = !PF
    Below = 0x92, Above = 0x97, Parity = 0x9A, NotParity = 0x9B
};

inline Cond invert(Cond c) {
    return static_cast<Cond>(static_cast<uint8_t>(c) ^ 1);
}

inline JumpPatch emit_jcc_rel32(CodeBuffer& buf, Cond cond) {
    emit_u8(buf, 0x0F);
    emit_u8(buf, static_cast<uint8_t>(cond));
    size_t rel32_offset = buf.size();
    emit_u32_le(buf, 0);
    return JumpPatch{rel32_offset, buf.size()};
}

// --- Stack-relative addressing, for spilled values and locals ---
// Always uses the disp32 ModRM form (mod=10), not the shorter disp8
// form -- one uniform code path, correct for any offset magnitude.

inline void emit_disp32_le(CodeBuffer& buf, int32_t disp) {
    emit_u32_le(buf, static_cast<uint32_t>(disp));
}

// mov [rbp + offset], src   (store to a stack slot)
// Encoding: REX.W + 89 /r, ModRM(mod=10, reg=src, rm=RBP), disp32
inline void emit_store_rbp_offset(CodeBuffer& buf, Reg src, int32_t offset) {
    emit_u8(buf, rex(true, src, Reg::RBP));
    emit_u8(buf, 0x89);
    emit_u8(buf, static_cast<uint8_t>(0x80 | (reg_low3(src) << 3) | reg_low3(Reg::RBP)));
    emit_disp32_le(buf, offset);
}

// mov dst, [rbp + offset]   (load from a stack slot)
// Encoding: REX.W + 8B /r, ModRM(mod=10, reg=dst, rm=RBP), disp32
inline void emit_load_rbp_offset(CodeBuffer& buf, Reg dst, int32_t offset) {
    emit_u8(buf, rex(true, dst, Reg::RBP));
    emit_u8(buf, 0x8B);
    emit_u8(buf, static_cast<uint8_t>(0x80 | (reg_low3(dst) << 3) | reg_low3(Reg::RBP)));
    emit_disp32_le(buf, offset);
}

// push reg / pop reg. Encoding: [41] 50+r / [41] 58+r.
inline void emit_push_reg(CodeBuffer& buf, Reg reg) {
    if (reg_is_extended(reg)) emit_u8(buf, 0x41);
    emit_u8(buf, static_cast<uint8_t>(0x50 + reg_low3(reg)));
}

inline void emit_pop_reg(CodeBuffer& buf, Reg reg) {
    if (reg_is_extended(reg)) emit_u8(buf, 0x41);
    emit_u8(buf, static_cast<uint8_t>(0x58 + reg_low3(reg)));
}

// sub rsp, imm32 -- always uses the imm32 form (REX.W + 81 /5 id).
inline void emit_sub_rsp_imm32(CodeBuffer& buf, int32_t imm) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x81);
    emit_u8(buf, static_cast<uint8_t>(0xC0 | (5 << 3) | reg_low3(Reg::RSP)));
    emit_disp32_le(buf, imm);
}

// add rsp, imm32 (REX.W + 81 /0 id). Used to release Windows shadow space.
inline void emit_add_rsp_imm32(CodeBuffer& buf, int32_t imm) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x81);
    emit_u8(buf, static_cast<uint8_t>(0xC0 | (0 << 3) | reg_low3(Reg::RSP)));
    emit_disp32_le(buf, imm);
}

// Standard function prologue: push rbp, mov rbp, rsp, sub rsp, frame_size.
inline void emit_prologue(CodeBuffer& buf, int32_t frame_size) {
    emit_push_reg(buf, Reg::RBP);
    emit_mov_reg_reg(buf, Reg::RBP, Reg::RSP);
    if (frame_size > 0) {
        emit_sub_rsp_imm32(buf, frame_size);
    }
}

// Standard function epilogue: mov rsp, rbp, pop rbp. Caller emits
// `ret` separately.
inline void emit_epilogue(CodeBuffer& buf) {
    emit_mov_reg_reg(buf, Reg::RSP, Reg::RBP);
    emit_pop_reg(buf, Reg::RBP);
}

// and dst, src  (64-bit register to register)
// Encoding: [REX] 21 /r   (AND r/m64, r64 -- src is the "reg" field,
// dst is the "r/m" field). Matches emit_add_reg_reg / emit_sub_reg_reg.
inline void emit_and_reg_reg(CodeBuffer& buf, Reg dst, Reg src) {
    if (reg_is_extended(dst) || reg_is_extended(src)) emit_u8(buf, rex(false, src, dst));
    emit_u8(buf, 0x21);
    emit_u8(buf, modrm_reg_reg(src, dst));
}

// setcc_opcode -- the 0F 9x byte for a condition.
//
// The two tables the CPU keeps are offset, and only for their first half:
//   Jcc    0F 80-8F   (je, jne, jl, jlE, jg, jge, jle, ja, ...)
//   SETcc  0F 90-9F   (sete, setne, setl, ..., setp, setnp)
// so within 0x80-0x8F the SETcc byte is the Jcc byte plus 0x10. The parity
// conditions break that rule: jp/jnp are 0F 9A/0F 9B, and setp/setnp are
// *also* 0F 9A/0F 9B. Adding 0x10 to a parity condition would land on
// 0F AA/0F AB, which are unrelated instructions (stosb, stosd) -- the
// program would silently do something else entirely. Hence the split
// rather than a blanket +0x10.
inline uint8_t setcc_opcode(Cond cond) {
    uint8_t c = static_cast<uint8_t>(cond);
    return static_cast<uint8_t>(c < 0x90 ? c + 0x10 : c);
}

// setcc dst_low_byte -- sets the low 8 bits of dst to 0 or 1 based
// on the flags from a preceding cmp/test. Encoding: [REX] 0F 9x /0.
// A REX prefix is always emitted: it is required for r8b-r15b and
// makes indices 4-7 mean spl/bpl/sil/dil instead of ah/ch/dh/bh.
//
// Do not confuse this with branching. In the 0F 9x range the byte alone
// does not say which instruction it is: 0F 9A is `setp r/m8` when the
// following byte is a register ModRM, and `jp rel32` otherwise, and the
// displacement of a forward jump very often does look like a
// non-register ModRM (0x0d is mod=00 rm=101, RIP-relative). Emitting a
// parity Jcc with a rel32 is therefore a trap; spell the condition out
// with setcc + and_reg_reg instead.
inline void emit_setcc(CodeBuffer& buf, Cond cond, Reg dst_low_byte) {
    emit_u8(buf, static_cast<uint8_t>(0x40 | (reg_is_extended(dst_low_byte) ? 1 : 0)));
    emit_u8(buf, 0x0F);
    emit_u8(buf, setcc_opcode(cond));
    emit_u8(buf, static_cast<uint8_t>(0xC0 | reg_low3(dst_low_byte)));
}

// movzx dst64, src_low_byte -- zero-extends an 8-bit value into a
// full 64-bit register. Encoding: REX.W + 0F B6 /r.
inline void emit_movzx_reg_reg8(CodeBuffer& buf, Reg dst64, Reg src_low_byte) {
    emit_u8(buf, rex(true, dst64, src_low_byte));
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0xB6);
    emit_u8(buf, modrm_reg_reg(dst64, src_low_byte));
}

// xor reg32, reg32 (self-xor to zero a register). Encoding: [REX] 31 /r.
// Verified: xor eax, eax = 31 c0.
inline void emit_xor_zero(CodeBuffer& buf, Reg reg) {
    if (reg_is_extended(reg)) emit_u8(buf, rex(false, reg, reg));
    emit_u8(buf, 0x31);
    emit_u8(buf, modrm_reg_reg(reg, reg));
}

// call reg (indirect call through a register holding a runtime
// address -- used to call host-process functions like printf).
// Encoding: [41] FF /2 (CALL r/m64). Verified: call rax = ff d0.
inline void emit_call_reg(CodeBuffer& buf, Reg reg) {
    if (reg_is_extended(reg)) emit_u8(buf, 0x41);
    emit_u8(buf, 0xFF);
    emit_u8(buf, static_cast<uint8_t>(0xC0 | (2 << 3) | reg_low3(reg)));
}

// ---------------------------------------------------------------------
// SSE2 scalar double-precision.
//
// A genuinely separate opcode family from everything above: these use
// the F2 0F mandatory prefix for packed-vs-scalar-double
// disambiguation, and XMM register numbers run 0-15, so the low-3 bits
// plus a REX.R extension describe the whole range. XMM0-XMM7 need no
// REX at all; XMM8-XMM15 need REX.R (bit 2) on the reg field.
//
// There is no "mov xmm, imm64" in x86-64. A double literal is always
// loaded from memory, so ConstFloat materializes into a constant pool
// and loads from [rip + disp32] -- see compile_function.h.
// ---------------------------------------------------------------------

enum class Xmm : uint8_t { XMM0 = 0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7,
                           XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15,
                           // Not a register. Passed to rex_sse for the rm
                           // field of an instruction whose rm operand is
                           // memory, where no REX.B bit applies. The encoding
                           // helpers never accept it as a destination.
                           none = 0xFF };

inline uint8_t xmm_low3(Xmm r) { return static_cast<uint8_t>(static_cast<uint8_t>(r) & 7); }
inline bool xmm_is_extended(Xmm r) { return static_cast<uint8_t>(r) >= 8; }

// REX for an SSE instruction, given the register that lands in ModRM.reg
// and, when ModRM.rm is a register rather than memory, the one in rm.
//
// Both extension bits matter. REX.R extends reg and REX.B extends rm, and
// they are independent: an instruction reading xmm14 as its rm operand needs
// REX.B set even when its reg operand is xmm0. kScratchFloat (XMM15) and
// kScratchFloatB (XMM14) are both in the extended range, so every reg-reg
// float op here sets both bits. Leaving B clear makes the CPU read the low
// three bits as xmm6, silently decoding a different register.
//
// Pass `none` for the rm argument when rm is a memory operand (ModRM rm=101
// RIP-relative, or mod=10 rbp-relative); memory operands are never extended.
inline uint8_t rex_sse(Xmm reg_field, Xmm rm_field = Xmm::none) {
    return static_cast<uint8_t>(0x40 | (xmm_is_extended(reg_field) ? 4 : 0) |
                                (xmm_is_extended(rm_field) ? 1 : 0));
}

// F2 0F <opcode> /r, reg <- reg op reg. Covers addsd/subsd/mulsd/divsd,
// which differ only in the opcode byte. Verified: addsd xmm0, xmm1 =
// f2 0f 58 c1.
inline void emit_sse_sd_op(CodeBuffer& buf, uint8_t opcode, Xmm dst, Xmm src) {
    if (xmm_is_extended(dst) || xmm_is_extended(src)) emit_u8(buf, rex_sse(dst, src));
    emit_u8(buf, 0xF2);
    emit_u8(buf, 0x0F);
    emit_u8(buf, opcode);
    emit_u8(buf, modrm_reg_reg(static_cast<Reg>(xmm_low3(dst)), static_cast<Reg>(xmm_low3(src))));
}

inline void emit_addsd(CodeBuffer& buf, Xmm dst, Xmm src) { emit_sse_sd_op(buf, 0x58, dst, src); }
inline void emit_subsd(CodeBuffer& buf, Xmm dst, Xmm src) { emit_sse_sd_op(buf, 0x5C, dst, src); }
inline void emit_mulsd(CodeBuffer& buf, Xmm dst, Xmm src) { emit_sse_sd_op(buf, 0x59, dst, src); }
inline void emit_divsd(CodeBuffer& buf, Xmm dst, Xmm src) { emit_sse_sd_op(buf, 0x5E, dst, src); }

// comisd xmm, xmm -- sets ZF/PF/CF like cmp, raising Invalid on either NaN.
// Encoding: 66 0F 2F /r. Verified: comisd xmm0, xmm1 = 66 0f 2f c1.
// Used for both float ordering compares and the Div-by-zero check.
inline void emit_comisd(CodeBuffer& buf, Xmm lhs, Xmm rhs) {
    if (xmm_is_extended(lhs) || xmm_is_extended(rhs)) emit_u8(buf, rex_sse(lhs, rhs));
    emit_u8(buf, 0x66);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x2F);
    emit_u8(buf, modrm_reg_reg(static_cast<Reg>(xmm_low3(lhs)), static_cast<Reg>(xmm_low3(rhs))));
}

// ucomisd xmm, xmm -- the quiet variant: identical ZF/PF/CF result, but it
// raises Invalid only for QNaN, so an SNaN operand does not fault.
// Encoding: 66 0F 2E /r. Prefers ucomisd over comisd for the ordering
// compares because it never traps on a signalling NaN.
inline void emit_ucomisd(CodeBuffer& buf, Xmm lhs, Xmm rhs) {
    if (xmm_is_extended(lhs) || xmm_is_extended(rhs)) emit_u8(buf, rex_sse(lhs, rhs));
    emit_u8(buf, 0x66);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x2E);
    emit_u8(buf, modrm_reg_reg(static_cast<Reg>(xmm_low3(lhs)), static_cast<Reg>(xmm_low3(rhs))));
}

// comisd against an all-zero double held in memory: comisd xmm, [rip+d32].
// Encoding: 66 0F 2F /r with ModRM(mod=00, reg=lhs, rm=101), disp32.
// The rm=101 + mod=00 combination is RIP-relative, which is what makes
// this position-independent -- the JIT emits code into an mmap'd buffer
// whose address it does not control.
inline void emit_comisd_zero(CodeBuffer& buf, Xmm lhs, int32_t disp32) {
    if (xmm_is_extended(lhs)) emit_u8(buf, rex_sse(lhs));
    emit_u8(buf, 0x66);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x2F);
    emit_u8(buf, static_cast<uint8_t>(0x05 | (xmm_low3(lhs) << 3)));  // mod=00 rm=101
    emit_disp32_le(buf, disp32);
}

// movsd xmm, [rip + disp32] -- load a double from the embedded constant
// pool. Encoding: F2 0F 10 /r, ModRM(mod=00, reg=dst, rm=101), disp32.
// The disp32 is emitted as a zero placeholder and patched by the caller
// once the pool's final position is known; see movsd_rip_disp_offset.
// The rm=101 + mod=00 combination is RIP-relative, which is what makes
// this position-independent -- the JIT emits code into an mmap'd buffer
// whose address it does not control.
inline void emit_movsd_xmm_rip(CodeBuffer& buf, Xmm dst) {
    if (xmm_is_extended(dst)) emit_u8(buf, rex_sse(dst));
    emit_u8(buf, 0xF2);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x10);
    emit_u8(buf, static_cast<uint8_t>(0x05 | (xmm_low3(dst) << 3)));  // mod=00 rm=101
    emit_disp32_le(buf, 0);
}

// movsd [rip + disp32], xmm -- store a double to the constant pool or a
// code-embedded data slot. Encoding: F2 0F 11 /r, ModRM(mod=00,
// reg=src, rm=101), disp32. Like emit_movsd_xmm_rip, the disp32 is a zero
// placeholder the caller patches once the target position is final.
inline void emit_movsd_mem_rip(CodeBuffer& buf, Xmm src) {
    if (xmm_is_extended(src)) emit_u8(buf, rex_sse(src));
    emit_u8(buf, 0xF2);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x11);
    emit_u8(buf, static_cast<uint8_t>(0x05 | (xmm_low3(src) << 3)));  // mod=00 rm=101
    emit_disp32_le(buf, 0);
}

// Returns the offset of the RIP-relative disp32 that emit_movsd_xmm_rip or
// emit_movsd_mem_rip just wrote, so the caller can patch it once the pool's
// final position is known.
//
// This must be called IMMEDIATELY after the emitter, while the disp32 is
// still the last thing in the buffer. It is not a query about an arbitrary
// earlier instruction -- it is a "where did that just-written instruction
// put its displacement" helper, and the two are only equivalent because the
// emitters put nothing after the disp32.
inline size_t movsd_rip_disp_offset(CodeBuffer& buf) { return buf.size() - 4; }

// movsd xmm, xmm -- register-to-register double move. Encoding:
// F2 0F 10 /r. This is how a double gets from the register it was
// computed in to the register it is consumed from.
inline void emit_movsd_xmm_xmm(CodeBuffer& buf, Xmm dst, Xmm src) {
    if (xmm_is_extended(dst) || xmm_is_extended(src)) emit_u8(buf, rex_sse(dst, src));
    emit_u8(buf, 0xF2);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x10);
    emit_u8(buf, modrm_reg_reg(static_cast<Reg>(xmm_low3(dst)), static_cast<Reg>(xmm_low3(src))));
}

// movsd xmm, [rbp + disp32] -- load a double from a frame slot. The XMM
// counterpart of emit_load_rbp_offset: same rbp-relative addressing, F2
// 0F 10 /r with ModRM(mod=10, reg=dst, rm=RBP).
inline void emit_movsd_xmm_rbp(CodeBuffer& buf, Xmm dst, int32_t offset) {
    if (xmm_is_extended(dst)) emit_u8(buf, rex_sse(dst));
    emit_u8(buf, 0xF2);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x10);
    emit_u8(buf, static_cast<uint8_t>(0x80 | (xmm_low3(dst) << 3) | reg_low3(Reg::RBP)));
    emit_disp32_le(buf, offset);
}

// movsd [rbp + disp32], xmm -- store a double into a frame slot.
inline void emit_movsd_rbp_mem(CodeBuffer& buf, Xmm src, int32_t offset) {
    if (xmm_is_extended(src)) emit_u8(buf, rex_sse(src));
    emit_u8(buf, 0xF2);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x11);
    emit_u8(buf, static_cast<uint8_t>(0x80 | (xmm_low3(src) << 3) | reg_low3(Reg::RBP)));
    emit_disp32_le(buf, offset);
}

// cvtsi2sd xmm, r64 -- convert a signed 64-bit integer to double.
// Encoding: F2 REX.W 0F 2A /r. Required because the type lattice
// promotes int+float to Float, so an int operand of a mixed expression
// must be widened before the SSE op. Verified: cvtsi2sd xmm0, rax =
// f2 48 0f 2a c0.
inline void emit_cvtsi2sd(CodeBuffer& buf, Xmm dst, Reg src64) {
    emit_u8(buf, 0xF2);
    emit_u8(buf, rex(true, static_cast<Reg>(0), src64));  // REX.W + REX.B
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x2A);
    emit_u8(buf, modrm_reg_reg(static_cast<Reg>(xmm_low3(dst)), src64));
}

// cvttsd2si r64, xmm -- convert a double to a signed 64-bit integer,
// truncating toward zero. Encoding: F2 REX.W 0F 2C /r.
inline void emit_cvttsd2si(CodeBuffer& buf, Reg dst64, Xmm src) {
    emit_u8(buf, 0xF2);
    emit_u8(buf, rex(true, dst64, static_cast<Reg>(0)));  // REX.W + REX.R
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x2C);
    emit_u8(buf, modrm_reg_reg(dst64, static_cast<Reg>(xmm_low3(src))));
}

// xorpd xmm, xmm -- zero an XMM register. Unlike xor on GP registers
// this needs no REX.W: SSE operands are 128 bits, already full size.
// Encoding: 66 0F 57 /r. Verified: xorpd xmm0, xmm0 = 66 0f 57 c0.
// Used to materialize 0.0 for the Div-by-zero check.
inline void emit_xorpd_zero(CodeBuffer& buf, Xmm reg) {
    if (xmm_is_extended(reg)) emit_u8(buf, rex_sse(reg));
    emit_u8(buf, 0x66);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x57);
    emit_u8(buf, modrm_reg_reg(static_cast<Reg>(xmm_low3(reg)), static_cast<Reg>(xmm_low3(reg))));
}

} // namespace lithon::jit
