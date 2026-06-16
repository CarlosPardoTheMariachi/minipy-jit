# while loops
# expect: 55
# expect: 5050
def sumto(n):
    i = 1
    s = 0
    while i <= n:
        s = s + i
        i = i + 1
    return s
print(sumto(10))
print(sumto(100))
