// Glue: pull orders from a shop API and push them to a CRM. Same job as the Python
// http_glue sample, so the API nodes have the same ids. Needs network; not run in tests.
const axios = require("axios");

const SHOP = "https://shop.example.com/api";
const CRM_DEALS = "https://crm.example.com/api/deals";

async function fetchOrders(since) {
  const res = await fetch(`${SHOP}/orders?since=${since}`);
  return res.json();
}

async function pushDeal(order) {
  return axios.post(CRM_DEALS, { title: `Order ${order.id}`, value: order.total });
}

async function closeDeal(dealId) {
  return fetch(`https://crm.example.com/api/deals/${dealId}`, {
    method: "PATCH",
    body: JSON.stringify({ status: "won" }),
  });
}

async function sync(since) {
  for (const order of await fetchOrders(since)) {
    await pushDeal(order);
  }
}

module.exports = { sync, closeDeal };
