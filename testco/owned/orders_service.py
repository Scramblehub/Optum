#!/usr/bin/env python3
"""The owned orders service: replaces the company's automation glue in stage 1
(docs/plans/phase3.md). Written by hand (for now) from the learned spec (operations on the shop and accounting, the
order lifecycle pending -> processing -> completed), on the platform's rules: idempotent writes,
an append-only audit log, one owner per step.

Old glue it replaces (three tools, polling every 5 minutes):
  payments webhook (Node)  charge succeeded -> order paid
  n8n "Orders to accounting" and nightly_sync.py (both!)  paid order -> invoice -> completed
New (one service, on the payment event): charge succeeded -> order paid -> one invoice -> completed.

    MODE=shadow  works out what it would do and writes it to /data/stage1/shadow/, changes nothing
    MODE=live    does it
The mode file /data/stage1/mode (written by stages/stage1.py) wins over the environment, so the
stage harness can switch it without a restart.
"""
import datetime
import http.server
import json
import os
import sys

import requests

sys.path.insert(0, "/company")
from common import ACCOUNTING, ACCOUNTING_DB, ACCOUNTING_PASSWORD, SHOP, SHOP_AUTH  # noqa: E402

STATE = "/data/stage1"
os.makedirs(os.path.join(STATE, "shadow"), exist_ok=True)


def mode():
    try:
        return open(os.path.join(STATE, "mode")).read().strip()
    except OSError:
        return os.environ.get("MODE", "shadow")


def audit(event, **fields):
    """Append-only audit log: who did what, when, with what before and after."""
    line = {"at": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds"),
            "event": event, "mode": mode(), **fields}
    with open(os.path.join(STATE, "audit.jsonl"), "a") as f:
        f.write(json.dumps(line) + "\n")


def odoo(model, method, *args, **kw):
    body = {"jsonrpc": "2.0", "method": "call", "params": {
        "service": "object", "method": "execute_kw",
        "args": [ACCOUNTING_DB, 2, ACCOUNTING_PASSWORD, model, method, list(args), kw]}}
    out = requests.post(ACCOUNTING, json=body, timeout=60).json()
    if out.get("error"):
        raise RuntimeError(out["error"]["data"]["message"])
    return out["result"]


def invoice_for(order):
    """The invoice an order needs: what the old glue writes, field for field."""
    b = order["billing"]
    return {"ref": f"order {order['id']}", "partner_email": b["email"].strip().lower(),
            "partner_name": f"{b['first_name']} {b['last_name']}",
            "lines": [{"name": li["name"], "quantity": li["quantity"], "price_unit": float(li["price"])}
                      for li in order["line_items"]],
            "total": float(order["total"])}


def handle_payment(order_id, charge_id):
    order = requests.get(f"{SHOP}/orders/{order_id}", auth=SHOP_AUTH, timeout=60).json()
    wanted = invoice_for(order)
    if mode() == "shadow":
        json.dump({"order": order_id, "charge": charge_id, "invoice": wanted},
                  open(os.path.join(STATE, "shadow", f"{order_id}.json"), "w"))
        audit("shadow_invoice", order=order_id, charge=charge_id)
        return
    # Live: every step checks first, so a repeated or retried event changes nothing twice.
    if order["status"] == "pending":
        requests.put(f"{SHOP}/orders/{order_id}", auth=SHOP_AUTH, timeout=60,
                     json={"status": "processing", "set_paid": True, "transaction_id": charge_id})
        audit("order_paid", order=order_id, charge=charge_id, before="pending", after="processing")
    ref = wanted["ref"]
    moves = odoo("account.move", "search_read", [["ref", "=", ref], ["move_type", "=", "out_invoice"]],
                 fields=["state"])
    if not moves:
        partner = odoo("res.partner", "search", [["email", "=", wanted["partner_email"]]], limit=1)
        partner_id = partner[0] if partner else odoo(
            "res.partner", "create", {"name": wanted["partner_name"], "email": wanted["partner_email"]})
        tax = odoo("account.tax", "search", [["name", "=", "VAT 20% (incl.)"], ["type_tax_use", "=", "sale"]])[0]
        move = odoo("account.move", "create", {
            "move_type": "out_invoice", "partner_id": partner_id, "ref": ref,
            "invoice_line_ids": [[0, 0, {**ln, "tax_ids": [[6, 0, [tax]]]}] for ln in wanted["lines"]]})
        audit("invoice_created", order=order_id, invoice=move, total=wanted["total"])
        moves = [{"id": move, "state": "draft"}]
    # A draft left by an interrupted run is posted now, not skipped (a retry finishes the job).
    for m in moves:
        if m["state"] == "draft":
            odoo("account.move", "action_post", [m["id"]])
            audit("invoice_posted", order=order_id, invoice=m["id"], total=wanted["total"])
    requests.put(f"{SHOP}/orders/{order_id}", auth=SHOP_AUTH, timeout=60, json={"status": "completed"})
    audit("order_completed", order=order_id, before="processing", after="completed")


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/stripe-events":
            self.send_response(404)
            self.end_headers()
            return
        order = None
        try:
            event = json.loads(self.rfile.read(int(self.headers.get("content-length", 0))))
            if event.get("type") == "charge.succeeded":
                c = event["data"]["object"]
                order = c["metadata"]["order_id"]
                handle_payment(order, c["id"])
            self.send_response(200)
        except Exception as e:  # noqa: BLE001 (answer the event; the audit log keeps the reason)
            audit("error", order=order, error=f"{type(e).__name__}: {e}")
            self.send_response(500)
        self.end_headers()

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    print(f"owned orders service on :7000 (mode {mode()})", flush=True)
    http.server.ThreadingHTTPServer(("0.0.0.0", 7000), Handler).serve_forever()
