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
    Equal = 0x84, NotEqual = 0x85, NotZero = 0x85
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

// setcc dst_low_byte -- sets the low 8 bits of dst to 0 or 1 based
// on the flags from a preceding cmp/test. Encoding: [REX] 0F 9x /0.
// The SETcc opcode is the matching Jcc opcode PLUS 0x10.
// A REX prefix is always emitted: it is required for r8b-r15b and
// makes indices 4-7 mean spl/bpl/sil/dil instead of ah/ch/dh/bh.
inline void emit_setcc(CodeBuffer& buf, Cond cond, Reg dst_low_byte) {
    emit_u8(buf, static_cast<uint8_t>(0x40 | (reg_is_extended(dst_low_byte) ? 1 : 0)));
    emit_u8(buf, 0x0F);
    emit_u8(buf, static_cast<uint8_t>(static_cast<uint8_t>(cond) + 0x10));
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

} // namespace lithon::jit
