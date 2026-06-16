def fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)

def main():
    i = 0
    while i < 30:
        print(fib(i))
        i = i + 1

main()
