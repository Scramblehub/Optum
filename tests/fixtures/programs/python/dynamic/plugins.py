"""Hard cases for static analysis: decorators, dispatch tables, getattr, callbacks."""

HANDLERS = {}


def handler(name):
    def register(fn):
        HANDLERS[name] = fn
        return fn

    return register


@handler("refund")
def do_refund(amount):
    return -amount


@handler("charge")
def do_charge(amount):
    return amount


def apply(op, amount):
    return HANDLERS[op](amount)


class Ops:
    def double(self, x):
        return x * 2

    def run(self, name, x):
        return getattr(self, name)(x)


def each(fn, xs):
    return [fn(x) for x in xs]


def main():
    print(apply("charge", 5), apply("refund", 5))
    print(Ops().run("double", 4))
    print(each(do_charge, [1, 2]))


if __name__ == "__main__":
    main()
