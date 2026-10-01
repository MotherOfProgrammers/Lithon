n: int[64] = 1_000_000_000
i: int[64] = 0
total: int[64] = 0

while i < n:
    total += (i * 31) ^ (i >> 3)
    i += 1

print(total)
