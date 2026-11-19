"""Glue: pull orders from a shop API and push them to a CRM. Needs network; not run in tests."""
import requests

SHOP = "https://shop.example.com/api"
CRM_DEALS = "https://crm.example.com/api/deals"


def fetch_orders(since):
    resp = requests.get(f"{SHOP}/orders", params={"since": since}, timeout=10)
    return resp.json()


def push_deal(order):
    body = {"title": f"Order {order['id']}", "value": order["total"]}
    return requests.post(CRM_DEALS, json=body, timeout=10)


def close_deal(deal_id):
    url = f"https://crm.example.com/api/deals/{deal_id}"
    return requests.patch(url, json={"status": "won"}, timeout=10)


def sync(since):
    for order in fetch_orders(since):
        push_deal(order)
