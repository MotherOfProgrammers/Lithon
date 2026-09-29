# JIT-only: 10,000,000-deep tail recursion. The interpreter recurses on the
# C++ stack and CPython has a 1000-frame limit, so neither can run this;
# the expected value is closed-form: n*(n+1)/2 = 50000005000000.
# Before TCO the native tier crashed with a stack overflow here.
def acc(n, s):
    if n < 1:
        return s
    return acc(n - 1, s + n)
print(acc(10000000, 0))
