// host_format_double against CPython's repr(), transcribed by hand.
//
// The JIT and the interpreter both call this one function, so it is the
// single point where a formatting difference becomes an observable
// interpreter-vs-JIT mismatch (and, since run_tier_diff.py diffs stdout
// byte-for-byte, a "JIT bug" that is really a formatter bug).
//
// Every expectation below is what `python3 -c 'print(repr(v))'` prints. They
// are transcribed rather than generated, because generating them from this
// code would only prove the code agrees with itself.
//
// The cases are grouped by the rule they pin, because each group corresponds
// to a rule that has been gotten wrong:
//
//   * shortest round-trip digits, so 1/3 is not 0.333333;
//   * the fixed-vs-exponential threshold, which is ABSOLUTE in CPython
//     (decimal exponent < -4 or > 16) and not a function of precision;
//   * non-finite spellings and signed zero;
//   * the always-present decimal point, so a float never prints as an int.
//
// Portability: pure C++ with no JIT or interpreter dependency, so this builds
// and runs on every platform, not just x86-64.

#include "float_runtime.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using lithon::jit::host_format_double;

static int failures = 0;

static void expect(double value, const char* want, const char* what) {
    const char* got = host_format_double(value);
    if (std::strcmp(got, want) == 0) return;
    std::fprintf(stderr, "FAIL %s\n  want: %s\n  got : %s\n", what, want, got);
    ++failures;
}

int main() {
    // --- shortest round-trip digits ------------------------------------
    // The default ostream precision is 6 significant digits, which is how the
    // interpreter used to render 1/3 as "0.333333".
    expect(1.0 / 3.0, "0.3333333333333333", "1/3 keeps all 16 significant digits");
    expect(0.1 + 0.2, "0.30000000000000004", "0.1+0.2 is not 0.3");
    expect(2.0 / 3.0, "0.6666666666666666", "2/3");
    expect(1.0000000000000002, "1.0000000000000002",
           "a double one ULP above 1 needs all 17 digits and is not truncated");
    // One digit is enough here, so the search must stop at one and not
    // pad out to 17.
    expect(3.5, "3.5", "3.5 is exactly representable in one digit");
    expect(0.5, "0.5", "0.5");

    // --- the always-present decimal point ------------------------------
    expect(1.0, "1.0", "a whole-valued float prints 1.0, not 1");
    expect(7.0, "7.0", "7.0");
    expect(-3.0, "-3.0", "-3.0");
    expect(0.0, "0.0", "zero is a float here");
    expect(-0.0, "-0.0", "negative zero keeps its sign");
    expect(1e16, "1e+16", "1e16 is at the exponent threshold and gets no .0");
    expect(100.0, "100.0", "a 3-digit integer-valued float");

    // --- the fixed-vs-exponential threshold ----------------------------
    // CPython's rule is absolute: exponent form only when the decimal point
    // would fall outside (-4, 16]. Letting %g decide by the precision it
    // happened to need is what made 1e15 print as "1e+15" and
    // 924996630.0 as "9.2499663e+08".
    expect(1e15, "1000000000000000.0", "1e15 is still fixed notation");
    expect(1e-4, "0.0001", "1e-4 is the smallest fixed-notation exponent");
    expect(1e-5, "1e-05", "1e-5 crosses into exponent form");
    expect(1e16, "1e+16", "1e16 crosses into exponent form");
    expect(1e17, "1e+17", "1e17");
    expect(1e100, "1e+100", "1e100 keeps three exponent digits");
    expect(1e-100, "1e-100", "1e-100 keeps three exponent digits");
    expect(1e-5, "1e-05", "exponents are zero-padded to two digits");
    // The smallest and largest magnitudes share a threshold with the middle,
    // which is what makes the rule a function of exponent and not of value:
    // all three of these round-trip at full precision and all three would have
    // been truncated to 6 significant digits by the old ostream default.
    expect(1.2345678901234567e-4, "0.00012345678901234567",
           "a subnormal-range exponent still uses full significant digits");
    expect(0.00012345678901234567, "0.00012345678901234567",
           "and the same value written literally");
    expect(123456789012345.67, "123456789012345.67",
           "15 significant digits is still fixed notation");    // The whole-valued case that specifically broke the %g path: nine
    // significant digits, so %g would have preferred exponent form, but
    // CPython prints it in full.
    expect(924996630.0, "924996630.0", "9 digits does not force exponent form");
    expect(-924996630.0, "-924996630.0", "and with a sign");
    expect(100000000000000.0, "100000000000000.0", "1e14 is 15 digits, still fixed");
    expect(0.0001, "0.0001", "the small end of the fixed range");

    // --- subnormals ---------------------------------------------------
    // std::stod used to reject every one of these with "error: stod", because
    // glibc reports ERANGE for a subnormal *result* and stod turns ERANGE into
    // an exception. The formatter has to handle them, and the parser has to
    // stop refusing them.
    expect(5e-324, "5e-324", "the smallest positive subnormal");
    expect(1e-309, "1e-309", "a subnormal below 2.2e-308");
    expect(2.2250738585072014e-308, "2.2250738585072014e-308",
           "the smallest normal double");
    expect(-5e-324, "-5e-324", "negative smallest subnormal");

    // --- extremes and non-finite --------------------------------------
    // Note the "+": CPython writes a three-digit exponent as e+308, and a
    // reader expecting the shorter "1.7976931348623157e308" would be wrong.
    expect(1.7976931348623157e308, "1.7976931348623157e+308",
           "the largest finite double round-trips");
    expect(-1.7976931348623157e308, "-1.7976931348623157e+308", "and negative");
    expect(std::numeric_limits<double>::infinity(), "inf", "positive infinity");
    expect(-std::numeric_limits<double>::infinity(), "-inf", "negative infinity");
    // The sign of a NaN is not something CPython prints; both spellings
    // collapse to "nan", which is what str() and repr() both produce.
    expect(std::numeric_limits<double>::quiet_NaN(), "nan", "NaN");
    expect(-std::numeric_limits<double>::quiet_NaN(), "nan",
           "a negative NaN still prints as nan, without the sign");

    // --- results that must be exact integers, not truncated ------------
    expect(1.0 / 4.0, "0.25", "exact quarters");
    expect(1.0 / 8.0, "0.125", "exact eighths");
    expect(3.0, "3.0", "3.0");

    if (failures) {
        std::fprintf(stderr, "\n%d float formatting check(s) wrong\n", failures);
        return 1;
    }
    std::printf("PASS: host_format_double matches CPython repr on all cases\n");
    return 0;
}
