// 1.2  CPUID feature detection: bit-mapping and OS-enablement tests.
//
// The runtime detection can only be checked against the CPU it runs on, so
// the bit mapping is unit-tested against synthetic leaf values from CPU
// generations this host is not:
//
//   Nehalem (2008)    SSE4.2, no AVX
//   Ivy Bridge (2012) SSE4.2 + AVX, no AVX2/FMA      (this repo's CI host class)
//   Haswell (2013)    + AVX2 + FMA
//   Skylake-X (2017)  + AVX-512 F/DQ/CD/BW/VL
//
// plus two OS-enablement cases where the CPU advertises a feature but XCR0
// does not enable its register state -- the case that makes "usable_avx"
// different from the raw CPUID bit and the reason a codegen path must check
// the former.
//
//   cpu_features_test            -- assertions
//   cpu_features_test --dump     -- machine-readable flags for the host,
//                                   compared against /proc/cpuinfo by
//                                   tools/check_cpu_features.py

#include <cstdio>
#include <cstring>
#include <string>

#include "cpu_features.h"

using namespace lithon::jit::cpu_features;

namespace {

constexpr uint32_t ECX_SSE3     = 1u << 0;
constexpr uint32_t ECX_SSSE3    = 1u << 9;
constexpr uint32_t ECX_FMA      = 1u << 12;
constexpr uint32_t ECX_SSE41    = 1u << 19;
constexpr uint32_t ECX_SSE42    = 1u << 20;
constexpr uint32_t ECX_OSXSAVE  = 1u << 27;
constexpr uint32_t ECX_AVX      = 1u << 28;
constexpr uint32_t EDX_SSE2     = 1u << 26;

constexpr uint32_t EBX_AVX2      = 1u << 5;
constexpr uint32_t EBX_AVX512F   = 1u << 16;
constexpr uint32_t EBX_AVX512DQ  = 1u << 17;
constexpr uint32_t EBX_AVX512CD  = 1u << 28;
constexpr uint32_t EBX_AVX512BW  = 1u << 30;
constexpr uint32_t EBX_AVX512VL  = 1u << 31;

constexpr uint32_t SSE_BASELINE = ECX_SSE3 | ECX_SSSE3 | ECX_SSE41 | ECX_SSE42;
constexpr uint64_t XCR0_AVX     = 0x6;      // XCR0[2:1]
constexpr uint64_t XCR0_AVX512  = 0xE6;     // XCR0[7:5] plus AVX

int failures = 0;

void check(const char* what, bool ok, const char* detail = "") {
    std::printf("  [%s] %s%s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok) ++failures;
}

} // namespace

int main(int argc, char** argv) {
    const bool dump = argc > 1 && std::strcmp(argv[1], "--dump") == 0;
    if (dump) {
        const Features& f = get();
        std::printf("vendor=%s\n", f.vendor);
        std::printf("max_leaf=%u\n", f.max_leaf);
        std::printf("max_subleaf7=%u\n", f.max_subleaf7);
        std::printf("sse2=%d\n", f.sse2);
        std::printf("sse3=%d\n", f.sse3);
        std::printf("ssse3=%d\n", f.ssse3);
        std::printf("sse41=%d\n", f.sse41);
        std::printf("sse42=%d\n", f.sse42);
        std::printf("osxsave=%d\n", f.osxsave);
        std::printf("avx=%d\n", f.avx);
        std::printf("fma=%d\n", f.fma);
        std::printf("avx2=%d\n", f.avx2);
        std::printf("avx512f=%d\n", f.avx512f);
        std::printf("avx512dq=%d\n", f.avx512dq);
        std::printf("avx512cd=%d\n", f.avx512cd);
        std::printf("avx512bw=%d\n", f.avx512bw);
        std::printf("avx512vl=%d\n", f.avx512vl);
        std::printf("usable_avx=%d\n", f.usable_avx);
        std::printf("usable_avx512=%d\n", f.usable_avx512);
        const char* lv[] = {"scalar", "sse2", "avx", "avx2", "avx512"};
        std::printf("simd_level=%s\n", lv[static_cast<int>(best_simd())]);
        return 0;
    }

    std::printf("synthetic CPUID leaves\n");

    // Nehalem: the last generation before AVX.
    {
        Features f = decode(SSE_BASELINE, EDX_SSE2, 0, 0, 0);
        check("nehalem: sse4.2 present", f.sse42);
        check("nehalem: no avx", !f.avx && !f.usable_avx);
        check("nehalem: no avx2", !f.avx2);
        check("nehalem: no avx512", !f.avx512f && !f.usable_avx512);
        check("nehalem: simd_level == sse2", simd_level(f) == SimdLevel::Sse2);
    }

    // Ivy Bridge: AVX, still no AVX2/FMA.
    {
        Features f = decode(SSE_BASELINE | ECX_OSXSAVE | ECX_AVX, EDX_SSE2, 0, 0, XCR0_AVX);
        check("ivy: avx usable", f.avx && f.usable_avx);
        check("ivy: no avx2", !f.avx2);
        check("ivy: no fma", !f.fma);
        check("ivy: simd_level == avx", simd_level(f) == SimdLevel::Avx);
    }

    // Haswell: AVX2 + FMA.
    {
        Features f = decode(SSE_BASELINE | ECX_OSXSAVE | ECX_AVX | ECX_FMA, EDX_SSE2,
                            EBX_AVX2, 0, XCR0_AVX);
        check("haswell: avx2 usable", f.avx2 && f.usable_avx);
        check("haswell: fma present", f.fma);
        check("haswell: no avx512", !f.avx512f);
        check("haswell: simd_level == avx2", simd_level(f) == SimdLevel::Avx2);
    }

    // Skylake-X: AVX-512 F/DQ/CD/BW/VL.
    {
        const uint32_t ebx = EBX_AVX2 | EBX_AVX512F | EBX_AVX512DQ | EBX_AVX512CD |
                             EBX_AVX512BW | EBX_AVX512VL;
        Features f = decode(SSE_BASELINE | ECX_OSXSAVE | ECX_AVX | ECX_FMA, EDX_SSE2,
                            ebx, 0, XCR0_AVX512);
        check("skylake-x: avx512f usable", f.avx512f && f.usable_avx512);
        check("skylake-x: avx512dq/bw/vl", f.avx512dq && f.avx512bw && f.avx512vl);
        check("skylake-x: simd_level == avx512", simd_level(f) == SimdLevel::Avx512);
    }

    // CPU advertises AVX but the OS never enabled it: still illegal to emit.
    {
        Features f = decode(SSE_BASELINE | ECX_AVX, EDX_SSE2, EBX_AVX2, 0, 0);
        check("avx cpuid, os disabled: raw avx set", f.avx);
        check("avx cpuid, os disabled: not usable", !f.usable_avx);
        check("avx cpuid, os disabled: avx2 not usable", !(f.usable_avx && f.avx2));
        check("avx cpuid, os disabled: level sse2", simd_level(f) == SimdLevel::Sse2);
    }

    // CPU advertises AVX-512F but the OS enabled only the YMM state.
    {
        Features f = decode(SSE_BASELINE | ECX_OSXSAVE | ECX_AVX, EDX_SSE2,
                            EBX_AVX512F, 0, XCR0_AVX);
        check("avx512 cpuid, os limited: raw avx512f set", f.avx512f);
        check("avx512 cpuid, os limited: not usable", !f.usable_avx512);
        check("avx512 cpuid, os limited: level avx", simd_level(f) == SimdLevel::Avx);
    }

    std::printf("host detection\n");
#if LITHON_CPU_FEATURES_X86
    const Features& f = get();
    std::printf("  vendor=%s max_leaf=%u\n", f.vendor, f.max_leaf);
    // x86-64 guarantees SSE2, so a false here means detection itself is broken.
    check("host reports SSE2", f.sse2);
    check("usable_avx implies raw avx", !f.usable_avx || f.avx);
    check("usable_avx implies OSXSAVE", !f.usable_avx || f.osxsave);
    check("usable_avx512 implies usable_avx", !f.usable_avx512 || f.usable_avx);
    check("detection is cached", &get() == &get());
    init();   // must be idempotent
    check("init() does not change the result", &get() == &get());
#else
    std::printf("  not an x86 host; SIMD features reported absent\n");
    check("non-x86: scalar level", best_simd() == SimdLevel::Scalar);
#endif

    if (failures) {
        std::printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("PASS: CPUID decode and host detection consistent\n");
    return 0;
}
