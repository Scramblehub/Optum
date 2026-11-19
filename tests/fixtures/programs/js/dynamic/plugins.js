// Hard cases for static analysis: a dispatch object, computed members, callbacks, events.
const handlers = {
  refund: (amount) => -amount,
  charge: doCharge,
};

function doCharge(amount) {
  return amount;
}

function apply(op, amount) {
  return handlers[op](amount);
}

class Ops {
  double(x) {
    return x * 2;
  }

  run(name, x) {
    return this[name](x);
  }
}

function each(fn, xs) {
  return xs.map((x) => fn(x));
}

const { EventEmitter } = require("node:events");
const bus = new EventEmitter();
bus.on("order", (o) => apply("charge", o.total));

function main() {
  console.log(apply("charge", 5), new Ops().run("double", 4), each(doCharge, [1, 2]));
  bus.emit("order", { total: 3 });
}

main();
