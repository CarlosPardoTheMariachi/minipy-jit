# calling a function that is defined nowhere in the file
# expect-stderr: tests/undef_call.mp:4: error: call to undefined function 'nope'
# expect-exit: 1
print(nope(1))
