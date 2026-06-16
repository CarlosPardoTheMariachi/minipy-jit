# nested calls, mutual recursion, deep expression nesting
# expect: 1
# expect: 0
# expect: 1
# expect: 0
# expect: 20
def is_even(n):
    if n == 0:
        return 1
    return is_odd(n - 1)
def is_odd(n):
    if n == 0:
        return 0
    return is_even(n - 1)
print(is_even(10))
print(is_odd(10))
print(is_even(7) or 1)
print(is_odd(8))
print(((1 + 2) * 3 + 1) + (2 * (3 + 1) + 2))
