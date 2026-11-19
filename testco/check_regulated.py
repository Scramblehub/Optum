#!/usr/bin/env python3
"""Regulated records stay byte for byte unchanged (docs/plans/phase3.md, check 4).

    python3 testco/check_regulated.py snapshot   # fingerprint every posted invoice (data/regulated.json)
    python3 testco/check_regulated.py verify     # exit 1 and name every invoice that changed or vanished

Why Optum checks this itself: Odoo locks a posted invoice's partner and date, but its API still
lets the total and line prices be overwritten (found loading the test company). So a tool's own
locking isn't evidence; a fingerprint taken before a stage and compared after is.

Read-only against the tools: it only reads invoices.
"""
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
FIELDS = ["name", "move_type", "state", "partner_id", "invoice_date", "currency_id", "amount_untaxed",
          "amount_tax", "amount_total", "ref", "invoice_line_ids"]
LINE_FIELDS = ["name", "quantity", "price_unit", "tax_ids", "price_subtotal", "price_total"]


def fingerprint(move, lines):
    """A stable hash of what a tax document says: its header and its lines, in order."""
    doc = {f: move.get(f) for f in FIELDS if f != "invoice_line_ids"}
    doc["lines"] = [{f: ln.get(f) for f in LINE_FIELDS} for ln in sorted(lines, key=lambda x: x["id"])]
    return hashlib.sha256(json.dumps(doc, sort_keys=True, default=str).encode()).hexdigest()


def compare(before, after):
    """Invoices whose fingerprint changed, and those that are gone: {"changed": [...], "missing": [...]}."""
    return {"changed": sorted(k for k in before if k in after and after[k] != before[k]),
            "missing": sorted(k for k in before if k not in after)}


def live_fingerprints():
    from load import Odoo
    odoo = Odoo()
    ids = odoo("account.move", "search", [["state", "=", "posted"], ["move_type", "=", "out_invoice"]])
    out = {}
    for i in range(0, len(ids), 500):
        moves = odoo("account.move", "read", ids[i:i + 500], fields=FIELDS)
        line_ids = [lid for m in moves for lid in m["invoice_line_ids"]]
        lines = {ln["id"]: ln for ln in odoo("account.move.line", "read", line_ids, fields=LINE_FIELDS + ["id"])}
        for m in moves:
            out[m["name"]] = fingerprint(m, [lines[lid] for lid in m["invoice_line_ids"]])
    return out


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("snapshot", "verify"):
        sys.exit(__doc__)
    path = os.path.join(DATA, "regulated.json")
    now = live_fingerprints()
    if sys.argv[1] == "snapshot":
        json.dump(now, open(path, "w"), indent=0, sort_keys=True)
        print(f"fingerprinted {len(now)} posted invoices -> {path}")
        return
    diff = compare(json.load(open(path)), now)
    if diff["changed"] or diff["missing"]:
        print(f"REGULATED RECORDS CHANGED: {len(diff['changed'])} changed {diff['changed'][:10]}, "
              f"{len(diff['missing'])} missing {diff['missing'][:10]}")
        sys.exit(1)
    print(f"all {len(now)} posted invoices unchanged")


if __name__ == "__main__":
    main()
