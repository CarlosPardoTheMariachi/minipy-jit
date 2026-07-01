# every parameter position, and left-to-right argument evaluation.
# a frame-slot off-by-one shows up here as an argument arriving in the wrong
# position, which nothing else in the suite would notice.
# expect: 10
# expect: 11
# expect: 12
# expect: 13
# expect: 14
# expect: 15
# expect: 16
# expect: 17
# expect: 1
# expect: 2
# expect: 3
# expect: 123
def p0(a, b, c, d, e, f, g, h):
    return a
def p1(a, b, c, d, e, f, g, h):
    return b
def p2(a, b, c, d, e, f, g, h):
    return c
def p3(a, b, c, d, e, f, g, h):
    return d
def p4(a, b, c, d, e, f, g, h):
    return e
def p5(a, b, c, d, e, f, g, h):
    return f
def p6(a, b, c, d, e, f, g, h):
    return g
def p7(a, b, c, d, e, f, g, h):
    return h

def side(x):
    print(x)
    return x

def three(a, b, c):
    return a * 100 + b * 10 + c

print(p0(10, 11, 12, 13, 14, 15, 16, 17))
print(p1(10, 11, 12, 13, 14, 15, 16, 17))
print(p2(10, 11, 12, 13, 14, 15, 16, 17))
print(p3(10, 11, 12, 13, 14, 15, 16, 17))
print(p4(10, 11, 12, 13, 14, 15, 16, 17))
print(p5(10, 11, 12, 13, 14, 15, 16, 17))
print(p6(10, 11, 12, 13, 14, 15, 16, 17))
print(p7(10, 11, 12, 13, 14, 15, 16, 17))
print(three(side(1), side(2), side(3)))
