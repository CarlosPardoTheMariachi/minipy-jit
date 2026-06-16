# and/or short-circuit + deciding-operand value, with print side effects
# expect: 1
# expect: 1
# expect: 0
# expect: 5
# expect: 5
# expect: 0
# expect: 0
# expect: 3
# expect: 7
# expect: 7
# expect: 1
# expect: 0
def side(x):
    print(x)
    return x
print(side(1) or side(2))
print(side(0) or side(5))
print(side(0) and side(9))
print(side(3) and side(7))
print(not 0)
print(not 5)
