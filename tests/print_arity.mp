# print is a builtin, so its arity check is hand-written rather than coming
# from the signature table
# expect-stderr: tests/print_arity.mp:5: error: print() takes exactly one argument
# expect-exit: 1
print(1, 2)
