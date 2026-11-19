#!/usr/bin/env python3
"""Optum frontend adapter for n8n workflow exports (docs/protocol.md).

Reads workflow JSON files (as exported from n8n) and describes them as a graph:
  - each workflow is a `program` node, `n8n:<workflow id>`
  - each n8n node is a `step`, `n8n:<workflow id>/<node name>`, defined by its workflow
  - each connection is a `triggers` edge between steps
  - HTTP Request steps reach `external` nodes with the same ids Optum's code frontends use
    (`http:<host><path>`), so a script and a workflow calling the same API share one node
  - Postgres/MySQL/MSSQL steps with a table reach `store` nodes (`sql:<table>`)
  - steps for a SaaS integration (Slack, HubSpot, ...) reach `external` nodes `saas:<service>`

Python 3.8+, standard library only. Run `optum_n8n.py describe` or `optum_n8n.py extract`.
"""
import json
import os
import re
import sys

PROTOCOL = "optum-frontend/0.1"
IR_VERSION = "0.1.0"
NAME = "n8n"

# n8n node types that are logic or plumbing, not a connection to another system.
CORE_TYPES = {
    "if", "switch", "merge", "set", "code", "function", "functionItem", "noOp", "wait",
    "splitInBatches", "splitOut", "aggregate", "filter", "sort", "limit", "removeDuplicates",
    "itemLists", "dateTime", "crypto", "html", "xml", "markdown", "renameKeys", "compareDatasets",
    "stopAndError", "executeWorkflow", "executeWorkflowTrigger", "respondToWebhook", "stickyNote",
    "manualTrigger", "scheduleTrigger", "cron", "interval", "webhook", "errorTrigger", "start",
}
TRIGGERS = {"manualTrigger": "manual", "scheduleTrigger": "schedule", "cron": "schedule",
            "interval": "schedule", "webhook": "webhook", "errorTrigger": "error", "start": "manual"}
SQL_TYPES = {"postgres", "mySql", "microsoftSql"}
SQL_WRITE_OPS = {"insert", "update", "upsert", "delete", "deleteTable", "createTable"}
SQL_READ_OPS = {"select", "get", "getAll"}
READ_OPS = {"get", "getAll", "getMany", "search", "list", "read", "lookup"}


def major_minor(version):
    """'optum-frontend/0.1.3' -> 'optum-frontend/0.1': requests may differ in patch only."""
    return ".".join(version.split(".")[:2])


def describe():
    return {"protocol": PROTOCOL, "name": NAME, "version": "0.1.0", "file_patterns": ["*.json"],
            "languages": ["n8n"]}


# ---- shared id rules (must match Core/src/http.cpp and sql.cpp) ----

def http_node_id(url):
    """Same rules as optum::http_node_id: drop scheme, default port, query, fragment."""
    m = re.match(r"^(https?)://(.*)$", url)
    if not m:
        return ""
    scheme, rest = m.group(1), re.split(r"[?#]", m.group(2), maxsplit=1)[0]
    host, _, path = rest.partition("/")
    path = "/" + path if path or rest.endswith("/") else ""
    if not host or "{}" in host:
        return ""
    if (scheme == "https" and host.endswith(":443")) or (scheme == "http" and host.endswith(":80")):
        host = host.rsplit(":", 1)[0]
    while len(path) > 1 and path.endswith("/"):
        path = path[:-1]
    if path == "/":
        path = ""
    return "http:" + host.lower() + path


def sql_store_id(table):
    for schema in ("public.", "dbo.", "main."):
        if len(table) > len(schema) and table[: len(schema)].lower() == schema:
            table = table[len(schema):]
            break
    return "sql:" + table


def expression_to_template(value):
    """n8n expressions start with '=' and embed {{ ... }}; unknown parts become {}."""
    if not isinstance(value, str):
        return None
    if value.startswith("="):
        value = value[1:]
    return re.sub(r"\{\{.*?\}\}", "{}", value).strip()


def param_value(v):
    """Newer n8n versions wrap some parameters as {"__rl": true, "value": ...}."""
    if isinstance(v, dict) and "value" in v:
        return v["value"]
    return v


def is_workflow(doc):
    return isinstance(doc, dict) and isinstance(doc.get("nodes"), list) and isinstance(doc.get("connections"), dict)


class Builder:
    def __init__(self):
        self.nodes = {}
        self.edges = {}

    def node(self, id_, kind, name, **extra):
        n = self.nodes.setdefault(id_, {"id": id_, "kind": kind, "name": name, "frontend": NAME})
        n.update(extra)

    def edge(self, a, b, kind, **extra):
        e = self.edges.setdefault((a, b, kind), {"from": a, "to": b, "kind": kind, "frontend": NAME})
        e.update(extra)

    def graph(self, source):
        return {"ir_version": IR_VERSION, "source": source,
                "nodes": list(self.nodes.values()), "edges": list(self.edges.values())}


