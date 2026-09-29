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

} // namespace lithon::jit::abi
