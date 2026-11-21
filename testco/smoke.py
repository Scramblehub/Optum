#!/usr/bin/env python3
"""Smoke test for the test company (docs/plans/phase3.md): each service answers the way the glue
will use it. Run after `docker compose up -d --wait`; exits 1 on the first service that doesn't.

    python3 testco/smoke.py
"""
import base64
import json
import sys
import urllib.parse
import urllib.request

PAYMENTS = "http://127.0.0.1:12111"
ACCOUNTING = "http://127.0.0.1:8069"
AUTOMATION = "http://127.0.0.1:5678"
SHOP = "http://127.0.0.1:8080/wp-json/wc/v3"
SHOP_AUTH = "Basic " + base64.b64encode(b"admin:testcoShopApiPassword00").decode()  # fixed test credential


def http(method, url, data=None, headers=None, form=False):
    body = None
    if data is not None:
        body = urllib.parse.urlencode(data).encode() if form else json.dumps(data).encode()
    req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    if body is not None:
        req.add_header("Content-Type", "application/x-www-form-urlencoded" if form else "application/json")
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.status, json.loads(r.read() or b"null")


def odoo(service, method, *args):
    _, out = http("POST", ACCOUNTING + "/jsonrpc",
                  {"jsonrpc": "2.0", "method": "call", "params": {"service": service, "method": method, "args": list(args)}})
    if out.get("error"):
        raise RuntimeError(out["error"]["data"]["message"])
    return out["result"]


def check(name, fn):
    try:
        print(f"ok    {name}: {fn()}")
    except Exception as e:  # noqa: BLE001 (a smoke test: report whatever went wrong)
        print(f"FAIL  {name}: {e}")
        sys.exit(1)


def payments():
    auth = "Basic " + base64.b64encode(b"sk_test_123:").decode()
    status, c = http("POST", PAYMENTS + "/v1/customers", {"email": "smoke@example.com"}, {"Authorization": auth}, form=True)
    assert status == 200 and c["object"] == "customer", c
    return f"created customer {c['id']}"


def accounting():
    uid = odoo("common", "authenticate", "testco", "admin", "admin", {})
    assert uid, "admin login failed"
    fields = odoo("object", "execute_kw", "testco", uid, "admin", "account.move", "fields_get", [], {"attributes": ["type"]})
    assert "invoice_date" in fields and "partner_id" in fields, sorted(fields)[:10]
    partners = odoo("object", "execute_kw", "testco", uid, "admin", "res.partner", "search_count", [[]])
    return f"logged in (uid {uid}); account.move has {len(fields)} fields; {partners} partners"


def automation():
    status, out = http("GET", AUTOMATION + "/healthz")
    assert status == 200 and out.get("status") == "ok", out
    return "healthy"


def shop():
    status, s = http("GET", SHOP + "/settings/general/woocommerce_currency", headers={"Authorization": SHOP_AUTH})
    assert status == 200 and s["value"] == "GBP", s
    status, orders = http("GET", SHOP + "/orders?per_page=1", headers={"Authorization": SHOP_AUTH})
    return f"WooCommerce answers, currency GBP, {'orders loaded' if orders else 'no orders yet'}"


if __name__ == "__main__":
    check("payments (stripe-mock)", payments)
    check("accounting (Odoo)", accounting)
    check("automation (n8n)", automation)
    check("shop (WooCommerce)", shop)
    print("all services answer")
