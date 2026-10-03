# Bitwise and bit-shift. Integer-only: there is no float bit pattern in
# Lithon, so `& | ^ << >>` on a float is a type error rather than a
# reinterpretation of the double's bytes.
#
# Every expected value is CPython's, and tests/programs/expected/bitwise.out
# is copied from CPython itself, not from Lithon -- the point of this file is
# that the two agree.
#
# Two deliberate divergences from CPython, NOT covered here because they
# cannot agree:
#
#   * Bounded word. CPython ints are unbounded, so 5 << 63 is a 66-bit
#     number. Lithon is a 64-bit machine, so the same expression wraps to
#     -2**63. Same rule, different width. tests/programs/bitwise_wrap.py
#     pins that boundary instead.
#   * Shift count. CPython allows any count; Lithon traps outside 0..63
#     because x86 masks the count to 6 bits and would otherwise silently
#     shift by a different amount. See the shift count tests.

# --- and / or / xor -------------------------------------------------------
a: int[64] = 5
b: int[64] = 3
print(a & b)
print(a | b)
print(a ^ b)

# Commuted, to pin that these are genuinely commutative rather than
# accidentally right in one order.
print(b & a)
print(b | a)
print(b ^ a)

# Identity and annihilator properties.
zero: int[64] = 0
all_ones: int[64] = -1
print(a & all_ones)
print(a & zero)
print(a | zero)
print(a | all_ones)
print(a ^ a)
print(a ^ all_ones)

# Overlapping bits: the case where and and or are exact complements.
x: int[64] = 0xF0F0
y: int[64] = 0xFF00
print(x & y)
print(x | y)
print(x ^ y)
print((x | y) & all_ones)

# --- negative operands: two's complement, unbounded-looking bits ----------
n: int[64] = -8
print(n & 255)
print(n | 0)
print(n ^ -1)
print(n & n)
print(-1 & n)
print(n | n)

# --- shifts, literal count ------------------------------------------------
# The immediate encodings: shl/sar reg, imm8 (and the D1 short form at 1).
print(5 << 0)
print(5 << 1)
print(5 << 7)
print(5 << 31)
print(1 << 31)
print(-8 >> 1)
print(-8 >> 3)
print(255 >> 4)
print(1024 >> 5)

# Width - 1, the largest legal count. Right shifts agree with CPython at 63
# because both produce a value that still fits: -1 >> 63 is -1, since every
# bit set survives an arithmetic shift. The matching LEFT shift at 63 cannot
# agree (2**63 does not fit in int64, so Lithon wraps to -2**63); that case
# lives in bitwise_wrap.py.
print(-1 >> 63)
print(1 >> 63)
print(-8 >> 63)
print(255 >> 63)

# One below the boundary, where the left shift still fits and so still agrees.
# 2 << 61 is 2**62, which is representable; 5 << 61 would not be, because
# 5 << 61 == 2**61 + 2**63 already overflows the signed range.
print(1 << 62)
print(-1 << 62)
print(2 << 61)

# --- shifts, dynamic count ------------------------------------------------
# The CL encodings (shl/sar reg, cl) plus the runtime count check.
k: int[64] = 3
v: int[64] = 5
print(v << k)
print(v >> k)
print(-8 >> k)
print(255 >> k)

k2: int[64] = 1
print(1 << k2)
k3: int[64] = 62
print(1 << k3)
k4: int[64] = 0
print(1 << k4)

# --- shifts by a variable derived from arithmetic -------------------------
# The count is not a constant the optimizer can see, so this must go through
# the CL path even though it evaluates to 4.
base: int[64] = 3
derived: int[64] = base + 1
print(9 << derived)
print(144 >> derived)

# --- composition ----------------------------------------------------------
# Shifting a mask is how the unsigned-safe mod strength reduction works, so
# this is the shape that peephole rules will want to recognise.
m: int[64] = 255
print(m & (m >> 4))
print((m ^ 0xFF) & 0xFF)
print(((17 << 3) | 1) & 255)
print(((17 << 3) ^ 1) & 255)
print(((17 << 3) & 255) ^ 1)

# --- loop, to exercise the encoders under the unroller and in a real body --
acc: int[64] = 0
i: int[64] = 0
while i < 8:
    acc = acc + (1 << i)
    i = i + 1
print(acc)
print(acc & 255)
print((acc >> 8) & 15)
