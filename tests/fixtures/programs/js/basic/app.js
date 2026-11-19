// Plain functions: declarations, arrow consts, a call chain, recursion, a nested helper,
// builtins (no edge), dead code.
function parse(text) {
  return text.split(",").map((x) => Number(x));
}

const total = (values) => values.reduce((a, b) => a + b, 0);

function factorial(n) {
  return n <= 1 ? 1 : n * factorial(n - 1);
}

function report(text) {
  const values = parse(text);
  const fmt = (v) => `total=${v}`;
  return `${fmt(total(values))} factorial_of_max=${factorial(Math.max(...values))}`;
}

function unused() {
  return report("1,2");
}

console.log(report("1,2,3"));
