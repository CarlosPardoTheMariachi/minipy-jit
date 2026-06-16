# calls, recursion, multiple args
# expect: 120
# expect: 3628800
# expect: 6
def fact(n):
    if n <= 1:
        return 1
    return n * fact(n - 1)
def add3(a, b, c):
    return a + b + c
print(fact(5))
print(fact(10))
print(add3(1, 2, 3))
