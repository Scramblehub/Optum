// Family D (JS): normalizeItems is copied into checkout.js as cleanItems, renamed.
function normalizeItems (items, currency) {
  const out = []
  for (const item of items) {
    if (!item || item.quantity <= 0) continue
    const price = Math.round(item.price * 100) / 100
    out.push({ sku: String(item.sku).trim(), quantity: item.quantity, price, currency })
  }
  out.sort((a, b) => a.sku.localeCompare(b.sku))
  return out
}

module.exports = { normalizeItems }
