"""Checks that an adapter's copies of the shared id rules match tests/fixtures/ids/url_cases.json.

Usage: check_shared_ids.py CASES_JSON ADAPTER_DIR MODULE
"""
import importlib
import json
import sys

cases_path, adapter_dir, module = sys.argv[1:4]
sys.path.insert(0, adapter_dir)
adapter = importlib.import_module(module)
cases = json.load(open(cases_path))
bad = [(u, want, adapter.http_node_id(u)) for u, want in cases["cases"] if adapter.http_node_id(u) != want]
bad += [(t, want, adapter.sql_store_id(t)) for t, want in cases["sql_cases"] if adapter.sql_store_id(t) != want]
for given, want, got in bad:
    print(f"{module}: {given!r} -> {got!r}, expected {want!r}")
sys.exit(1 if bad else 0)
