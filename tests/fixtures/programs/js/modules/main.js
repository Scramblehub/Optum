// Modules: default, named, aliased and namespace imports; CommonJS require with destructuring;
// re-exports (named and *); a directory index; a third-party package; a Node builtin (no node).
// Mixes ES modules and CommonJS on purpose, so it isn't run.
import { placeOrder, priceWithTax } from "./src/index.js";
import lodash from "lodash";
import path from "node:path";

function run() {
  return placeOrder([{ price: 10 }]);
}

console.log(run(), priceWithTax(5), lodash.identity(path.sep));
