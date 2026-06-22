# arity is checked against the signature collected in sema's first pass
# expect-stderr: tests/bad_arity.mp:6: error: 'add' expects 2 argument(s), got 1
# expect-exit: 1
def add(a, b):
    return a + b
print(add(1))
