#!/usr/bin/env python3
"""optum model on the test company against its answer key (tests/fixtures/model/expected.json):
each component's replaceability class, the reasons that matter (what only the webhook does, who
covers the n8n flow, the same invoice fields), weakest-evidence confidence, and staleness when a
source file changes.

    check_model.py OPTUM_BINARY PYTHON"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

OPTUM, PY = sys.argv[1], sys.argv[2]
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
FIX = os.path.join(ROOT, "tests", "fixtures", "model")
expected = json.load(open(os.path.join(FIX, "expected.json")))
fails = []
check = lambda cond, what: cond or fails.append(what)  # noqa: E731

with tempfile.TemporaryDirectory() as d:
    code = os.path.join(d, "company")
    shutil.copytree(os.path.join(ROOT, "testco", "company"), code)
    out = os.path.join(d, "model.json")
    cmd = [OPTUM, "model", code, "--adapter", f"{PY} {os.path.join(ROOT, 'Core', 'adapters', 'n8n', 'optum_n8n.py')}",
           "--recordings", os.path.join(FIX, "recordings"), "--n8n", os.path.join(code, "n8n"),
           "--deployment", os.path.join(ROOT, "testco", "deployment.json"), "-f", "json", "-o", out]
    subprocess.run(cmd, check=True, capture_output=True)
    m = json.load(open(out))
    comps = {c["id"]: c for c in m["components"]}
    for cid, cls in expected["components"].items():
        got = comps.get(cid, {}).get("replaceability", {}).get("class")
        check(got == cls, f"{cid}: expected {cls}, got {got}")
    for sys_name, cls in expected["systems"].items():
        got = next((s["replaceability"]["class"] for s in m["systems"] if s["name"] == sys_name), None)
        check(got == cls, f"system {sys_name}: expected {cls}, got {got}")

    hook = comps["js:payments_webhook.js"]["replaceability"]
    check(any("processing" in u for u in hook["properties"]["functional"]["unique"]),
          "the webhook is essential because only it marks orders paid (status processing)")
    flow = comps["n8n:orders-to-accounting"]["replaceability"]
    check(flow["properties"]["functional"]["covered_by"] == ["py:nightly_sync"], "the n8n flow is covered by the nightly script")
    check(all(x["same_fields"] for x in flow["properties"]["data"]), "the same invoice fields (data equivalence)")
    check(flow["properties"]["behavioural"].startswith("not shown"), "behavioural equivalence is not claimed")
    check(any(e.startswith("mutual with py:nightly_sync") for e in flow["evidence"]), "mutual cover: keep one of the two")
    check(flow["confidence"] in ("inferred", "declared"), "never more certain than the declared deployment")
    cart = comps["n8n:abandoned-cart-reminder"]["replaceability"]
    check(cart["confidence"] == "recorded" and "never ran" in cart["reason"], "the cart flow never ran (recorded)")
    check("anyone needs it" in comps["py:vip_report"]["replaceability"]["reason"], "the report: unknown, and why")

    # Staleness: change one source file; exactly the conclusions resting on it go stale.
    fresh = subprocess.run([OPTUM, "model", "--check", out], capture_output=True, text=True)
    check(fresh.returncode == 0 and json.loads(fresh.stdout)["stale"] == [], "a fresh model has nothing stale")
    with open(os.path.join(code, "vip_report.py"), "a") as f:
        f.write("# changed\n")
    stale = subprocess.run([OPTUM, "model", "--check", out], capture_output=True, text=True)
    subjects = sorted(s["subject"] for s in json.loads(stale.stdout)["stale"])
    check(stale.returncode == 1 and subjects == ["py:common", "py:vip_report"],
          f"vip_report changed: its conclusion and common's (used by it) go stale, nothing else; got {subjects}")

if fails:
    sys.exit("FAIL:\n  " + "\n  ".join(fails))
print("optum model: answer key matched; reasons, confidence and staleness as expected")
