#!/usr/bin/env python3
"""The test company's generator: deterministic, with its traps in place (testco/README.md)."""
import json, os, subprocess, sys, tempfile

GEN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "testco", "generate.py")
with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
    for d in (a, b):
        subprocess.run([sys.executable, GEN, "-o", d, "--people", "200", "--orders", "500"], check=True, capture_output=True)
    for f in sorted(os.listdir(a)):
        assert open(os.path.join(a, f)).read() == open(os.path.join(b, f)).read(), f"not deterministic: {f}"
    truth = json.load(open(os.path.join(a, "ground_truth.json")))
    def recs(tool):
        return {r["id"]: r for r in json.load(open(os.path.join(a, tool + ".json")))["records"]}
    crm = recs("crm")
    smiths = [r for r in crm.values() if (r["first_name"], r["last_name"]) == ("John", "Smith")]
    owners = {p for p in ("P0000", "P0001") for x in truth["people"][p] if x["tool"] == "crm"}
    assert len(smiths) >= 2 and owners == {"P0000", "P0001"}, "two John Smiths, two people"
    emails = {r["email"] for tool in ("crm", "shop", "helpdesk") for x in truth["people"]["P0002"]
              if x["tool"] == tool for r in [recs(tool)[x["id"]]]}
    assert len(emails) == 2, f"one person, two emails: {emails}"
    assert len(truth["orders"]) == 500
    inv = [r for r in recs("accounting").values() if r.get("type") == "invoice"]
    assert inv and all(r["regulated"] == "tax" for r in inv), "invoices are tax documents"
print("testco data: deterministic, traps in place")

# Regulated records: the fingerprint sees any change to a tax document (check_regulated.py).
sys.path.insert(0, os.path.join(os.path.dirname(GEN)))
from check_regulated import compare, fingerprint  # noqa: E402
move = {"name": "INV/2026/00001", "move_type": "out_invoice", "state": "posted", "partner_id": [7, "A"],
        "invoice_date": "2026-11-19", "currency_id": [144, "GBP"], "amount_untaxed": 36.67, "amount_tax": 7.33,
        "amount_total": 44.0, "ref": "order 5000"}
lines = [{"id": 2, "name": "Mug", "quantity": 2, "price_unit": 12.0, "tax_ids": [1], "price_subtotal": 20.0, "price_total": 24.0},
         {"id": 1, "name": "Tee", "quantity": 1, "price_unit": 20.0, "tax_ids": [1], "price_subtotal": 16.67, "price_total": 20.0}]
fp = fingerprint(move, lines)
assert fp == fingerprint(dict(move), list(reversed(lines))), "stable: line order as stored, not as read"
assert fp != fingerprint({**move, "amount_total": 1.0}, lines), "a changed total"
assert fp != fingerprint(move, [{**lines[0], "price_unit": 1.0}, lines[1]]), "a changed line price"
assert compare({"A": "x", "B": "y", "C": "z"}, {"A": "x", "B": "Y"}) == {"changed": ["B"], "missing": ["C"]}
print("regulated fingerprints: any change to a tax document is seen")
