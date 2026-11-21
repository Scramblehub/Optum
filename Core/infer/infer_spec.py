#!/usr/bin/env python3
"""A first behaviour spec for each tool, learned from its recorded traffic (docs/plans/phase3.md,
"Learning a tool we can't read"). Rough on purpose: it exists before it's good.

    infer_spec.py RECORDINGS_DIR [--n8n WORKFLOWS_DIR] [-o OUT_DIR]

Reads the recording proxy's JSON lines (testco/observe/record.py) and writes, per tool:

  OUT_DIR/<tool>.spec.json   operations (method and endpoint with ids folded to {}, or a JSON-RPC
                             model and method), how often each is called, by whom, with which
                             statuses; the fields sent and received with their types; the
                             resources (orders, account.move...) and their fields; and the status
                             changes seen for each resource (pending -> processing -> completed)
  OUT_DIR/spec.md            the same, for people
  OUT_DIR/observed.json      findings only traffic can show: automation flows that never ran
                             (with --n8n: a webhook flow whose webhook nobody called)

Everything here is evidence from traffic: a call counted, a field seen, a status change watched.
Nothing is read from a tool's source.
"""
import argparse
import collections
import glob
import json
import os
import re

ID_SEGMENT = re.compile(r"^(\d+|[0-9a-f]{8}-[0-9a-f-]{27,}|[0-9a-f]{24,}|[a-z]{2,4}_[A-Za-z0-9]{8,})$")


def template(path):
    """/wp-json/wc/v3/orders/2026 -> /wp-json/wc/v3/orders/{}; ids, uuids and Stripe-style ids fold."""
    return "/".join("{}" if ID_SEGMENT.match(seg) else seg for seg in path.split("/"))


def kind(v):
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, (int, float)):
        return "number"
    if isinstance(v, str):
        return "string"
    if isinstance(v, list):
        return "list"
    return "object"


def fields_of(value, prefix="", out=None, depth=0):
    """Field paths and the types seen: {"billing.email": {"string"}, "line_items[].quantity": {"number"}}."""
    out = out if out is not None else collections.defaultdict(set)
    if depth > 3:
        return out
    if isinstance(value, dict):
        for k, v in value.items():
            path = f"{prefix}{k}"
            out[path].add(kind(v))
            if isinstance(v, dict):
                fields_of(v, path + ".", out, depth + 1)
            elif isinstance(v, list) and v and isinstance(v[0], dict):
                fields_of(v[0], path + "[].", out, depth + 1)
    elif isinstance(value, list) and value and isinstance(value[0], dict):
        fields_of(value[0], prefix, out, depth + 1)
    return out


def rpc_call(rec):
    """An Odoo-style JSON-RPC call: (operation, model, the values or domain it carries), or None."""
    body = rec["request"].get("body")
    if not (isinstance(body, dict) and isinstance(body.get("params"), dict)):
        return None
    p = body["params"]
    args = p.get("args") if isinstance(p.get("args"), list) else []
    if p.get("service") == "object" and p.get("method") in ("execute_kw", "execute") and len(args) >= 5:
        model, method = args[3], args[4]
        rest = args[5] if len(args) > 5 else []
        values = None
        if method == "create" and rest:
            values = rest[0] if isinstance(rest[0], (dict, list)) else None
        elif method == "write" and len(rest) > 1:
            values = rest[1]
        return f"rpc {model}.{method}", str(model), values
    if p.get("service") in ("common", "db"):
        return f"rpc {p['service']}.{p.get('method')}", None, None
    return None


def resource_of(path):
    """The resource an endpoint is about: the last named segment (orders in /wc/v3/orders/{})."""
    named = [s for s in path.split("/") if s and s != "{}"]
    return named[-1] if named else path


def learn(records):
    ops = {}
    resources = collections.defaultdict(lambda: collections.defaultdict(set))
    states = collections.defaultdict(dict)  # resource -> record id -> [(time, status)]
    for r in sorted(records, key=lambda r: r.get("started") or ""):
        call = rpc_call(r)
        if call:
            op, model, values = call
            resource = model
        else:
            path = template(r["path"])
            op = f"{r['method']} {path}"
            resource = resource_of(path)
            values = r["request"].get("body") if r["method"] in ("POST", "PUT", "PATCH") else None
        o = ops.setdefault(op, {"calls": 0, "clients": collections.Counter(), "statuses": collections.Counter(),
                                "first": r.get("started"), "last": r.get("started"),
                                "request": collections.defaultdict(set), "response": collections.defaultdict(set),
                                "query": set(), "resource": resource})
        o["calls"] += 1
        o["clients"][r.get("client") or "unknown"] += 1
        o["statuses"][r["response"]["status"]] += 1
        o["last"] = r.get("started")
        o["query"].update(r.get("query") or {})
        if values is not None:
            fields_of(values if not isinstance(values, list) or not values or isinstance(values[0], dict) else {},
                      out=o["request"])
            if resource:
                for f, t in fields_of(values).items():
                    resources[resource][f] |= t
        resp = r["response"].get("body")
        if isinstance(resp, dict) and "result" in resp and call:
            resp = resp["result"]
        if isinstance(resp, (dict, list)):
            fields_of(resp, out=o["response"])
            if resource and not call:
                for f, t in fields_of(resp).items():
                    resources[resource][f] |= t
        # Status changes: a record's status as it was sent or returned, over time.
        if not call:
            rid = None
            if isinstance(resp, dict) and "id" in resp:
                rid = resp["id"]
            sent = r["request"].get("body") if isinstance(r["request"].get("body"), dict) else {}
            status = sent.get("status") or (resp.get("status") if isinstance(resp, dict) else None)
            if rid is not None and isinstance(status, str):
                seq = states[resource].setdefault(str(rid), [])
                if not seq or seq[-1] != status:
                    seq.append(status)
    transitions = {}
    for res, by_id in states.items():
        t = collections.Counter()
        for seq in by_id.values():
            for a, b in zip(seq, seq[1:]):
                t[f"{a} -> {b}"] += 1
        if t:
            transitions[res] = dict(t.most_common())
    return ops, resources, transitions


