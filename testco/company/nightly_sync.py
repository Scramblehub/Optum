#!/usr/bin/env python3
"""Nightly sync: paid shop orders become invoices in accounting, then the orders are completed.

Written by the company before it set up the n8n flow "Orders to accounting", which does the same
job; nobody switched this off. It checks for an existing invoice first, so the two don't invoice
an order twice: they just both do the work.
"""
import requests

from common import ACCOUNTING, ACCOUNTING_DB, ACCOUNTING_LOGIN, ACCOUNTING_PASSWORD, SHOP, SHOP_AUTH


def clean_address(street, city, postcode):
    """One address line, tidied: single spaces, title-case city, upper-case postcode."""
    parts = []
    for value in (street, city, postcode):
        value = " ".join(str(value or "").split())
        if not value:
            continue
        parts.append(value)
    if len(parts) >= 2:
        parts[1] = parts[1].title()
    if len(parts) == 3:
        parts[2] = parts[2].upper()
    return ", ".join(parts)


def normalize_email(email):
    """Lower case, no spaces, and Gmail-style dots kept (they matter for most providers)."""
    email = (email or "").strip().lower()
    if email.count("@") != 1:
        return None
    local, domain = email.split("@")
    if not local or "." not in domain:
        return None
    return local + "@" + domain


def odoo(method, model, args, kwargs=None):
    body = {"jsonrpc": "2.0", "method": "call", "params": {
        "service": "object", "method": "execute_kw",
        "args": [ACCOUNTING_DB, 2, ACCOUNTING_PASSWORD, model, method, args, kwargs or {}]}}
    out = requests.post(ACCOUNTING, json=body, timeout=60).json()
    if out.get("error"):
        raise RuntimeError(out["error"]["data"]["message"])
    return out["result"]


def partner_for(order):
    email = normalize_email(order["billing"]["email"])
    found = odoo("search", "res.partner", [[["email", "=", email]]], {"limit": 1})
    if found:
        return found[0]
    b = order["billing"]
    return odoo("create", "res.partner", [{"name": f"{b['first_name']} {b['last_name']}", "email": email,
                                          "street": clean_address(b["address_1"], b["city"], b["postcode"])}])


def vat_tax():
    return odoo("search", "account.tax", [[["name", "=", "VAT 20% (incl.)"], ["type_tax_use", "=", "sale"]]])[0]


def sync():
    orders = requests.get(SHOP + "/orders", params={"status": "processing", "per_page": 100},
                          auth=SHOP_AUTH, timeout=60).json()
    tax = vat_tax() if orders else None
    invoiced = 0
    for order in orders:
        ref = f"order {order['id']}"
        if not odoo("search_count", "account.move", [[["ref", "=", ref]]]):
            move = odoo("create", "account.move", [{
                "move_type": "out_invoice", "partner_id": partner_for(order), "ref": ref,
                "invoice_line_ids": [[0, 0, {"name": li["name"], "quantity": li["quantity"],
                                             "price_unit": float(li["price"]), "tax_ids": [[6, 0, [tax]]]}]
                                     for li in order["line_items"]]}])
            odoo("action_post", "account.move", [[move]])
            invoiced += 1
        requests.put(f"{SHOP}/orders/{order['id']}", json={"status": "completed"}, auth=SHOP_AUTH, timeout=60)
    print(f"nightly sync: {len(orders)} paid orders, {invoiced} invoiced here")


if __name__ == "__main__":
    sync()
