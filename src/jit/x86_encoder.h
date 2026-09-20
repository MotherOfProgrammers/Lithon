#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace lithon::jit {

enum class Reg : uint8_t {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3,
    RSP = 4, RBP = 5, RSI = 6, RDI = 7
};

using CodeBuffer = std::vector<uint8_t>;

inline void emit_u8(CodeBuffer& buf, uint8_t byte) {
    buf.push_back(byte);
}

// ModRM byte for register-direct addressing: mod=11, reg field,
// rm field. Used by every reg-to-reg instruction below.
inline uint8_t modrm_reg_reg(Reg reg_field, Reg rm_field) {
    return static_cast<uint8_t>(0xC0 | (static_cast<uint8_t>(reg_field) << 3) | static_cast<uint8_t>(rm_field));
}

inline void emit_mov_reg_reg(CodeBuffer& buf, Reg dst, Reg src) {
    emit_u8(buf, 0x48);                     // REX.W
    emit_u8(buf, 0x89);                     // MOV r/m64, r64
    emit_u8(buf, modrm_reg_reg(src, dst));
}

inline void emit_mov_reg_imm64(CodeBuffer& buf, Reg dst, int64_t imm) {
    emit_u8(buf, 0x48);                              // REX.W
    emit_u8(buf, static_cast<uint8_t>(0xB8 + static_cast<uint8_t>(dst)));
    uint64_t bits;
    std::memcpy(&bits, &imm, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        emit_u8(buf, static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
    }
}

// add dst, src  (64-bit register to register)
// Encoding: REX.W + 01 /r   (ADD r/m64, r64)
inline void emit_add_reg_reg(CodeBuffer& buf, Reg dst, Reg src) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x01);
    emit_u8(buf, modrm_reg_reg(src, dst));
}

// sub dst, src  (64-bit register to register)
// Encoding: REX.W + 29 /r   (SUB r/m64, r64)
inline void emit_sub_reg_reg(CodeBuffer& buf, Reg dst, Reg src) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x29);
    emit_u8(buf, modrm_reg_reg(src, dst));
}

// imul dst, src  (64-bit signed multiply, register to register)
// Encoding: REX.W + 0F AF /r   (IMUL r64, r/m64 -- note operand
// order is REVERSED from add/sub: dst is the "reg" field here)
inline void emit_imul_reg_reg(CodeBuffer& buf, Reg dst, Reg src) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0xAF);
    emit_u8(buf, modrm_reg_reg(dst, src));
}

// cmp lhs, rhs  (64-bit register to register)
// Encoding: REX.W + 39 /r   (CMP r/m64, r64)
inline void emit_cmp_reg_reg(CodeBuffer& buf, Reg lhs, Reg rhs) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x39);
    emit_u8(buf, modrm_reg_reg(rhs, lhs));
}


inline void emit_test_reg_reg(CodeBuffer& buf, Reg reg) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x85);
    emit_u8(buf, modrm_reg_reg(reg, reg));
}

// ret
inline void emit_ret(CodeBuffer& buf) {
    emit_u8(buf, 0xC3);
}

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

// Once the target's final byte position in the buffer is known, call
// this to overwrite the placeholder with the real relative offset.
inline void resolve_jump_patch(CodeBuffer& buf, const JumpPatch& patch, size_t target_offset) {
    int32_t rel = static_cast<int32_t>(
        static_cast<int64_t>(target_offset) - static_cast<int64_t>(patch.instr_end_offset));
    patch_u32_at(buf, patch.rel32_offset, static_cast<uint32_t>(rel));
}

// jmp rel32 (unconditional). Encoding: E9 cd.
// Returns the JumpPatch so the caller can resolve it once the
// target block's position is known.
inline JumpPatch emit_jmp_rel32(CodeBuffer& buf) {
    emit_u8(buf, 0xE9);
    size_t rel32_offset = buf.size();
    emit_u8(buf, 0x00); emit_u8(buf, 0x00); emit_u8(buf, 0x00); emit_u8(buf, 0x00);
    return JumpPatch{rel32_offset, buf.size()};
}

enum class Cond : uint8_t { Less = 0x8C, Greater = 0x8F, Equal = 0x84, NotZero = 0x85 };

inline JumpPatch emit_jcc_rel32(CodeBuffer& buf, Cond cond) {
    emit_u8(buf, 0x0F);
    emit_u8(buf, static_cast<uint8_t>(cond));
    size_t rel32_offset = buf.size();
    emit_u8(buf, 0x00); emit_u8(buf, 0x00); emit_u8(buf, 0x00); emit_u8(buf, 0x00);
    return JumpPatch{rel32_offset, buf.size()};
}

inline void emit_disp32_le(CodeBuffer& buf, int32_t disp) {
    uint32_t bits = static_cast<uint32_t>(disp);
    emit_u8(buf, static_cast<uint8_t>(bits & 0xFF));
    emit_u8(buf, static_cast<uint8_t>((bits >> 8) & 0xFF));
    emit_u8(buf, static_cast<uint8_t>((bits >> 16) & 0xFF));
    emit_u8(buf, static_cast<uint8_t>((bits >> 24) & 0xFF));
}

inline void emit_store_rbp_offset(CodeBuffer& buf, Reg src, int32_t offset) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x89);
    emit_u8(buf, static_cast<uint8_t>(0x80 | (static_cast<uint8_t>(src) << 3) | static_cast<uint8_t>(Reg::RBP)));
    emit_disp32_le(buf, offset);
}

inline void emit_load_rbp_offset(CodeBuffer& buf, Reg dst, int32_t offset) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x8B);
    emit_u8(buf, static_cast<uint8_t>(0x80 | (static_cast<uint8_t>(dst) << 3) | static_cast<uint8_t>(Reg::RBP)));
    emit_disp32_le(buf, offset);
}

inline void emit_push_reg(CodeBuffer& buf, Reg reg) {
    emit_u8(buf, static_cast<uint8_t>(0x50 + static_cast<uint8_t>(reg)));
}

inline void emit_pop_reg(CodeBuffer& buf, Reg reg) {
    emit_u8(buf, static_cast<uint8_t>(0x58 + static_cast<uint8_t>(reg)));
}

inline void emit_sub_rsp_imm32(CodeBuffer& buf, int32_t imm) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x81);
    emit_u8(buf, static_cast<uint8_t>(0xC0 | (5 << 3) | static_cast<uint8_t>(Reg::RSP)));
    emit_disp32_le(buf, imm);
}

inline void emit_prologue(CodeBuffer& buf, int32_t frame_size) {
    emit_push_reg(buf, Reg::RBP);
    emit_mov_reg_reg(buf, Reg::RBP, Reg::RSP);
    if (frame_size > 0) {
        emit_sub_rsp_imm32(buf, frame_size);
    }
}

inline void emit_epilogue(CodeBuffer& buf) {
    emit_mov_reg_reg(buf, Reg::RSP, Reg::RBP);
    emit_pop_reg(buf, Reg::RBP);
}

inline void emit_setcc(CodeBuffer& buf, Cond cond, Reg dst_low_byte) {
    emit_u8(buf, 0x0F);
    emit_u8(buf, static_cast<uint8_t>(static_cast<uint8_t>(cond) + 0x10));
    emit_u8(buf, static_cast<uint8_t>(0xC0 | static_cast<uint8_t>(dst_low_byte)));
}

inline void emit_movzx_reg_reg8(CodeBuffer& buf, Reg dst64, Reg src_low_byte) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0xB6);
    emit_u8(buf, modrm_reg_reg(dst64, src_low_byte));
}

} // namespace lithon::jit
