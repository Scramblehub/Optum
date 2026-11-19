function cleanItems (rows, unit) {
  const result = []
  for (const row of rows) {
    if (!row || row.quantity <= 0) continue
    const cost = Math.round(row.price * 100) / 100
    result.push({ sku: String(row.sku).trim(), quantity: row.quantity, cost, unit })
  }
  result.sort((x, y) => x.sku.localeCompare(y.sku))
  return result
}

module.exports = { cleanItems }
