total: int[64] = 0
i: int[64] = 0
j: int[64] = 0
for i in range(5):
    for j in range(5):
        total = total + i * j
print(total)
