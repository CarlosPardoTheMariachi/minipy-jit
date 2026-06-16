# if / elif / else
# expect: -1
# expect: 0
# expect: 1
def classify(n):
    if n < 0:
        return -1
    elif n == 0:
        return 0
    else:
        return 1
print(classify(-5))
print(classify(0))
print(classify(9))
