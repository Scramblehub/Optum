import discount, { priceWithTax as withTax } from "./pricing.js";
import * as pricing from "./pricing";
const { format } = require("./util");
const Cart = require("./util/cart.js");

export function placeOrder(items) {
  let subtotal = items.reduce((sum, i) => sum + i.price, 0);
  subtotal = discount(subtotal, 10);
  const check = pricing.priceWithTax(subtotal);
  new Cart().add(items[0]);
  require("./util/audit")("order placed");
  return format(withTax(subtotal) === check ? check : 0);
}
