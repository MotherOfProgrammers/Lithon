n:int[64] = 1000
i:int[64] = 0
j:int[64] = 0
k:int[64] = 0
e:float[64] = 0.0
le:float[64] = 0.0

for i in range(n):
    k = i + 1
    e = 1.0
    for j in range(k):
        e = e * (1.0 + 1.0 / k)
    if e > le:
        le = e

print(le)
