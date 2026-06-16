# division by zero: prior stdout flushed, error to stderr, exit 1
# expect: 42
# expect-stderr: runtime error: division by zero
# expect-exit: 1
print(42)
print(1 // 0)
print(999)
