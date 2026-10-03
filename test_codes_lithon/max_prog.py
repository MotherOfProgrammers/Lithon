def find_max(a:int[8],b:int[8]) -> int[8]:
	if a > b:
		return a
	else:
		return b

a:int[8] = int(input("Enter value 1: "))
b:int[8] = int(input("Enter value 2: "))
print(find_max(a,b))