def add_workflow(b, doc, rel, warn):
    wf_id = str(doc.get("id") or os.path.splitext(os.path.basename(rel))[0])
    prog = "n8n:" + wf_id
    b.node(prog, "program", doc.get("name") or wf_id, loc={"file": rel},
           attrs={"active": bool(doc.get("active", False))})
    steps = {}
    for n in doc["nodes"]:
        if not isinstance(n, dict):
            continue  # not an n8n node: skip rather than fail the whole run
        name = n.get("name")
        if not isinstance(name, str) or not name:
            continue
        step = f"{prog}/{name}"
        steps[name] = step
        ntype = str(n.get("type", ""))
        short = ntype.rsplit(".", 1)[-1]
        attrs = {"type": ntype}
        if short in TRIGGERS:
            attrs["trigger"] = TRIGGERS[short]
        if n.get("disabled"):
            attrs["disabled"] = True
        b.node(step, "step", name, loc={"file": rel}, attrs=attrs)
        b.edge(prog, step, "defines")
        params = n.get("parameters")
        add_step_effects(b, step, short, params if isinstance(params, dict) else {}, rel, warn)

    for src, outputs in doc["connections"].items():
        if src not in steps or not isinstance(outputs, dict):
            continue
        for branches in outputs.values():
            for branch in branches if isinstance(branches, list) else []:
                for target in branch if isinstance(branch, list) else []:
                    dst = target.get("node") if isinstance(target, dict) else None
                    if dst in steps:
                        b.edge(steps[src], steps[dst], "triggers")


def add_step_effects(b, step, short, params, rel, warn):
    if short == "httpRequest":
        method = str(param_value(params.get("method", params.get("requestMethod", "GET")))).upper()
        url = expression_to_template(param_value(params.get("url", "")))
        node_id = http_node_id(url or "")
        if not node_id:
            warn(f"{rel}: step '{step}': can't tell which host '{params.get('url')}' calls")
            return
        conf = "inferred" if "{}" in node_id else "exact"
        b.node(node_id, "external", node_id[5:], confidence=conf)
        kind = "reads" if method in ("GET", "HEAD", "OPTIONS") else "sends_to"
        b.edge(step, node_id, kind, confidence=conf, attrs={"method": method})
    elif short in SQL_TYPES:
        op = str(params.get("operation", "executeQuery"))
        table = param_value(params.get("table"))
        schema = param_value(params.get("schema"))
        if not isinstance(table, str) or not table or op == "executeQuery":
            warn(f"{rel}: step '{step}': SQL '{op}' without a table parameter is not analysed yet")
            return
        full = f"{schema}.{table}" if isinstance(schema, str) and schema else table
        store = sql_store_id(full)
        b.node(store, "store", store[4:])
        b.edge(step, store, "reads" if op in SQL_READ_OPS else "writes", attrs={"operation": op})
    elif short not in CORE_TYPES and short:
        service = re.sub(r"(Trigger|Tool)$", "", short)
        ext = "saas:" + service.lower()
        b.node(ext, "external", service, attrs={"service": service})
        op = str(params.get("operation", ""))
        b.edge(step, ext, "reads" if op in READ_OPS else "sends_to",
               attrs={"operation": op} if op else {})


def extract(request):
    root = request["root"]
    b = Builder()
    warnings = []
    for rel in request["files"]:
        try:
            with open(os.path.join(root, rel), encoding="utf-8") as f:
                doc = json.load(f)
        except (OSError, ValueError):
            continue  # not JSON we can read: not ours
        docs = doc if isinstance(doc, list) else [doc]  # n8n can export several at once
        for d in docs:
            if is_workflow(d):
                add_workflow(b, d, rel, warnings.append)
    for w in warnings:
        print(w, file=sys.stderr)
    return b.graph(root)


def main(argv):
    if len(argv) != 2 or argv[1] not in ("describe", "extract"):
        print("usage: optum_n8n.py describe|extract  (extract reads a request on stdin)", file=sys.stderr)
        return 2
    if argv[1] == "describe":
        out = describe()
    else:
        request = json.load(sys.stdin)
        if major_minor(str(request.get("protocol", ""))) != major_minor(PROTOCOL):
            print(f"unsupported protocol {request.get('protocol')!r}", file=sys.stderr)
            return 3
        out = extract(request)
    json.dump(out, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
