#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

// Host-side support for JIT-emitted float code.
//
// Two functions, both called by address from emitted machine code (the
// same way the JIT already calls std::printf):
//
//   host_report_error   -- a runtime error detected by emitted code
//   host_format_double  -- a double rendered the way CPython renders it
//
// They live here, not in the interpreter, because the requirement is
// that a JIT program and an interpreted one behave IDENTICALLY: same
// stdout, same stderr, same exit status. run_tier_diff.py diffs the two
// engines byte-for-byte, so any divergence between them is reported as
// a JIT bug even when the JIT is the more correct of the two.

namespace lithon::jit {

// Reports a runtime error detected by emitted code (currently only
// division by zero) on stderr, then exits. Never returns: the emitted
// code has no way to resume, and falling through would execute
// whatever instruction happens to follow the call.
//
// This is the JIT's counterpart to the interpreter's
// `throw std::runtime_error("interpreter: division by zero")`, which
// hello.cpp catches and reports as "error: ..." before returning 1.
// Both paths therefore produce the same stderr text and the same exit
// status, which is what lets run_tier_diff.py treat the interpreter as
// an oracle for float code.
[[noreturn]] inline void host_report_error(const char* message) {
    std::fputs(message, stderr);
    std::fflush(stderr);
    std::fflush(stdout);
    std::exit(1);
}

// Renders a double exactly as CPython's repr() would, into a static
// buffer, and returns a pointer to it.
//
// Why not "%f": CPython uses repr(), which is the SHORTEST decimal
// string that round-trips to the same double. So 3.5 renders as "3.5"
// and 7.0 as "7.0", where "%f" would produce "3.500000" and
// "7.000000". Since the oracle is CPython (via the interpreter), "%f"
// would fail every float comparison.
//
// Why not "%.17g": always round-trips, but is not SHORTEST -- 0.1 would
// render as "0.10000000000000001" instead of "0.1". Correct but
// wrong-looking, and it would also fail the byte-for-byte diff.
//
// The algorithm below is the standard way to get shortest-roundtrip
// without linking a dtoa library: ask for increasing numbers of significant
// digits and keep the first that reads back bit-identical. One digit is
// tried first, so the common case (a short decimal like 3.5) exits after a
// single try.
//
// The digits are obtained in *scientific* form and then laid out by hand,
// because that is the only way to match CPython's fixed-vs-exponential
// choice. This is not a detail: "%g" decides based on the PRECISION it was
// given, so the same value formats differently depending on how many digits
// were needed to round-trip. %.*g rendered 924966630.0 as "9.2499663e+08"
// (nine significant digits, so exponent form won) where CPython prints
// "924996630.0". CPython instead uses one absolute rule: exponential only
// when the decimal point would land at position < -4 or > 16, which is why
// 1e15 prints as "1000000000000000.0" but 1e16 prints as "1e+16".
//
// The buffer is static and therefore not thread-safe. That is a real
// limitation, stated rather than hidden: emitted code here runs on one
// thread, and a reentrant case would need the caller to own the buffer.
inline char* host_format_double(double value) {
    static char buffer[64];

    // Non-finite values have their own spellings, and no finite
    // precision-based rendering produces them. Python prints these
    // exactly as below (str(float('inf')) == 'inf', and 'nan').
    if (std::isnan(value)) { std::strcpy(buffer, "nan"); return buffer; }
    if (std::isinf(value)) {
        std::strcpy(buffer, value < 0 ? "-inf" : "inf");
        return buffer;
    }

    // Step 1: the shortest digit string that round-trips, in scientific
    // form. "%.*e" is used rather than "%.*g" precisely because it never
    // chooses a layout for us: it always emits d[.ddd]e[+-]XX, so the
    // significant digits and the decimal exponent are available separately
    // and step 2 can apply CPython's rule to them. 17 significant digits is
    // always sufficient for IEEE754 binary64, so the loop always terminates.
    //
    // The round-trip test compares bits rather than values so that a
    // candidate which happens to parse back to a different NaN payload is
    // correctly rejected, and so that -0.0 does not compare equal to 0.0.
    char sci[64];
    for (int digits = 1; digits <= 17; ++digits) {
        std::snprintf(sci, sizeof(sci), "%.*e", digits - 1, value);
        double round_trip = 0.0;
        std::sscanf(sci, "%lf", &round_trip);
        if (std::memcmp(&round_trip, &value, sizeof(double)) == 0) break;
    }

    // Step 2: split "[-]D[.DDD]e[+-]XX" into sign, significant digits
    // (leading zeros stripped, since the digit count is the significant
    // count) and the exponent.
    const char* p = sci;
    const bool negative = (*p == '-');
    if (negative) ++p;

    char digits[20];
    size_t ndigits = 0;
    bool seen_nonzero = false;
    for (; *p != 'e' && *p != 'E'; ++p) {
        if (*p == '.') continue;
        // A leading zero is not a significant digit, but a zero *after* the
        // first nonzero one is (0.001001 needs its interior zeros).
        if (*p == '0' && !seen_nonzero) continue;
        seen_nonzero = true;
        digits[ndigits++] = *p;
    }
    digits[ndigits] = '\0';
    int exponent = std::atoi(p + 1);

    // "-0.0" is the only case where every digit was a leading zero: the
    // value is zero, and Python still shows a decimal point and keeps the
    // sign.
    if (ndigits == 0) {
        std::strcpy(buffer, negative ? "-0.0" : "0.0");
        return buffer;
    }

    // decpt is the position of the decimal point relative to the start of
    // the digit string, matching CPython's own terminology.
    const int decpt = exponent + 1;

    char* out = buffer;
    if (negative) *out++ = '-';

    // CPython's rule: scientific notation only when the decimal point would
    // fall outside (-4, 16]. Verified against repr() for 1e-5 ("1e-05")
    // through 1e15 ("1000000000000000.0") and 1e16 ("1e+16").
    if (decpt <= -4 || decpt > 16) {
        *out++ = digits[0];
        if (ndigits > 1) {
            *out++ = '.';
            std::memcpy(out, digits + 1, ndigits - 1);
            out += ndigits - 1;
        }
        *out++ = 'e';
        int e = decpt - 1;
        *out++ = e < 0 ? '-' : '+';
        e = e < 0 ? -e : e;
        // CPython pads the exponent to at least two digits: "1e-05", not
        // "1e-5". The tens digit is written as a literal 0 and the units
        // digit is written separately, so a three-digit exponent still goes
        // through snprintf below rather than being truncated.
        if (e < 10) {
            *out++ = '0';
            *out++ = static_cast<char>('0' + e);
        } else {
            out += std::snprintf(out, 12, "%d", e);
        }
    } else if (decpt <= 0) {
        // 0.000ddd -- more leading zeros than digits.
        *out++ = '0';
        *out++ = '.';
        for (int i = 0; i < -decpt; ++i) *out++ = '0';
        std::memcpy(out, digits, ndigits);
        out += ndigits;
    } else if (static_cast<size_t>(decpt) >= ndigits) {
        // All digits are to the left of the decimal point; the rest of the
        // integer part is zeros, and Python still shows a ".0" so that a
        // float never renders as an int.
        std::memcpy(out, digits, ndigits);
        out += ndigits;
        for (size_t i = ndigits; i < static_cast<size_t>(decpt); ++i) *out++ = '0';
        *out++ = '.';
        *out++ = '0';
    } else {
        std::memcpy(out, digits, decpt);
        out += decpt;
        *out++ = '.';
        std::memcpy(out, digits + decpt, ndigits - decpt);
        out += ndigits - decpt;
    }
    *out = '\0';
    return buffer;
}

} // namespace lithon::jit
