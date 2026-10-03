#pragma once
//
// 1.2  Runtime CPUID target-feature detection.
//
// An AVX2 instruction on a host without AVX2 is SIGILL, not a slow path.
// That is the exact opposite of "refuses what it cannot prove": the program
// does not trap because Lithon diagnosed something, it traps because it
// executed an illegal instruction the compiler chose to emit. So no SIMD
// codegen path may be enabled from a build flag or a compile-time #ifdef;
// it must ask this header at run time and fall back to scalar when the
// feature is absent.
//
// This header exists before any AVX codegen on purpose -- the guardrail is
// cheapest to add while there is nothing that can violate it yet.
//
// Detection executes CPUID once:
//   * leaf 0        -- max basic leaf, vendor string
//   * leaf 1        -- ECX/EDX: SSE3/SSSE3/SSE4.1/SSE4.2, OSXSAVE, AVX, FMA
//   * leaf 7 sub 0  -- EBX/ECX: AVX2, AVX-512 F/DQ/BW/CD/VL
// and XGETBV(0) for XCR0, which is what actually decides whether the OS has
// enabled the wider register state. A CPU can advertise AVX (or AVX-512) in
// CPUID while the OS leaves it disabled; executing it is still SIGILL. The
// `has_*()` surface therefore reports *usable* features, and the raw CPUID
// bits stay visible on Features for diagnostics.
//
// The decoded result is cached in a function-local static, so it is
// computed exactly once (thread-safe by C++11) no matter how many codegen
// paths query it. call init() from main() if you want it out of the way
// before any compilation happens.

#include <cstdint>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  define LITHON_CPU_FEATURES_X86 1
#else
#  define LITHON_CPU_FEATURES_X86 0
#endif

#if LITHON_CPU_FEATURES_X86
#  if defined(_MSC_VER)
#    include <intrin.h>
#  else
#    include <cpuid.h>
#  endif
#endif

namespace lithon::jit::cpu_features {

struct Features {
    // Raw leaves, kept verbatim so tests can assert on exactly what the CPU
    // reported and a dump tool can show it.
    uint32_t max_leaf = 0;
    uint32_t max_subleaf7 = 0;
    uint32_t leaf1_ecx = 0;
    uint32_t leaf1_edx = 0;
    uint32_t leaf7_ebx = 0;
    uint32_t leaf7_ecx = 0;
    uint64_t xcr0 = 0;
    char vendor[13] = {0};

    // Raw CPUID bits.
    bool sse2 = false;
    bool sse3 = false;
    bool ssse3 = false;
    bool sse41 = false;
    bool sse42 = false;
    bool osxsave = false;
    bool avx = false;
    bool fma = false;
    bool avx2 = false;
    bool avx512f = false;
    bool avx512dq = false;
    bool avx512bw = false;
    bool avx512cd = false;
    bool avx512vl = false;

    // CPUID bit AND the OS actually having enabled the register state.
    bool usable_avx = false;
    bool usable_avx512 = false;
};

// Pure decode of the raw values into flags. Split out from detect() so the
// bit mapping can be unit-tested against synthetic leaves from CPU
// generations this host is not (Nehalem, Haswell, Skylake-X, ...).
//
// XCR0 bits: [1]=SSE, [2]=AVX, [5]=opmask, [6]=ZMM_Hi256, [7]=Hi16_ZMM.
inline Features decode(uint32_t leaf1_ecx, uint32_t leaf1_edx,
                       uint32_t leaf7_ebx, uint32_t leaf7_ecx,
                       uint64_t xcr0) {
    Features f;
    f.leaf1_ecx = leaf1_ecx;
    f.leaf1_edx = leaf1_edx;
    f.leaf7_ebx = leaf7_ebx;
    f.leaf7_ecx = leaf7_ecx;
    f.xcr0 = xcr0;

    f.sse2  = (leaf1_edx >> 26) & 1u;
    f.sse3  = (leaf1_ecx >> 0)  & 1u;
    f.ssse3 = (leaf1_ecx >> 9)  & 1u;
    f.sse41 = (leaf1_ecx >> 19) & 1u;
    f.sse42 = (leaf1_ecx >> 20) & 1u;
    f.osxsave = (leaf1_ecx >> 27) & 1u;
    f.avx   = (leaf1_ecx >> 28) & 1u;
    f.fma   = (leaf1_ecx >> 12) & 1u;

    f.avx2     = (leaf7_ebx >> 5)  & 1u;
    f.avx512f  = (leaf7_ebx >> 16) & 1u;
    f.avx512dq = (leaf7_ebx >> 17) & 1u;
    f.avx512cd = (leaf7_ebx >> 28) & 1u;
    f.avx512bw = (leaf7_ebx >> 30) & 1u;
    f.avx512vl = (leaf7_ebx >> 31) & 1u;

    const bool xmm_ymm = (xcr0 & 0x6u) == 0x6u;     // XCR0[2:1] == 11
    const bool zmm = (xcr0 & 0xE0u) == 0xE0u;       // XCR0[7:5] == 111
    f.usable_avx = f.avx && f.osxsave && xmm_ymm;
    f.usable_avx512 = f.usable_avx && f.avx512f && zmm;
    return f;
}

#if LITHON_CPU_FEATURES_X86

inline void cpuid_leaf(uint32_t leaf, uint32_t subleaf,
                       uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
#if defined(_MSC_VER)
    int regs[4];
    __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(subleaf));
    a = static_cast<uint32_t>(regs[0]); b = static_cast<uint32_t>(regs[1]);
    c = static_cast<uint32_t>(regs[2]); d = static_cast<uint32_t>(regs[3]);
#else
    __cpuid_count(leaf, subleaf, a, b, c, d);
#endif
}

