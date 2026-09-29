# Self tail calls (turned into loops by convert_self_tail_calls).
# Depths stay under CPython's default recursion limit (1000) so the
# expected output can be recorded from real CPython.

def acc(n, s):
    if n < 1:
        return s
    return acc(n - 1, s + n)

# Arguments cross over each iteration: the new `a` is computed from `b`
# and the new `b` is the old `a`. Wrong store ordering breaks this.
def g(a, b):
    if a > 100:
        return b
    return g(b + 1, a)

# Side effect before the tail call.
def count(n):
    if n < 1:
        return 0
    print(n)
    return count(n - 1)

# Several tail calls, different arguments per path.
def steps(n, k):
    if n == 1:
        return k
    if n > 100:
        return steps(n - 100, k + 1)
    return steps(n - 1, k + 2)

# Tail call after a loop inside the function body.
def go(n, t):
    if n < 1:
        return t
    x = 0
    for i in range(3):
        x = x + i
    return go(n - 1, t + x)

# NOT a tail call (the addition happens after the call): must stay recursive.
def s(n):
    if n < 1:
        return 0
    return n + s(n - 1)

print(acc(500, 0))
print(g(1, 2))
print(g(50, 60))
print(count(5))
print(steps(1000, 0))
print(go(300, 0))
print(s(300))
