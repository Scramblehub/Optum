#!/usr/bin/env python3
"""Load the generated data (generate.py) into the running tools, through the recording proxy.

    python3 testco/load.py [--data testco/data] [--tools accounting,payments]

Writes data/ids.json: each tool's generated record ids mapped to the ids the live tool gave them,
so the ground truth can be checked against what Optum finds. Optum never reads this file (or
ground_truth.json): it learns from the tools themselves.

Idempotent for accounting: run twice, it loads once (it checks for a marker partner first).
"""
import argparse
import base64
import json
import os
import time
import urllib.request

PROXY = "http://127.0.0.1"
ODOO, ODOO_DB = PROXY + ":8069/jsonrpc", "testco"
STRIPE = PROXY + ":12111"
SHOP = PROXY + ":8080/wp-json/wc/v3"
SHOP_AUTH = "Basic " + base64.b64encode(b"admin:testcoShopApiPassword00").decode()  # fixed test credential (shop/setup.sh)
MARKER = "testco-loaded"


def http_json(url, payload):
    req = urllib.request.Request(url, json.dumps(payload).encode(), {"Content-Type": "application/json"})
    out = json.loads(urllib.request.urlopen(req, timeout=300).read())
    if out.get("error"):
        raise RuntimeError(out["error"]["data"]["message"][:400])
    return out["result"]


class Odoo:
    def __init__(self):
        self.uid = self.call("common", "authenticate", ODOO_DB, "admin", "admin", {})

    def call(self, service, method, *args):
        return http_json(ODOO, {"jsonrpc": "2.0", "method": "call",
                                "params": {"service": service, "method": method, "args": list(args)}})

    def __call__(self, model, method, *args, **kw):
        return self.call("object", "execute_kw", ODOO_DB, self.uid, "admin", model, method, list(args), kw)


def setup_accounting(odoo):
    """A UK company in GBP, with VAT at 20% included in prices (as shop prices are)."""
    gb = odoo("res.country", "search", [["code", "=", "GB"]])[0]
    gbp = odoo("res.currency", "search", [["name", "=", "GBP"]], context={"active_test": False})[0]
    odoo("res.currency", "write", [gbp], {"active": True})
    company = odoo("res.company", "read", [1], fields=["currency_id"])[0]
    if company["currency_id"][0] != gbp:
        odoo("res.company", "write", [1], {"name": "Testco Retail Ltd", "country_id": gb, "currency_id": gbp})
    taxes = odoo("account.tax", "search", [["name", "=", "VAT 20% (incl.)"], ["type_tax_use", "=", "sale"]])
    if taxes:
        return taxes[0]
    return odoo("account.tax", "create", {"name": "VAT 20% (incl.)", "amount": 20.0, "amount_type": "percent",
                                         "type_tax_use": "sale", "price_include_override": "tax_included"})


def load_accounting(odoo, data, ids):
    if odoo("res.partner", "search_count", [["ref", "=", MARKER]]):
        print("accounting: already loaded")
        return
    vat = setup_accounting(odoo)
    recs = data["records"]
    people = [r for r in recs if r.get("type") is None]
    invoices = [r for r in recs if r.get("type") == "invoice"]
    t = time.time()
    partner_ids = []
    for i in range(0, len(people), 200):
        batch = people[i:i + 200]
        partner_ids += odoo("res.partner", "create", [{"name": p["name"], "email": p["email"], "street": p["street"],
                                                       "city": p["city"], "zip": p["zip"]} for p in batch])
    ids["accounting"] = {str(p["id"]): pid for p, pid in zip(people, partner_ids)}
    by_email = {p["email"]: pid for p, pid in zip(people, partner_ids)}
    move_ids = []
    for i in range(0, len(invoices), 100):
        batch = invoices[i:i + 100]
        moves = odoo("account.move", "create", [{
            "move_type": "out_invoice", "partner_id": by_email[inv["partner_email"]], "ref": f"order {inv['id']}",
            "invoice_line_ids": [[0, 0, {"name": ln["name"], "quantity": ln["quantity"],
                                         "price_unit": ln["price_unit_cents"] / 100.0, "tax_ids": [[6, 0, [vat]]]}]
                                 for ln in inv["lines"]]} for inv in batch])
        odoo("account.move", "action_post", moves)  # posted: a tax document, no longer editable
        move_ids += moves
    for inv, mid in zip(invoices, move_ids):
        ids["accounting"][str(inv["id"])] = mid
    odoo("res.partner", "create", {"name": "Testco loader marker", "ref": MARKER})
    totals = odoo("account.move", "read", move_ids[:3], fields=["amount_total", "state", "name"])
    print(f"accounting: {len(partner_ids)} partners, {len(move_ids)} posted invoices in {time.time() - t:.0f} s "
          f"(first: {[(m['name'], m['amount_total'], m['state']) for m in totals]})")