inline uint64_t read_xcr0() {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    uint32_t lo = 0, hi = 0;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
}

#else   // not x86: a Lithon build here never emits SIMD; report nothing.

inline void cpuid_leaf(uint32_t, uint32_t, uint32_t& a, uint32_t& b,
                       uint32_t& c, uint32_t& d) { a = b = c = d = 0; }
inline uint64_t read_xcr0() { return 0; }

#endif

inline Features detect() {
    Features f;
#if LITHON_CPU_FEATURES_X86
    uint32_t a = 0, b = 0, c = 0, d = 0;
    cpuid_leaf(0, 0, a, b, c, d);
    f.max_leaf = a;
    std::memcpy(f.vendor + 0, &b, 4);
    std::memcpy(f.vendor + 4, &d, 4);
    std::memcpy(f.vendor + 8, &c, 4);
    f.vendor[12] = '\0';

    uint32_t l1a = 0, l1b = 0, l1c = 0, l1d = 0;
    if (f.max_leaf >= 1) cpuid_leaf(1, 0, l1a, l1b, l1c, l1d);

    uint32_t l7a = 0, l7b = 0, l7c = 0, l7d = 0;
    if (f.max_leaf >= 7) {
        cpuid_leaf(7, 0, l7a, l7b, l7c, l7d);
        f.max_subleaf7 = l7a;
    }

    uint64_t xcr0 = 0;
    if (l1c & (1u << 27)) xcr0 = read_xcr0();   // XGETBV is legal only if OSXSAVE

    Features decoded = decode(l1c, l1d, l7b, l7c, xcr0);
    decoded.max_leaf = f.max_leaf;
    decoded.max_subleaf7 = f.max_subleaf7;
    std::memcpy(decoded.vendor, f.vendor, sizeof(f.vendor));
    return decoded;
#else
    return f;
#endif
}

// Process-wide, detect-once feature set. C++11 guarantees a single
// thread-safe initialization of a function-local static, so the CPUID
// executes once however many callers ask.
inline const Features& get() {
    static const Features cached = detect();
    return cached;
}

// Explicit early initialisation for lithon_jit.cpp's main(). Idempotent.
inline void init() { (void)get(); }

// Usable-feature queries. These are what SIMD codegen must branch on.
inline bool has_sse2()     { return get().sse2; }
inline bool has_sse41()    { return get().sse41; }
inline bool has_sse42()    { return get().sse42; }
inline bool has_avx()      { return get().usable_avx; }
inline bool has_avx2()     { return get().usable_avx && get().avx2; }
inline bool has_fma()      { return get().usable_avx && get().fma; }
inline bool has_avx512f()  { return get().usable_avx512; }
inline bool has_avx512dq() { return get().usable_avx512 && get().avx512dq; }
inline bool has_avx512bw() { return get().usable_avx512 && get().avx512bw; }
inline bool has_avx512vl() { return get().usable_avx512 && get().avx512vl; }

// The widest vector width that can safely be used. Codegen consults this so
// "AVX-512 on an AVX2 host" degrades to AVX2 rather than to SIGILL. The pure
// overload takes the decoded set explicitly so it is testable off-host.
enum class SimdLevel { Scalar, Sse2, Avx, Avx2, Avx512 };
inline SimdLevel simd_level(const Features& f) {
    if (f.usable_avx512) return SimdLevel::Avx512;
    if (f.usable_avx && f.avx2) return SimdLevel::Avx2;
    if (f.usable_avx) return SimdLevel::Avx;
    if (f.sse2) return SimdLevel::Sse2;
    return SimdLevel::Scalar;
}
inline SimdLevel best_simd() { return simd_level(get()); }

} // namespace lithon::jit::cpu_features
