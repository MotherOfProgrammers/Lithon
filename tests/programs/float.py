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
