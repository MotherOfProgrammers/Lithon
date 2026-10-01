#pragma once

#include <array>
#include "x86_encoder.h"

// Per-platform register roles for Lithon's x86-64 JIT.
//
// Both supported host ABIs are handled here so the rest of the code
// generator never mentions an OS:
//
//                   System V (Linux/macOS)       Microsoft x64 (Windows)
//   integer args    rdi, rsi, ...                rcx, rdx, r8, r9
//   callee-saved    rbx rbp r12-r15              rbx rbp rdi rsi r12-r15 (+xmm6-15)
//   shadow space    none                         32 bytes above the return address
//
// Register roles chosen so BOTH ABIs are satisfied by one design:
//   * promoted variables live in {rbx, r12-r15}: callee-saved on both
//     ABIs, so they survive calls into printf/other JIT functions.
//     Each function saves/restores the ones it uses in its own frame.
//   * temporaries live in a caller-saved pool that never contains an
//     argument register, so marshalling call arguments can never
//     clobber a source operand.
//   * r10 / r11 are permanent scratch (never allocated to a value):
//     r10 = left operand / spilled result, r11 = right operand /
//     indirect call target. Caller-saved on both ABIs.
//   * rdi/rsi are never touched on Windows (callee-saved there).

namespace lithon::jit::abi {

#if defined(_WIN32)
inline constexpr Reg kArgRegs[2] = {Reg::RCX, Reg::RDX};
inline constexpr std::array<Reg, 3> kTempPool = {Reg::RAX, Reg::R8, Reg::R9};
inline constexpr int kShadowSpace = 32;
#else
inline constexpr Reg kArgRegs[2] = {Reg::RDI, Reg::RSI};
inline constexpr std::array<Reg, 5> kTempPool = {Reg::RAX, Reg::RCX, Reg::RDX, Reg::R8, Reg::R9};
inline constexpr int kShadowSpace = 0;
#endif

inline constexpr std::array<Reg, 5> kPromotionPool = {
    Reg::RBX, Reg::R12, Reg::R13, Reg::R14, Reg::R15};

inline constexpr Reg kScratchLeft = Reg::R10;
inline constexpr Reg kScratchRight = Reg::R11;

// ---------------------------------------------------------------------
// SSE2 double-precision register roles.
//
// A structurally separate pool from the GP pools above, not an
// extension of kPromotionPool: an XMM register holds 128 bits of
// double-precision data and shares no storage with a 64-bit GP
// register, so a float value can never occupy a slot that a GP
// temporary is using. Mixing them in one array would make every
// existing "is this slot free?" query wrong.
//
//   * float temporaries live in XMM0-XMM5: all caller-saved on BOTH
//     ABIs, so they never need saving, and none is an argument
//     register, so marshalling printf's double in XMM0 can never
//     clobber a source operand.
//   * XMM6-XMM15 are callee-saved on Microsoft x64 but only
//     XMM8-XMM15 on System V, so they are left alone. This is the one
//     place the two ABIs genuinely disagree, and staying inside
//     XMM0-XMM7 sidesteps it entirely.
//   * XMM6/XMM7 would additionally be wrong on SysV as promotion
//     registers for a different reason: nothing in the current design
//     needs six simultaneously-live float temporaries.
//   * XMM15 is reserved as the permanent scratch, mirroring how r10
//     and r11 are reserved on the GP side. It is caller-saved on both
//     ABIs, so using it as scratch costs nothing.
inline constexpr std::array<Xmm, 6> kFloatTempPool = {
    Xmm::XMM0, Xmm::XMM1, Xmm::XMM2, Xmm::XMM3, Xmm::XMM4, Xmm::XMM5};

// Two reserved XMM scratch registers, never allocated to a value. Both are
// caller-saved on System V and Microsoft x64, so using them costs no
// prologue/epilogue work. Two rather than one because every float op here is
// two-operand and the register allocator's destination may be the same
// register as one of the operands: staging both operands in scratch first is
// what makes the sequence correct without a copy in every case.
inline constexpr Xmm kScratchFloat = Xmm::XMM15;
inline constexpr Xmm kScratchFloatB = Xmm::XMM14;
// A third, used only as the zero operand of the Div-by-zero comisd. XMM13 is
// outside kFloatTempPool and is never written by anything else.
inline constexpr Xmm kScratchFloatZero = Xmm::XMM13;

// Slot 0 of the float argument registers. Under SysV variadic calling
// convention a double argument is passed in the first XMM register;
// under Microsoft x64 the same register is used for all FP arguments.
inline constexpr Xmm kFloatArgReg = Xmm::XMM0;

} // namespace lithon::jit::abi
