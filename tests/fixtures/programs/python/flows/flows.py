"""Calls found by following values: parameters, return values, callbacks, class creation.

Every call here runs (python3 flows.py), so the runtime tracer checks the graph too.
"""
import re


def shout(text):
    return text.upper()


def whisper(text):
    return text.lower()


class Registry:
    """A class body calls like a function: these calls belong to the class."""

    PATTERN = re.compile(r"\w+")
    NAMES = sorted(["b", "a"], key=shout)

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        cls.registered = True


class Field:
    def __set_name__(self, owner, name):
        self.name = name

    def __get__(self, obj, objtype=None):
        return self.name


class Bag(Registry):
    """Created through Registry.__init_subclass__ and Field.__set_name__."""

    label = Field()

    def __init__(self, items):
        self.items = items

    def __len__(self):
        return len(self.items)

    def __iter__(self):
        return iter(self.items)


class Meta(type):
    def __call__(cls, *args):
        return super().__call__(*args)


class Counted(metaclass=Meta):
    def __init__(self, n):
        self.n = n


class Plain(Bag):
    """Inherits its constructor: Plain() runs Bag.__init__."""


def apply(fn, text):
    """Calls whatever it's given: shout and whisper, from main."""
    return fn(text)


def relay(fn, text):
    """Passes its parameter on: apply's fn gets what relay's callers pass."""
    return apply(fn, text)


def size(bag):
    """bag is a Bag at every call site: len(bag) runs Bag.__len__."""
    return len(bag) + len(list(bag))


def render(text, style=shout):
    """Calls its parameter's default unless the caller passes another."""
    return style(text)


def make_formatter(prefix):
    def fmt(text):
        return prefix + text

    return fmt


def make_bag():
    return Bag(["x"])


def tagged(tag):
    """A decorator factory: @tagged("t") calls tagged, then the function it returns."""

    def wrap(fn):
        fn.tag = tag
        return fn

    return wrap


@tagged("t")
def task():
    return "done"


class Greeter:
    """A mixin: greet() calls name(), which only its subclasses have."""

    def greet(self):
        return "hi " + self.name()


class Person(Greeter):
    def name(self):
        return "ann"


def main():
    print(apply(shout, "a"), relay(whisper, "B"))
    print(size(Bag([1, 2])), render("c"))
    print(make_formatter("> ")("d"))
    print(make_bag().__len__(), task())
    print(list(map(whisper, ["E"])), Person().greet(), Counted(3).n, len(Plain([1])))


if __name__ == "__main__":
    main()
