# shadowing the builtin print is a compile-time error
# expect-stderr: tests/shadow_print.mp:4: error: cannot shadow builtin 'print'
# expect-exit: 1
print = 5
