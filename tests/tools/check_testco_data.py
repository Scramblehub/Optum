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
