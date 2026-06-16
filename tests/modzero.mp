# modulo by zero uses the same abort
# expect-stderr: runtime error: division by zero
# expect-exit: 1
print(5 % 0)
