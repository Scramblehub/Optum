#!/usr/bin/env python3
"""A debug build's graph merges with its source graph node for node (decision 0005).

Graphs a source directory with Optum's source frontends and an unoptimised (-O0, nothing inlined)
binary built from it with debug info, then requires:
  - every binary function with a source id (c:...) to be a function of the source graph;
  - every call between such functions to be a call of the source graph;
  - at least --min-functions of them, so a broken debug-info reader can't pass by finding none.

Usage: check_merge.py --optum OPTUM --source DIR --binary FILE [--min-functions N]
"""
import argparse
import json
import subprocess
import sys


def graph(optum, path):
    r = subprocess.run([optum, "graph", path, "-q", "--indent", "-1"], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"optum graph {path} failed:\n{r.stderr}")
    return json.loads(r.stdout)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--optum", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--min-functions", type=int, default=1)
    args = ap.parse_args()
    src, binary = graph(args.optum, args.source), graph(args.optum, args.binary)
    src_funcs = {n["id"] for n in src["nodes"] if n["kind"] == "function"}
    src_calls = {(e["from"], e["to"]) for e in src["edges"] if e["kind"] == "calls"}
    bin_funcs = {n["id"] for n in binary["nodes"] if n["kind"] == "function" and n["id"].startswith("c:")}
    bin_calls = {(e["from"], e["to"]) for e in binary["edges"]
                 if e["kind"] == "calls" and e["from"] in bin_funcs and e["to"] in bin_funcs}
    bad = sorted(bin_funcs - src_funcs) + [f"{a} -> {b}" for a, b in sorted(bin_calls - src_calls)]
    print(f"binary functions with source ids: {len(bin_funcs)} ({len(bin_funcs & src_funcs)} in the source graph); "
          f"calls between them: {len(bin_calls)} ({len(bin_calls & src_calls)} in the source graph)")
    for b in bad:
        print("  not in the source graph:", b)
    sys.exit(1 if bad or len(bin_funcs) < args.min_functions else 0)


if __name__ == "__main__":
    main()
