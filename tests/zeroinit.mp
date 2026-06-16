# locals read as 0 before first assignment — the JIT must zero its
# frame slots; a prior call leaves garbage on the stack exactly where they live.
# expect: 0
# expect: 0
# expect: 6
def dirty(a, b, c):
    t = a + b + c
    return t
def f():
    if 0:
        x = 5
    return x
def g():
    s = 0
    if 0:
        y = 9
    while s == 0 and y == 0:
        s = 6
    return s + y
dirty(111, 222, 333)
print(f())
dirty(444, 555, 666)
print(f())
print(g())
