"""An adapter that misbehaves on purpose, to test how Optum handles it.

Usage: fake_adapter.py <mode> describe|extract
Modes: ok, bad_protocol, crash, not_json, bad_graph, slow
"""
import json
import sys
import time

mode, request = sys.argv[1], sys.argv[2]
if request == "describe":
    proto = "optum-frontend/9.0" if mode == "bad_protocol" else "optum-frontend/0.1"
    print(json.dumps({"protocol": proto, "name": "fake-" + mode, "file_patterns": ["*.fake"]}))
    sys.exit(0)

json.load(sys.stdin)
if mode == "crash":
    print("something broke inside the adapter", file=sys.stderr)
    sys.exit(3)
if mode == "not_json":
    print("this is not json")
elif mode == "bad_graph":
    print(json.dumps({"ir_version": "0.1.0", "nodes": [{"id": "x", "kind": "gizmo"}], "edges": []}))
elif mode == "slow":
    time.sleep(30)
else:
    print("fake adapter ran", file=sys.stderr)
    print(json.dumps({"ir_version": "0.1.0", "nodes": [{"id": "fake:a", "kind": "step"}], "edges": []}))
