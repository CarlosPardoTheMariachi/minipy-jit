# integer literals, arithmetic, precedence, parens
# expect: 14
# expect: 20
# expect: 13
# expect: 6
# expect: 6
print(2 + 3 * 4)
print((2 + 3) * 4)
print(3 * 5 - 2)
print(-2 * -3)
print(2 * 10 % 7)