def to_json(ops, resources, transitions):
    def fmap(d):
        return {k: sorted(v) for k, v in sorted(d.items())}
    return {
        "spec_version": "0.1",
        "operations": {op: {"calls": o["calls"], "clients": dict(o["clients"].most_common()),
                            "statuses": {str(k): v for k, v in sorted(o["statuses"].items())},
                            "first_seen": o["first"], "last_seen": o["last"], "resource": o["resource"],
                            "query": sorted(o["query"]), "request_fields": fmap(o["request"]),
                            "response_fields": fmap(o["response"])}
                       for op, o in sorted(ops.items(), key=lambda kv: -kv[1]["calls"])},
        "resources": {res: fmap(f) for res, f in sorted(resources.items())},
        "status_changes": transitions,
    }


def n8n_webhooks(folder):
    """(flow id, flow name, method, path) for every webhook-triggered flow in a folder of exported flows."""
    out = []
    for f in sorted(glob.glob(os.path.join(folder, "*.json"))):
        try:
            wf = json.load(open(f))
        except (ValueError, OSError):
            continue
        for n in wf.get("nodes", []):
            if str(n.get("type", "")).endswith(".webhook"):
                p = n.get("parameters", {})
                out.append((wf.get("id") or os.path.basename(f), wf.get("name", ""), p.get("httpMethod", "GET"),
                            p.get("path", "")))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("recordings")
    ap.add_argument("--n8n", help="folder of the automation tool's exported flows")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    by_tool = collections.defaultdict(list)
    harness = 0
    for f in sorted(glob.glob(os.path.join(a.recordings, "*.jsonl"))):
        for line in open(f, encoding="utf-8"):
            if line.strip():
                r = json.loads(line)
                if r.get("harness"):
                    harness += 1  # Optum's own loading and checking tools: not the company's behaviour
                    continue
                by_tool[r["tool"]].append(r)
    os.makedirs(a.out, exist_ok=True)
    md = ["# Behaviour specs, learned from recorded traffic", ""]
    times = sorted(r["started"] for rs in by_tool.values() for r in rs if r.get("started"))
    window = [times[0], times[-1]] if times else [None, None]
    md.append(f"Recorded from {window[0]} to {window[1]}: "
              + ", ".join(f"{t} {len(rs)} calls" for t, rs in sorted(by_tool.items()))
              + f". Left out: {harness} calls from Optum's own harness (loading, checks).")
    md.append("")
    for tool, recs in sorted(by_tool.items()):
        spec = to_json(*learn(recs))
        spec["tool"] = tool
        json.dump(spec, open(os.path.join(a.out, f"{tool}.spec.json"), "w"), indent=1)
        md += [f"## {tool}", "", "| Operation | Calls | By | Statuses |", "|---|---|---|---|"]
        for op, o in spec["operations"].items():
            md.append(f"| `{op}` | {o['calls']} | {', '.join(f'{c} ({n})' for c, n in o['clients'].items())} | "
                      f"{', '.join(f'{s}: {n}' for s, n in o['statuses'].items())} |")
        if spec["status_changes"]:
            md += ["", "Status changes seen: " + "; ".join(
                f"{res}: " + ", ".join(f"{t} ({n})" for t, n in ts.items()) for res, ts in spec["status_changes"].items())]
        md.append("")
    findings = []
    if a.n8n:
        hits = collections.Counter(template(r["path"]) for r in by_tool.get("automation", []))
        for wf_id, name, method, path in n8n_webhooks(a.n8n):
            seen = hits.get(f"/webhook/{path}", 0) + hits.get(f"/webhook-test/{path}", 0)
            if seen == 0:
                findings.append({"kind": "never_triggered", "flow": f"n8n:{wf_id}", "name": name,
                                 "trigger": f"webhook {method} /webhook/{path}", "calls_seen": 0,
                                 "window": window,
                                 "evidence": f"no call to /webhook/{path} in {len(by_tool.get('automation', []))} "
                                             f"recorded calls to the automation tool"})
    json.dump({"window": window, "findings": findings}, open(os.path.join(a.out, "observed.json"), "w"), indent=1)
    if findings:
        md += ["## Observed findings", ""]
        md += [f"- **{f['name']}** (`{f['flow']}`) never ran: {f['evidence']}, {window[0]} to {window[1]}."
               for f in findings]
    open(os.path.join(a.out, "spec.md"), "w").write("\n".join(md) + "\n")
    print(f"{len(by_tool)} tools, {sum(len(r) for r in by_tool.values())} calls -> {a.out}; "
          f"{len(findings)} observed finding(s)")


if __name__ == "__main__":
    main()