def shop_api(method, path, payload=None):
    req = urllib.request.Request(SHOP + path, None if payload is None else json.dumps(payload).encode(), method=method,
                                 headers={"Authorization": SHOP_AUTH, "Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=300).read())


def load_shop(data, ids):
    """Products, the shop's customers and its 2,000 orders, through WooCommerce's REST API."""
    if shop_api("GET", "/products?sku=SKU-TEE-01"):  # our products are the marker (admin is a "customer" too)
        print("shop: already loaded")
        return
    t = time.time()
    recs = data["records"]
    customers = [r for r in recs if r.get("type") is None]
    orders = [r for r in recs if r.get("type") == "order"]
    products = {}
    for o in orders:
        for li in o["line_items"]:
            products.setdefault(li["sku"], li)
    made = shop_api("POST", "/products/batch", {"create": [
        {"name": p["name"], "sku": sku, "type": "simple", "regular_price": f"{p['unit_cents'] / 100:.2f}"}
        for sku, p in sorted(products.items())]})["create"]
    product_id = {p["sku"]: p["id"] for p in made}
    live = {}
    for i in range(0, len(customers), 100):
        batch = customers[i:i + 100]
        out = shop_api("POST", "/customers/batch", {"create": [
            {"email": c["email"], "first_name": c["first_name"], "last_name": c["last_name"],
             "billing": {**c["billing"], "first_name": c["first_name"], "last_name": c["last_name"],
                         "email": c["email"], "country": "GB"}} for c in batch]})["create"]
        for c, o in zip(batch, out):
            if "error" in o:
                raise RuntimeError(f"customer {c['email']}: {o['error']}")
            live[c["id"]] = o["id"]
    ids["shop"] = {str(k): v for k, v in live.items()}
    by_id = {c["id"]: c for c in customers}
    for i in range(0, len(orders), 100):
        batch = orders[i:i + 100]
        out = shop_api("POST", "/orders/batch", {"create": [{
            "customer_id": live[o["customer_id"]], "status": "completed", "set_paid": True, "currency": "GBP",
            "billing": {**by_id[o["customer_id"]]["billing"], "email": by_id[o["customer_id"]]["email"],
                        "first_name": by_id[o["customer_id"]]["first_name"],
                        "last_name": by_id[o["customer_id"]]["last_name"], "country": "GB"},
            "line_items": [{"product_id": product_id[li["sku"]], "quantity": li["quantity"]} for li in o["line_items"]]}
            for o in batch]})["create"]
        for o, made_order in zip(batch, out):
            if "error" in made_order:
                raise RuntimeError(f"order {o['id']}: {made_order['error']}")
            ids["shop"][str(o["id"])] = made_order["id"]
    print(f"shop: {len(made)} products, {len(live)} customers, {len(orders)} orders in {time.time() - t:.0f} s")


def load_payments(data, ids):
    """stripe-mock keeps nothing, so 'loading' is the traffic itself: the customers and charges
    the shop's checkout would create, recorded by the proxy."""
    auth = {"Authorization": "Basic " + base64.b64encode(b"sk_test_123:").decode(),
            "Content-Type": "application/x-www-form-urlencoded"}
    import urllib.parse
    n = 0
    for r in data["records"][:300]:  # a sample of checkouts is enough traffic to learn from
        if r.get("type") == "charge":
            body = urllib.parse.urlencode({"amount": r["amount"], "currency": r["currency"], "source": "tok_visa",
                                           "receipt_email": r["receipt_email"]}).encode()
            path = "/v1/charges"
        else:
            body = urllib.parse.urlencode({"email": r["email"], "name": r["name"]}).encode()
            path = "/v1/customers"
        out = json.loads(urllib.request.urlopen(urllib.request.Request(STRIPE + path, body, auth), timeout=30).read())
        ids.setdefault("payments", {})[str(r["id"])] = out["id"]
        n += 1
    print(f"payments: {n} checkout calls recorded (stripe-mock answers in Stripe's shape and keeps nothing)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--data", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "data"))
    ap.add_argument("--tools", default="shop,accounting,payments")
    a = ap.parse_args()
    ids_path = os.path.join(a.data, "ids.json")
    ids = json.load(open(ids_path)) if os.path.exists(ids_path) else {}
    for tool in a.tools.split(","):
        data = json.load(open(os.path.join(a.data, f"{tool}.json")))
        if tool == "shop":
            load_shop(data, ids)
        elif tool == "accounting":
            load_accounting(Odoo(), data, ids)
        elif tool == "payments":
            load_payments(data, ids)
        else:
            print(f"{tool}: no loader yet")
    json.dump(ids, open(ids_path, "w"), indent=1)


if __name__ == "__main__":
    main()
