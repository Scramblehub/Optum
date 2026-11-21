#!/usr/bin/env python3
"""Every live order (business_day.py) ends up completed, with exactly one invoice of its total.

    python3 testco/check_glue.py      # exit 1 on any order that isn't
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from load import Odoo, shop_api  # noqa: E402

live = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "live_orders.json")))
odoo = Odoo()
problems = []
for o in live:
    order = shop_api("GET", f"/orders/{o['order']}")
    invoices = odoo("account.move", "search_read", [["ref", "=", f"order {o['order']}"]], fields=["amount_total", "state"])
    if order["status"] != "completed":
        problems.append(f"order {o['order']}: {order['status']}, not completed")
    if len(invoices) != 1:
        problems.append(f"order {o['order']}: {len(invoices)} invoices")
    elif round(invoices[0]["amount_total"] * 100) != o["total_cents"] or invoices[0]["state"] != "posted":
        problems.append(f"order {o['order']}: invoice {invoices[0]}, order total {o['total_cents']}")
print(f"{len(live)} live orders: " + ("all completed, each with one posted invoice of its total" if not problems
                                      else f"{len(problems)} problems"))
for p in problems[:20]:
    print("  " + p)
sys.exit(1 if problems else 0)
