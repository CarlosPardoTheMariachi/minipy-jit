# reading a name that is never a param and never assigned anywhere in the
# function is a compile-time error (an assigned-but-not-yet-reached name is
# fine — see zeroinit.mp — but this one has no slot to read at all)
# expect-stderr: tests/undef_name.mp:7: error: name 'zzz' is not defined in 'f'
# expect-exit: 1
def f():
    return zzz
print(f())
