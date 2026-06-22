# no dynamic rebinding here, so a second def of the same name is an error
# rather than "the last one wins"
# expect-stderr: tests/redef_func.mp:7: error: redefinition of function 'f'
# expect-exit: 1
def f(a):
    return a
def f(b):
    return b + 1
print(f(1))
