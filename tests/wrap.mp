# two's-complement wraparound & INT64_MIN edge cases
# expect: -9223372036854775808
# expect: -9223372036854775808
# expect: -9223372036854775808
# expect: 0
print(9223372036854775807 + 1)
print(-9223372036854775808)
m = -9223372036854775808
print(m // -1)
print(m % -1)
