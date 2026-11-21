// The payments webhook: when a card charge succeeds, the shop order becomes "processing" (paid).
// Stripe calls it (here: the business driver, standing in for Stripe's webhooks).
const http = require('http')

const SHOP = process.env.SHOP_URL || 'http://shop.testco.internal:8080/wp-json/wc/v3'
const SHOP_AUTH = 'Basic ' + Buffer.from('admin:testcoShopApiPassword00').toString('base64')

async function markPaid (orderId, chargeId) {
  const res = await fetch(`${SHOP}/orders/${orderId}`, {
    method: 'PUT',
    headers: { Authorization: SHOP_AUTH, 'Content-Type': 'application/json' },
    body: JSON.stringify({ status: 'processing', set_paid: true, transaction_id: chargeId })
  })
  return res.status
}

function handleEvent (event) {
  if (event.type !== 'charge.succeeded') return Promise.resolve(204)
  const charge = event.data.object
  return markPaid(charge.metadata.order_id, charge.id)
}

http.createServer((req, res) => {
  if (req.method !== 'POST' || req.url !== '/stripe-events') {
    res.writeHead(404).end()
    return
  }
  let body = ''
  req.on('data', chunk => { body += chunk })
  req.on('end', async () => {
    try {
      const status = await handleEvent(JSON.parse(body))
      res.writeHead(status < 300 ? 200 : 502).end()
    } catch (e) {
      res.writeHead(400).end(String(e))
    }
  })
}).listen(3000, () => console.log('payments webhook on :3000'))
