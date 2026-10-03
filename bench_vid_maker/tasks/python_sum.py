"""Plain-CPython equivalent of test_codes_lithon/test_1b_fsum.py.

Same program (accumulate i over range(1_000_000_000) and print the sum);
the only difference is the Lithon-specific `int[64]` width annotations,
which CPython's int already handles natively and has no syntax for.
"""
j = 0
for i in range(1_000_000_000):
    j += i
print(j)
