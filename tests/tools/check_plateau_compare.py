#!/usr/bin/env python3
"""tests/plateau/compare.py on a synthetic before/after pair: gains, relabels and losses are told
apart, per `via` label and per category, and a loss fails the run whatever the totals say."""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "plateau"))
from compare import compare  # noqa: E402


def edge(a, b, via=None):
    e = {"kind": "calls", "from": a, "to": b, "confidence": "exact" if via is None else "inferred"}
    if via:
        e["attrs"] = {"via": via}
    return e


before = {"edges": [edge("f", "g"), edge("f", "h", "unique_method_name"), edge("g", "k", "override"),
                    {"kind": "defines", "from": "m", "to": "f", "confidence": "exact"}]}
after = {"edges": [edge("f", "g"), edge("f", "h", "local_type"), edge("h", "z", "class_var"),
                   edge("q", "r", "class_var")]}
trace = {"calls": [{"from": "f", "to": "g"}, {"from": "f", "to": "h"}, {"from": "g", "to": "k"},
                   {"from": "h", "to": "z"}]}
labels = {"g -> k": "K3", "h -> z": "K4"}

r = compare(before, after, trace, labels)
fails = []
check = lambda cond, what: cond or fails.append(what)  # noqa: E731
check(r["observed"] == 4, "observed")
check(r["recall_before"] == 75.0 and r["recall_after"] == 75.0, "recall: same total, different edges")
check(r["gained"] == 1, "h -> z gained")
check(r["lost"] == ["g -> k"], "g -> k lost")
check(r["relabelled"] == ["f -> h: unique_method_name -> local_type"], "f -> h relabelled")
check(r["per_via"]["unique_method_name"]["removed_confirmed_relabelled"] == 1, "relabel counted as relabel")
check(r["per_via"]["override"]["removed_confirmed_gone"] == 1, "loss counted under its label")
check(r["per_via"]["class_var"]["added"] == 2 and r["per_via"]["class_var"]["added_unexercised"] == 1,
      "unexercised added edge (q -> r) flagged for a hand check")
check(r["per_category"]["K3"] == {"observed": 1, "found_before": 1, "found_after": 0}, "K3 regressed")
check(r["per_category"]["K4"] == {"observed": 1, "found_before": 0, "found_after": 1}, "K4 improved")

# The command line fails on a loss even though recall is unchanged.
with tempfile.TemporaryDirectory() as d:
    paths = {}
    for name, obj in (("b", before), ("a", after), ("t", trace)):
        paths[name] = os.path.join(d, name + ".json")
        json.dump(obj, open(paths[name], "w"))
    cmd = [sys.executable, os.path.join(HERE, "..", "plateau", "compare.py"),
           "--before", paths["b"], "--after", paths["a"], "--trace", paths["t"]]
    check(subprocess.run(cmd, capture_output=True).returncode == 1, "a loss exits 1")
    check(subprocess.run(cmd + ["--allow-lost", "1"], capture_output=True).returncode == 0, "--allow-lost")

if fails:
    sys.exit("FAIL: " + "; ".join(fails))
print("plateau compare: ok")
