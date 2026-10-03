# A conditional expression in a *typed* program. The frontend lowers it to a
# temporary that both arms store and the join loads, and that temporary carries
# no annotation because there is no source variable to annotate. V1_SPEC 0.6.1
# requires every *source* binding to be annotated, so the checker has to infer
# the merge result from the arms instead of demanding a declaration it cannot
# have. The float line below is the part that matters most: infer the temp as
# an int and it prints nonsense, and the merge arms disagreeing on type is
# still rejected (pinned in src/typecheck/typecheck_test.cpp).
x: int[64] = 5
y: int[64] = 2

print(x if x > y else y)
print(1 if x < y else 2 if x == 5 else 3)

f: float[64] = 1.5
g: float[64] = 0.5
print(f if f > g else g)

# Nested in a merge, and the merge feeding an assignment rather than a call.
z: int[64] = (x if x > y else y) - (1 if y < x else 0)
print(z)

# Merges inside a loop: the temporary is live across the back edge.
total: int[64] = 0
i: int[64] = 0
for i in range(5):
    total = total + (1 if x > y else -1)
print(total)
