#!/usr/bin/env python3
"""Compare two Optum builds on the same traced run, relationship type by relationship type, so a
gain in one kind of edge can't hide a loss in another (docs/plans/plateau.md).

    compare.py --before G1.json --after G2.json --trace TRACE.json [--labels LABELS.json] [--json]

Inputs: two `optum graph` outputs for the same code (before and after a change) and one trace
(`Core/tracers`) of a real run. LABELS (optional) maps "caller -> callee" to a cause category from
an analyst's hand classification (K1, row B, ...): recall is then reported per category too.

Reports, before and after:
  recall                   observed calls the graph has an edge for
  per `via` label          call edges (exact, or the inference that produced them); how many the run
                           confirmed; added and removed; added edges the run never exercised (the
                           ones only a hand check can judge: precision risk)
  per category             recall within each labelled cause (with LABELS)
  relabelled               observed calls found both times, but by a different rule (normal when a
                           guess becomes exact; listed so a reader can check it)
  lost                     every observed call the before graph found and the after graph doesn't

Exit 1 when anything is lost (a regression, whatever the totals say), unless --allow-lost N.
"""
import argparse
import collections
import json
import sys


def call_edges(graph):
    """(from, to) -> the label that produced it: "exact", or the inference's `via`."""
    out = {}
    for e in graph["edges"]:
        if e.get("kind") != "calls":
            continue
        via = "exact" if e.get("confidence") == "exact" else (e.get("attrs") or {}).get("via") or "inferred"
        out.setdefault((e["from"], e["to"]), via)
    return out


def observed(trace):
    return {(c["from"], c["to"]) for c in trace["calls"]}


def compare(before, after, trace, labels=None):
    obs = observed(trace)
    b, a = call_edges(before), call_edges(after)
    found_b, found_a = obs & set(b), obs & set(a)
    vias = sorted(set(b.values()) | set(a.values()))
    per_via = {}
    for v in vias:
        eb = {k for k, x in b.items() if x == v}
        ea = {k for k, x in a.items() if x == v}
        added, removed = ea - eb, eb - ea
        per_via[v] = {"edges_before": len(eb), "edges_after": len(ea),
                      "confirmed_before": len(eb & obs), "confirmed_after": len(ea & obs),
                      "added": len(added), "removed": len(removed),
                      "added_unexercised": len(added - obs),
                      # A confirmed edge leaving this label either moved to another (relabelled: the
                      # same call, found another way) or is gone (counted in `lost`).
                      "removed_confirmed_relabelled": len(removed & obs & set(a)),
                      "removed_confirmed_gone": len((removed & obs) - set(a))}
    relabelled = sorted(f"{x} -> {y}: {b[(x, y)]} -> {a[(x, y)]}" for (x, y) in found_b & found_a if b[(x, y)] != a[(x, y)])
    per_cat = {}
    if labels:
        groups = collections.defaultdict(set)
        for pair in obs:
            groups[labels.get(f"{pair[0]} -> {pair[1]}", "unlabelled")].add(pair)
        for cat, pairs in sorted(groups.items()):
            per_cat[cat] = {"observed": len(pairs), "found_before": len(pairs & found_b),
                            "found_after": len(pairs & found_a)}
    lost = sorted(found_b - found_a)
    return {
        "observed": len(obs),
        "recall_before": round(100 * len(found_b) / len(obs), 1) if obs else None,
        "recall_after": round(100 * len(found_a) / len(obs), 1) if obs else None,
        "gained": len(found_a - found_b),
        "lost": [f"{x} -> {y}" for x, y in lost],
        "relabelled": relabelled,
        "per_via": per_via,
        "per_category": per_cat,
    }


def show(r):
    print(f"recall {r['recall_before']}% -> {r['recall_after']}% of {r['observed']} observed calls "
          f"(+{r['gained']} found, {len(r['lost'])} lost)")
    print(f"\n{'via':22} {'edges':>13} {'confirmed':>13} {'added':>6} {'removed':>8} {'unexercised':>12}")
    for v, s in r["per_via"].items():
        flag = (f"  <- {s['removed_confirmed_gone']} confirmed edge(s) gone" if s["removed_confirmed_gone"] else
                f"  ({s['removed_confirmed_relabelled']} confirmed relabelled)" if s["removed_confirmed_relabelled"] else "")
        print(f"{v:22} {s['edges_before']:>6} -> {s['edges_after']:<5} {s['confirmed_before']:>5} -> "
              f"{s['confirmed_after']:<5} {s['added']:>6} {s['removed']:>8} {s['added_unexercised']:>12}{flag}")
    if r["per_category"]:
        print(f"\n{'category':22} {'observed':>9} {'found':>15}")
        for c, s in r["per_category"].items():
            flag = "  <- REGRESSED" if s["found_after"] < s["found_before"] else ""
            print(f"{c:22} {s['observed']:>9} {s['found_before']:>6} -> {s['found_after']:<6}{flag}")
    if r["relabelled"]:
        print(f"\nrelabelled (found both times, by a different rule): {len(r['relabelled'])}")
        for e in r["relabelled"][:10]:
            print("  " + e)
    if r["lost"]:
        print("\nLOST (found before, missed after):")
        for e in r["lost"]:
            print("  " + e)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--before", required=True)
    ap.add_argument("--after", required=True)
    ap.add_argument("--trace", required=True)
    ap.add_argument("--labels")
    ap.add_argument("--allow-lost", type=int, default=0)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    load = lambda p: json.load(open(p))  # noqa: E731
    r = compare(load(a.before), load(a.after), load(a.trace), load(a.labels) if a.labels else None)
    print(json.dumps(r, indent=1)) if a.json else show(r)
    sys.exit(1 if len(r["lost"]) > a.allow_lost else 0)


if __name__ == "__main__":
    main()
