#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

// Zero-dependency x86-64 instruction encoder. Hand-written, no
// external assembler or library. Covers exactly the register set
// and instruction subset Lithon's v1 IR needs.
//
// Deliberately restricted to the eight "legacy" 64-bit registers
// (rax/rcx/rdx/rbx/rsp/rbp/rsi/rdi) -- this keeps REX prefix
// encoding trivial (always 0x48, no extension bits). Extending to
// r8-r15 later is a real but bounded addition (needs REX.B/R/X bits).

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

// mov dst, src  (64-bit register to register)
// Encoding: REX.W + 89 /r   (MOV r/m64, r64 -- src is the "reg" field,
// dst is the "r/m" field). Verified: mov rax, rdi = 48 89 f8.
inline void emit_mov_reg_reg(CodeBuffer& buf, Reg dst, Reg src) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x89);
    emit_u8(buf, modrm_reg_reg(src, dst));
}

// mov dst, imm64
// Encoding: REX.W + B8+r io  (MOV r64, imm64 -- register encoded
// directly in the opcode byte, followed by 8 immediate bytes).
inline void emit_mov_reg_imm64(CodeBuffer& buf, Reg dst, int64_t imm) {
    emit_u8(buf, 0x48);
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
// Encoding: REX.W + 0F AF /r   (IMUL r64, r/m64 -- operand order is
// REVERSED from add/sub: dst is the "reg" field here)
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

// test reg, reg  (64-bit) -- ANDs the operand with itself purely to
// set flags (ZF set iff reg == 0), without modifying either operand.
// Encoding: REX.W + 85 /r (TEST r/m64, r64).
// Verified: test rax, rax = 48 85 c0.
inline void emit_test_reg_reg(CodeBuffer& buf, Reg reg) {
    emit_u8(buf, 0x48);
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
    emit_u8(buf, 0x00); emit_u8(buf, 0x00); emit_u8(buf, 0x00); emit_u8(buf, 0x00);
    return JumpPatch{rel32_offset, buf.size()};
}

// Conditional jumps, following a preceding cmp or test. Encoding:
// 0F 8x cd. jl=0x8C, jg=0x8F, je=0x84, jne(jnz)=0x85.
enum class Cond : uint8_t { Less = 0x8C, Greater = 0x8F, Equal = 0x84, NotZero = 0x85 };

inline JumpPatch emit_jcc_rel32(CodeBuffer& buf, Cond cond) {
    emit_u8(buf, 0x0F);
    emit_u8(buf, static_cast<uint8_t>(cond));
    size_t rel32_offset = buf.size();
    emit_u8(buf, 0x00); emit_u8(buf, 0x00); emit_u8(buf, 0x00); emit_u8(buf, 0x00);
    return JumpPatch{rel32_offset, buf.size()};
}

// --- Stack-relative addressing, for spilled values and locals ---
// Always uses the disp32 ModRM form (mod=10), not the shorter disp8
// form -- one uniform code path, correct for any offset magnitude.

inline void emit_disp32_le(CodeBuffer& buf, int32_t disp) {
    uint32_t bits = static_cast<uint32_t>(disp);
    emit_u8(buf, static_cast<uint8_t>(bits & 0xFF));
    emit_u8(buf, static_cast<uint8_t>((bits >> 8) & 0xFF));
    emit_u8(buf, static_cast<uint8_t>((bits >> 16) & 0xFF));
    emit_u8(buf, static_cast<uint8_t>((bits >> 24) & 0xFF));
}

// mov [rbp + offset], src   (store to a stack slot)
// Encoding: REX.W + 89 /r, ModRM(mod=10, reg=src, rm=RBP), disp32
inline void emit_store_rbp_offset(CodeBuffer& buf, Reg src, int32_t offset) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x89);
    emit_u8(buf, static_cast<uint8_t>(0x80 | (static_cast<uint8_t>(src) << 3) | static_cast<uint8_t>(Reg::RBP)));
    emit_disp32_le(buf, offset);
}

// mov dst, [rbp + offset]   (load from a stack slot)
// Encoding: REX.W + 8B /r, ModRM(mod=10, reg=dst, rm=RBP), disp32
inline void emit_load_rbp_offset(CodeBuffer& buf, Reg dst, int32_t offset) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x8B);
    emit_u8(buf, static_cast<uint8_t>(0x80 | (static_cast<uint8_t>(dst) << 3) | static_cast<uint8_t>(Reg::RBP)));
    emit_disp32_le(buf, offset);
}

// push reg / pop reg. Encoding: 0x50+r / 0x58+r.
inline void emit_push_reg(CodeBuffer& buf, Reg reg) {
    emit_u8(buf, static_cast<uint8_t>(0x50 + static_cast<uint8_t>(reg)));
}

inline void emit_pop_reg(CodeBuffer& buf, Reg reg) {
    emit_u8(buf, static_cast<uint8_t>(0x58 + static_cast<uint8_t>(reg)));
}

// sub rsp, imm32 -- always uses the imm32 form (REX.W + 81 /5 id).
inline void emit_sub_rsp_imm32(CodeBuffer& buf, int32_t imm) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x81);
    emit_u8(buf, static_cast<uint8_t>(0xC0 | (5 << 3) | static_cast<uint8_t>(Reg::RSP)));
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
// on the flags from a preceding cmp/test. Encoding: 0F 9x /0.
// The SETcc opcode is the matching Jcc opcode PLUS 0x10
// (jl=0x8C -> setl=0x9C; jg=0x8F -> setg=0x9F; je=0x84 -> sete=0x94).
// Only valid, without a REX prefix, for AL/CL/DL/BL (indices 0-3) --
// exactly RAX/RCX/RDX/RBX.
inline void emit_setcc(CodeBuffer& buf, Cond cond, Reg dst_low_byte) {
    emit_u8(buf, 0x0F);
    emit_u8(buf, static_cast<uint8_t>(static_cast<uint8_t>(cond) + 0x10));
    emit_u8(buf, static_cast<uint8_t>(0xC0 | static_cast<uint8_t>(dst_low_byte)));
}

// movzx dst64, src_low_byte -- zero-extends an 8-bit value into a
// full 64-bit register. Encoding: REX.W + 0F B6 /r.
inline void emit_movzx_reg_reg8(CodeBuffer& buf, Reg dst64, Reg src_low_byte) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0xB6);
    emit_u8(buf, modrm_reg_reg(dst64, src_low_byte));
}

// xor reg32, reg32 (self-xor to zero a register, including AL as its
// low byte). Used for the variadic-call ABI requirement that AL hold
// the count of vector registers used (0 -- no float args yet).
// Encoding: 31 /r. Verified: xor eax, eax = 31 c0.
inline void emit_xor_zero(CodeBuffer& buf, Reg reg) {
    emit_u8(buf, 0x31);
    emit_u8(buf, modrm_reg_reg(reg, reg));
}

// call reg (indirect call through a register holding a runtime
// address -- used to call host-process functions like printf).
// Encoding: FF /2 (CALL r/m64). Verified: call rax = ff d0.
inline void emit_call_reg(CodeBuffer& buf, Reg reg) {
    emit_u8(buf, 0xFF);
    emit_u8(buf, static_cast<uint8_t>(0xC0 | (2 << 3) | static_cast<uint8_t>(reg)));
}

} // namespace lithon::jit
