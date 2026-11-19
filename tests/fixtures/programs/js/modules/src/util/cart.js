// CommonJS class export: `const Cart = require(...)` then `new Cart()`.
class Cart {
  add(item) {
    return [item];
  }
}

module.exports = Cart;
