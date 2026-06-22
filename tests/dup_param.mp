# two params with the same name would want the same frame slot
# expect-stderr: tests/dup_param.mp:4: error: duplicate parameter 'a' in 'f'
# expect-exit: 1
def f(a, a):
    return a
print(f(1, 2))
