"""Plain functions: direct calls, a call chain, recursion, a builtin call (no edge), dead code."""


def parse(text):
    return [int(x) for x in text.split(",")]


def total(values):
    return sum(values)


def factorial(n):
    return 1 if n <= 1 else n * factorial(n - 1)


def report(text):
    values = parse(text)
    return f"total={total(values)} factorial_of_max={factorial(max(values))}"


def unused():
    return report("1,2")


if __name__ == "__main__":
    print(report("1,2,3"))
