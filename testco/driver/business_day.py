#!/usr/bin/env python3
"""A day of trading at the test company: new orders, card charges, payment events.

    docker compose exec driver python /driver/business_day.py [--orders 20] [--seed 7]
    docker compose exec company python nightly_sync.py      # the company's own nightly run, if wanted

Each order: placed in the shop (pending), charged through payments, and the "charge succeeded"
event sent to the payments webhook, which marks it paid. Then the glue takes over: the n8n flow
(every 5 minutes), or the company's nightly script if it's run (in the company's container, so
its traffic is recorded as the company's). Every new order is
written to /data/live_orders.json, so check_glue.py can confirm each ends up completed with one
invoice. Not the company's code (so it's kept out of company/, which Optum analyses): a stand-in
for its customers and Stripe.
"""
import argparse
import json
import os
import random

import sys

import requests

sys.path.insert(0, "/company")
from common import HOOKS, PAYMENTS, PAYMENTS_KEY, SHOP, SHOP_AUTH

LIVE = "/data/live_orders.json"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--orders", type=int, default=20)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    customers = requests.get(SHOP + "/customers", params={"per_page": 100}, auth=SHOP_AUTH, timeout=60).json()
    products = requests.get(SHOP + "/products", params={"per_page": 20}, auth=SHOP_AUTH, timeout=60).json()
    live = json.load(open(LIVE)) if os.path.exists(LIVE) else []
    for _ in range(a.orders):
        c = rng.choice(customers)
        items = [{"product_id": p["id"], "quantity": rng.randint(1, 3)} for p in rng.sample(products, rng.randint(1, 3))]
        order = requests.post(SHOP + "/orders", auth=SHOP_AUTH, timeout=60, json={
            "customer_id": c["id"], "status": "pending", "currency": "GBP", "billing": c["billing"], "line_items": items}).json()
        cents = round(float(order["total"]) * 100)
        charge = requests.post(PAYMENTS + "/v1/charges", auth=(PAYMENTS_KEY, ""), timeout=60, data={
            "amount": cents, "currency": "gbp", "source": "tok_visa", "receipt_email": c["email"],
            "metadata[order_id]": order["id"]}).json()
        event = {"type": "charge.succeeded", "data": {"object": {"id": charge["id"], "amount": cents,
                                                                 "metadata": {"order_id": str(order["id"])}}}}
        hook = requests.post(HOOKS + "/stripe-events", json=event, timeout=60)
        live.append({"order": order["id"], "email": c["email"], "total_cents": cents, "charge": charge["id"],
                     "webhook_status": hook.status_code})
    json.dump(live, open(LIVE, "w"), indent=1)
    print(f"business day: {a.orders} orders placed, charged and paid ({len(live)} live orders in all)")


if __name__ == "__main__":
    main()
