# forward references and mutual recursion must stay LEGAL: that is the whole
# reason sema collects every signature before checking any body
# expect: 11
# expect: 1
# expect: 0
def a(n):
    return b(n) + 1
def b(n):
    return n * 2
def is_even(n):
    if n == 0:
        return 1
    return is_odd(n - 1)
def is_odd(n):
    if n == 0:
        return 0
    return is_even(n - 1)
print(a(5))
print(is_even(10))
print(is_odd(10))
