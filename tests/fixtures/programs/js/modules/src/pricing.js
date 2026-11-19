export const TAX = 0.2;

export function priceWithTax(amount) {
  return Math.round(amount * (1 + TAX) * 100) / 100;
}

export default function discount(amount, pct) {
  return amount * (1 - pct / 100);
}
