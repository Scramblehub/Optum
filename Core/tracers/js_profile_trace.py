#!/usr/bin/env python3
"""Turn V8 CPU profiles of a Node program into an Optum trace.

    node --cpu-prof --cpu-prof-interval 5 --cpu-prof-dir prof/ app.js ...   # one or more runs
    optum graph app/ -q -o graph.json
    python3 js_profile_trace.py --graph graph.json --root app/ --profiles prof/ -o trace.json
    optum verify graph.json trace.json

Each parent -> child frame pair in a profile where both are project functions is an observed call.
Frames are mapped to the graph's JS functions by file and line, so anonymous callbacks count as
their enclosing named function, as in the graph. Left out: getters and setters (attrs.accessor,
they run on property access), and frames that don't start a named function. A constructor frame
counts as a call to its class (`new X()` is drawn that way), unless the graph has an explicit call
to that constructor (`super(...)`). A class without a constructor runs an implicit one, a frame
at the class declaration: it counts as the class (`new Sub()`), and the base constructor it calls
(an implicit `super(...)`, not in the source) is left out. The profiler samples, so rare calls can be missed: this
measures recall on the calls that were seen.
"""
import argparse
import collections
import glob
import json
import os


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--graph", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--profiles", required=True, help="directory of .cpuprofile files")
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()
    root = os.path.realpath(args.root)
    g = json.load(open(args.graph))
    calls_in_graph = {(e["from"], e["to"]) for e in g["edges"] if e["kind"] == "calls"}
    accessors = {n["id"] for n in g["nodes"] if n.get("attrs", {}).get("accessor")}
    spans = collections.defaultdict(list)
    classes = {}  # (file, line, name) -> class id: where implicit constructors' frames point
    for n in g["nodes"]:
        if n.get("frontend") == "js" and "loc" in n and n["kind"] == "data_type":
            classes[(n["loc"]["file"], n["loc"].get("line", 1), n.get("name"))] = n["id"]
        if n.get("frontend") == "js" and "loc" in n and n["kind"] in ("function", "module"):
            loc = n["loc"]
            spans[loc["file"]].append((loc.get("line", 1), loc.get("end_line", 10**9), n["id"]))

    def frame_id(cf):
        url = cf["url"]
        if not url.startswith("file://"):
            return None, False
        path = os.path.realpath(url[7:])
        if not path.startswith(root + os.sep) or "/node_modules/" in path[len(root):]:
            return None, False
        rel = os.path.relpath(path, root)
        line = cf["lineNumber"] + 1
        implicit = classes.get((rel, line, cf["functionName"]))
        if implicit:
            return implicit, True
        best, starts_here = None, False
        for a, b, i in spans.get(rel, []):
            if a <= line <= b and (best is None or a >= best[0]):
                best = (a, i)
                starts_here = a == line and cf["functionName"] != ""
        return (best[1] if best else None), starts_here

    implicit_ids = set(classes.values())
    calls = collections.Counter()
    for f in glob.glob(os.path.join(args.profiles, "*.cpuprofile")):
        p = json.load(open(f))
        by_id = {n["id"]: n for n in p["nodes"]}
        parent = {c: n["id"] for n in p["nodes"] for c in n.get("children", [])}
        for n in p["nodes"]:
            pid = parent.get(n["id"])
            if pid is None:
                continue
            callee, starts = frame_id(n["callFrame"])
            caller, _ = frame_id(by_id[pid]["callFrame"])
            if not callee or not caller or not starts or callee in accessors:
                continue
            if caller in implicit_ids and callee.endswith(".constructor"):
                continue  # an implicit constructor's super(...)
            if callee.endswith(".constructor") and (caller, callee) not in calls_in_graph:
                callee = callee[: -len(".constructor")]
            if callee != caller:
                calls[(caller, callee)] += 1
    with open(args.out, "w") as f:
        json.dump({"trace_version": "0.1", "source": root, "language": "javascript",
                   "calls": [{"from": a, "to": b, "count": c} for (a, b), c in sorted(calls.items())]}, f, indent=1)
    print(f"{len(calls)} distinct calls observed in {len(glob.glob(os.path.join(args.profiles, '*.cpuprofile')))} profiles")


if __name__ == "__main__":
    main()
