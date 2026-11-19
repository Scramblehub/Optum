"""Classes: constructors, super(), overriding, self-calls, typed locals and attributes."""


class Shape:
    def __init__(self, name):
        self.name = name

    def area(self):
        return 0.0

    def describe(self):
        return f"{self.name}: {self.area():.1f}"


class Square(Shape):
    def __init__(self, side):
        super().__init__("square")
        self.side = side

    def area(self):
        return self.side * self.side


class Circle(Shape):
    def __init__(self, r):
        super().__init__("circle")
        self.r = r

    def area(self):
        return 3.14159 * self.r * self.r


class Canvas:
    def __init__(self):
        self.main = Square(1)

    def main_area(self):
        return self.main.area()


def make_all():
    return [Square(2), Circle(1)]


def describe_square():
    sq = Square(3)
    return sq.describe()


if __name__ == "__main__":
    for shape in make_all():
        print(shape.describe())
    print(describe_square())
    print(Canvas().main_area())
