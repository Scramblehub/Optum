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

Skip-level artifacts. A sampled profile sometimes shows A calling C directly although the code is
A -> B -> C: B was inlined or its frame was missed in that sample. Rule: a call A -> C is a
skip-level artifact when its total count over all profiles is at most SKIP_MAX_COUNT (5), every
profile that has it also has one chain A -> B -> C (or A -> B -> X -> C, up to SKIP_MAX_EDGES
edges) of observed calls, and every call on that chain has a total at least SKIP_RATIO (2) times
the A -> C total. Artifacts are not put in "calls"; they are listed in the trace's
"skip_level_artifacts" (from, to, count, via: the chain's middle frames), which
`optum verify` ignores. `--keep-skip-level` turns the rule off. A real, rare direct call that
also happens through B would be set aside too: that is why they are listed, not deleted.
"""
import argparse
import collections
import glob
import json
import os

SKIP_MAX_COUNT = 5
SKIP_RATIO = 2
SKIP_MAX_EDGES = 3  # A -> B -> C, or A -> B -> X -> C


def _paths(pc, a, c):
    """Call chains a -> ... -> c of 2 to SKIP_MAX_EDGES edges among the edges of one profile (not a -> c itself)."""
    out_edges = collections.defaultdict(list)
    for (x, y) in pc:
        out_edges[x].append(y)
    found, frontier = set(), [(a,)]
    for _ in range(SKIP_MAX_EDGES):
        nxt = []
        for path in frontier:
            for y in out_edges.get(path[-1], ()):
                if y == c and len(path) >= 2:
                    found.add(path + (y,))
                elif y not in path and y != c:
                    nxt.append(path + (y,))
        frontier = nxt
    return found


def split_skip_level(per_profile):
    """per_profile: one Counter of (caller, callee) per profile -> (kept Counter, artifacts).

    artifacts: {(a, c): (count, via)}, via the path's middle frames. A pair A -> C is an artifact
    when its total over all profiles is at most SKIP_MAX_COUNT, every profile that has it also has
    one common chain A -> B -> C (or A -> B -> X -> C) of observed calls, and every call on that chain
    has a total at least SKIP_RATIO times the A -> C total.
    """
    total = collections.Counter()
    for pc in per_profile:
        total.update(pc)
    artifacts = {}
    for (a, c), n in total.items():
        if n > SKIP_MAX_COUNT:
            continue
        chains = None
        for pc in per_profile:
            if (a, c) in pc:
                here = _paths(pc, a, c)
                chains = here if chains is None else chains & here
                if not chains:
                    break
        for chain in sorted(chains or ()):
            if all(total[(x, y)] >= SKIP_RATIO * n for x, y in zip(chain, chain[1:])):
                artifacts[(a, c)] = (n, list(chain[1:-1]))
                break
    kept = collections.Counter({k: n for k, n in total.items() if k not in artifacts})
    return kept, artifacts


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--graph", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--profiles", required=True, help="directory of .cpuprofile files")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--keep-skip-level", action="store_true", help="count skip-level artifacts as calls")
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
    per_profile = []
    profiles = sorted(glob.glob(os.path.join(args.profiles, "*.cpuprofile")))
    if not profiles:
        ap.error(f"no .cpuprofile files in {args.profiles} (run node with --cpu-prof --cpu-prof-dir {args.profiles})")
    for f in profiles:
        p = json.load(open(f))
        by_id = {n["id"]: n for n in p["nodes"]}
        parent = {c: n["id"] for n in p["nodes"] for c in n.get("children", [])}
        pc = collections.Counter()
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
                pc[(caller, callee)] += 1
        per_profile.append(pc)
    if args.keep_skip_level:
        calls, skipped = sum(per_profile, collections.Counter()), {}
    else:
        calls, skipped = split_skip_level(per_profile)
    with open(args.out, "w") as f:
        json.dump({"trace_version": "0.1", "source": root, "language": "javascript",
                   "calls": [{"from": a, "to": b, "count": c} for (a, b), c in sorted(calls.items())],
                   "skip_level_artifacts": [{"from": a, "to": c, "count": n, "via": via}
                                            for (a, c), (n, via) in sorted(skipped.items())]}, f, indent=1)
    print(f"{len(calls)} distinct calls observed in {len(profiles)} profiles; "
          f"{len(skipped)} skip-level artifacts set aside")


if __name__ == "__main__":
    main()
