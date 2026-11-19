"""Prototype for feat/trace-verify: recall of the JS frontend on real code, from V8 CPU profiles.

Each parent -> child frame pair in a profile where both are project functions is a call seen at
runtime. Getters/setters (attrs.accessor) are skipped, since they run on property access, and a
constructor frame counts as a call to its class, as `new X()` is drawn. Sampling misses rare calls,
so this measures recall on the calls that were seen. Usage:

    optum graph <project> -q -o graph.json
    node --cpu-prof --cpu-prof-interval 5 --cpu-prof-dir prof/ <entry>.js ...   # one or more runs
    python3 -I tests/tools/js_profile_recall.py graph.json <project> prof/

Known artifact: subclasses without their own constructor run an implicit one that calls the
parent's; it shows up as the module calling the base class. See docs/notes/2026-11-18-js-frontend-eval.md.
"""
import bisect, collections, glob, json, os, random, sys
graph_path, root, prof_dir = sys.argv[1:4]
root = os.path.realpath(root)
g = json.load(open(graph_path))
static = {(e["from"], e["to"]) for e in g["edges"] if e["kind"] == "calls"}
accessors = {n["id"] for n in g["nodes"] if n.get("attrs", {}).get("accessor")}
# Named defs per file, by line span: map a runtime frame to the innermost named def containing it.
spans = collections.defaultdict(list)
for n in g["nodes"]:
    if n.get("frontend") == "js" and "loc" in n and n["kind"] in ("function", "module"):
        l = n["loc"]; spans[l["file"]].append((l.get("line", 1), l.get("end_line", 10**9), n["id"], n["kind"]))
def frame_id(cf):
    url = cf["url"]
    if not url.startswith("file://"): return None, None
    path = os.path.realpath(url[7:])
    if not path.startswith(root + "/") or "/node_modules/" in path[len(root):]: return None, None
    rel = os.path.relpath(path, root)
    line = cf["lineNumber"] + 1
    best, exact = None, False
    for (a, b, i, k) in spans.get(rel, []):
        if a <= line <= b and (best is None or a >= best[0]):
            best = (a, i); exact = (a == line and cf["functionName"] != "")
    return (best[1] if best else None), exact
seen = collections.Counter(); exact_frames = collections.Counter()
for f in glob.glob(os.path.join(prof_dir, "*.cpuprofile")):
    p = json.load(open(f))
    by_id = {n["id"]: n for n in p["nodes"]}
    parent = {}
    for n in p["nodes"]:
        for c in n.get("children", []): parent[c] = n["id"]
    for n in p["nodes"]:
        pid = parent.get(n["id"])
        if pid is None: continue
        callee, ex = frame_id(n["callFrame"])
        caller, _ = frame_id(by_id[pid]["callFrame"])
        # Only a callee frame that starts a named function counts: anonymous callbacks fold into
        # their enclosing function and module top-level code isn't a call target.
        if callee in accessors: continue  # getters/setters run on property access, not a written call
        if callee and callee.endswith(".constructor") and (caller, callee) not in static:
            callee = callee[: -len(".constructor")]  # `new X()` is an edge to the class X
        if callee and caller and ex and callee != caller:
            seen[(caller, callee)] += 1
obs = set(seen)
hit = obs & static
print(f"observed call edges: {len(obs)} | in the static graph: {len(hit)} ({100*len(hit)/max(1,len(obs)):.1f}%)")
miss = sorted(obs - static)
random.seed(2)
for a, b in random.sample(miss, min(25, len(miss))): print("  MISS", a, "->", b)
