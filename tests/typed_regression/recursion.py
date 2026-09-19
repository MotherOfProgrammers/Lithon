def fib(n: int[64]) -> int[64]:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

x: int[64] = fib(10)
print(x)
