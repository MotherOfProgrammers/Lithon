# Where Lithon's bitwise semantics deliberately DIFFER from CPython, pinned
# separately from tests/programs/bitwise.py so that file can stay a
# pure agreement test.
#
# 1. The machine word is 64 bits wide and shifts WRAP rather than growing.
#    CPython ints are unbounded, so `5 << 63` is the 66-bit number
#    46116860184273879040. Lithon computes it in 64 bits and keeps the low
#    64, which is -2**63. Nothing is trapped and nothing is auto-widened --
#    there is no wider type to widen to.
#
# 2. A shift count outside 0..63 is a compile-time RCR error when the count is
#    a literal, and a runtime error when it is only known at run time, because
#    x86 masks the count to its low 6 bits. A count of 64 would otherwise
#    execute as 0 and quietly return the unshifted value, and -1 would
#    execute as 63. Lithon refuses rather than returning the wrong answer.
#
#    This means `1 << 64` is a program that cannot run, where CPython prints a
#    perfectly good 65-bit number. That is the cost of a bounded word, and it
#    is a refusal, not a wrong answer.

# 5 << 63 == 5 * 2**63 == 2**63 + 2**65; mod 2**64 only 2**63 survives.
print(5 << 63)
# 5 << 62 == 2**62 + 2**64; the 2**64 term is gone, leaving 2**62. This one
# agrees with CPython by luck, and is here to show the wrap is a truncation
# and not a saturation.
print(5 << 62)
# 1 << 63 is exactly the sign bit: representable, and negative.
print(1 << 63)
# 3 << 63 keeps only bit 63.
print(3 << 63)
# 255 >> 63 and -1 >> 63: every bit set survives an arithmetic shift, so both
# agree with CPython even at the boundary.
print(255 >> 63)
print(-1 >> 63)
# Same wrap, but through the dynamic CL path rather than the immediate one, so
# the boundary is pinned for both encodings.
k: int[64] = 63
print(5 << k)
print(1 << k)
print(255 >> k)
# And one below the boundary, where nothing is lost at all.
k2: int[64] = 62
print(5 << k2)
