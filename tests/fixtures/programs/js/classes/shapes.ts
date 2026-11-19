// Classes: constructors, super, overriding, this-calls, typed locals and parameters,
// parameter properties, a typed class field.
export class Shape {
  constructor(public name: string) {}

  area(): number {
    return 0;
  }

  describe(): string {
    return `${this.name}: ${this.area().toFixed(1)}`;
  }
}

export class Square extends Shape {
  constructor(private side: number) {
    super("square");
  }

  area(): number {
    return this.side * this.side;
  }
}

export class Circle extends Shape {
  constructor(private r: number) {
    super("circle");
  }

  area(): number {
    return 3.14159 * this.r * this.r;
  }
}

class Canvas {
  private main = new Square(1);

  constructor(private readonly backup: Circle) {}

  mainArea(): number {
    return this.main.area();
  }

  backupArea(): number {
    return this.backup.area();
  }
}

function describeOne(shape: Shape): string {
  return shape.describe();
}

export function makeAll(): Shape[] {
  return [new Square(2), new Circle(1)];
}

for (const s of makeAll()) {
  console.log(describeOne(s));
}
console.log(new Canvas(new Circle(2)).mainArea());
