# Float printing, arithmetic, comparison and division semantics.
#
# Every case here is one that has actually been wrong at least once, which is
# why each print is a separate statement with a comment rather than a loop:
# a regression names itself in the diff.

# --- shortest-roundtrip formatting -------------------------------------
# 1/3 is 0.3333333333333333, not 0.333333. The default ostream precision of
# 6 significant digits used to produce the latter in the interpreter, which
# made correct JIT output look like a JIT bug.
print(1.0 / 3.0)
# 0.1 + 0.2 is the classic: the shortest string that reads back bit-identical
# is 0.30000000000000004.
print(0.1 + 0.2)
# 1.0 is a whole number but still a float, so it prints "1.0" and never "1".
print(1.0)
# Negative zero keeps its sign: Python prints "-0.0" and so must we.
print(-0.0)
# A value needing 17 significant digits must not be truncated.
print(1.0000000000000002)

# --- the fixed-vs-exponential threshold -------------------------------
# CPython switches to exponent form only below 1e-4 or at/above 1e16. The
# formatter used to let %g decide based on the precision it needed, so 1e15
# came out as "1e+15" and 924996630.0 as "9.2499663e+08".
print(1e15)
print(1e16)
print(1e-4)
print(1e-5)
# Subnormals: the smallest positive double, and one just below the normal
# range. std::stod used to reject both outright with "error: stod", because
# glibc reports ERANGE for a subnormal result and stod turns that into an
# exception.
print(5e-324)
print(1e-309)
print(2.2250738585072014e-308)
# The largest finite double.
print(1.7976931348623157e308)

# --- arithmetic --------------------------------------------------------
a = 3.5
b = 2.0
print(a + b)
print(a - b)
print(a * b)
print(a / b)
# Mixed int and float promotes: 7 + 0.5 is 7.5, an int result would be wrong.
print(7 + 0.5)
print(0.5 + 7)
# int / int is a float in this language, not an int.
print(7 / 2)

# --- comparison, including the NaN cases ------------------------------
# A float value that survives a print() call: this is the register-allocator
# case, where a float temp was being left in a caller-saved XMM that the host
# formatter then clobbered.
s = 0.0 * -0.0
r = 5
print(s * r > r)
# NaN. Produced without a literal because there is no NaN syntax: inf - inf,
# where inf comes from an overflow at run time (1e308 * 1e308).
big = 1e308
nan = big * big - big * big
# Every ordered comparison against NaN is False. This is where the unordered
# case (comisd sets ZF, PF and CF all at once) has to be excluded explicitly,
# or NaN would come out as less than everything.
print(nan < 1.0)
print(nan > 1.0)
print(nan == nan)
# A NaN divisor must NOT raise. Python propagates: 1.0/nan is nan. The zero
# check used to trap on it, because comisd sets ZF for an unordered compare
# just as it does for an equal one, so branching on "not equal" could not tell
# "equal" from "unordered" and took the trap.
print(1.0 / nan)

# --- modulo -------------------------------------------------------------
# Modulo is a - n*b for n = trunc(a/b), so the remainder takes the sign of
# the dividend. These are the cases where that agrees with CPython's %,
# which is why they can live in a file compared against CPython: a positive
# dividend over a positive divisor. The trunc-vs-floor disagreements
# (a negative dividend, or a negative divisor) are covered as adversarial
# native-tier cases in tools/run_tier_diff.py instead, since CPython's %
# floors and would make the expected output here wrong.
print(7.5 % 2.0)
# An exact division has a zero remainder. We normalize it to +0.0, matching
# CPython, which does not preserve the dividend's sign for a zero remainder:
# -4.0 % 2.0 prints 0.0 and not -0.0. Note this differs from C fmod, which
# returns -0.0 here; the interpreter and the JIT agree with each other and
# with CPython, which is what the expected output is recorded from.
print(3.0 % 2.0)
print(-4.0 % 2.0)
# A dividend smaller in magnitude than the divisor is its own remainder.
print(1.0 % 2.0)
# --- modulo against inf and nan ---------------------------------------
# inf is made by overflowing at run time; there is no inf literal.
inf = 1e308 * 1e308
nan = inf - inf
# n = trunc(1.0/inf) is exactly 0, so the answer is the dividend, 1.0. This
# is the case that has to skip the n*b multiply: IEEE says 0 * inf is NaN,
# but n*b is 0 for every b when n is 0, so multiplying here produced a NaN
# where C's fmod and CPython both give 1.0. The guard that skips the multiply
# used to land on the addsd that normalizes signed zero instead, one
# instruction too early, so the multiply still ran and this printed nan.
print(1.0 % inf)
# A zero dividend against inf is likewise its own remainder.
print(0.0 % inf)
# inf as the dividend is always NaN, because fmod(inf, b) is inf - inf.
print(inf % 1.0)
print(inf % inf)
# NaN propagates in either position, and must not be mistaken for a zero
# quotient by the "skip the multiply" guard.
print(1.0 % nan)
print(nan % 1.0)
