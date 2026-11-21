#!/usr/bin/env python3
"""infer_spec.py on hand-made recordings: operations, callers, fields, status changes, harness
calls left out, and a webhook flow nobody called."""
import json, os, subprocess, sys, tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
TOOL = os.path.join(ROOT, "Core", "infer", "infer_spec.py")


def rec(tool, method, path, client, req=None, resp=None, status=200, t="2026-11-20T10:00:00.000Z", harness=None):
    r = {"recording_version": "0.1", "tool": tool, "started": t, "duration_ms": 1, "method": method, "path": path,
         "query": {}, "request": {"headers": {}, "body": req}, "response": {"status": status, "headers": {}, "body": resp},
         "client": client}
    if harness:
        r["harness"] = harness
    return r


rpc = lambda model, method, *rest: {"jsonrpc": "2.0", "method": "call", "params": {
    "service": "object", "method": "execute_kw", "args": ["db", 2, "[redacted]", model, method, *rest]}}
recs = [
    rec("shop", "POST", "/api/orders", "driver", {"status": "pending", "total": "9.00"}, {"id": 41, "status": "pending"}, 201,
        "2026-11-20T10:00:01Z"),
    rec("shop", "PUT", "/api/orders/41", "hooks", {"status": "processing"}, {"id": 41, "status": "processing"}, 200, "2026-11-20T10:00:02Z"),
    rec("shop", "PUT", "/api/orders/41", "automation", {"status": "completed"}, {"id": 41, "status": "completed"}, 200, "2026-11-20T10:05:00Z"),
    rec("shop", "GET", "/api/orders/41", "checker", None, {"id": 41}, harness="testco"),
    rec("accounting", "POST", "/jsonrpc", "automation",
        rpc("account.move", "create", [{"partner_id": 7, "ref": "order 41", "invoice_line_ids": [[0, 0, {"name": "Mug"}]]}]),
        {"jsonrpc": "2.0", "result": 3}),
    rec("accounting", "POST", "/jsonrpc", "company", rpc("account.move", "search_count", [[["ref", "=", "order 41"]]]),
        {"jsonrpc": "2.0", "result": 1}),
]
with tempfile.TemporaryDirectory() as d:
    os.makedirs(os.path.join(d, "rec")); os.makedirs(os.path.join(d, "n8n"))
    for tool in ("shop", "accounting"):
        with open(os.path.join(d, "rec", f"{tool}.jsonl"), "w") as f:
            f.writelines(json.dumps(r) + "\n" for r in recs if r["tool"] == tool)
    json.dump({"id": "cart", "name": "Cart reminder", "nodes": [{"name": "Hook", "type": "n8n-nodes-base.webhook",
              "parameters": {"httpMethod": "POST", "path": "abandoned"}}]}, open(os.path.join(d, "n8n", "cart.json"), "w"))
    subprocess.run([sys.executable, TOOL, os.path.join(d, "rec"), "--n8n", os.path.join(d, "n8n"), "-o", os.path.join(d, "out")],
                   check=True, capture_output=True)
    shop = json.load(open(os.path.join(d, "out", "shop.spec.json")))
    acc = json.load(open(os.path.join(d, "out", "accounting.spec.json")))
    observed = json.load(open(os.path.join(d, "out", "observed.json")))

ops = shop["operations"]
assert set(ops) == {"POST /api/orders", "PUT /api/orders/{}"}, ops.keys()  # ids fold; the harness GET is left out
assert ops["PUT /api/orders/{}"]["clients"] == {"hooks": 1, "automation": 1}
assert "status" in ops["POST /api/orders"]["request_fields"] and ops["POST /api/orders"]["statuses"] == {"201": 1}
assert shop["status_changes"] == {"orders": {"pending -> processing": 1, "processing -> completed": 1}}, shop["status_changes"]
assert set(acc["operations"]) == {"rpc account.move.create", "rpc account.move.search_count"}
assert "invoice_line_ids" in acc["resources"]["account.move"] and "ref" in acc["resources"]["account.move"]
assert [f["flow"] for f in observed["findings"]] == ["n8n:cart"] and observed["findings"][0]["kind"] == "never_triggered"
print("infer_spec: operations, callers, fields, status changes, harness left out, an unused flow found")
