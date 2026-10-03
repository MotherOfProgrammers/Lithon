"""Plain-CPython equivalent of test_codes_lithon/test_1k_fsum.py.

A tiny (1,000-iteration) version of the fsum benchmark -- both engines
finish in well under a second, so this is for eyeballing the bench_vid_maker
UI/video output quickly, not for measuring real speedup.
"""
j = 0
for i in range(1000):
    j += i
print(j)
