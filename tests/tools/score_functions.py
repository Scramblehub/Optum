#!/usr/bin/env python3
"""Score function discovery on the binary corpus (docs/plans/binary.md, stages 2-3).

The answer key's function starts (tests/tools/build_corpus.py) are compared with starts found by:
  --baseline symbols    the .symtab of the symbols-only form (checks the answer keys: should be 100%)
  --baseline eh_frame   the .eh_frame unwind table of the stripped form: the boundary information
                        that survives stripping, and the bar a binary frontend has to beat
  --graphs DIR          Optum graphs, one per binary at DIR/<project>/<opt>/<form>/<binary>.json; a
                        function's start is its loc.address

Usage:
  score_functions.py --corpus DIR --baseline eh_frame [--form stripped] [--min-recall 0.9]
  score_functions.py --corpus DIR --graphs GDIR --form stripped [--verbose]

Reports precision and recall of function starts per project and level, for all functions and for
source functions only (leaving out GCC's .cold / .part / .constprop pieces). Exits 1 below the
minimums, measured over all binaries.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys


def eh_frame_starts(path):
    out = subprocess.run(["readelf", "--debug-dump=frames", "-W", path], capture_output=True, text=True).stdout
    return {int(m.group(1), 16) for m in re.finditer(r"FDE cie=\w+ pc=([0-9a-f]+)\.\.", out)}


def code_sections(path):
    """Indexes of code sections a function can live in (not .init/.fini/.plt*), as the answer keys use."""
    out = subprocess.run(["readelf", "-S", "-W", path], capture_output=True, text=True).stdout
    idx = set()
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+(\S*X\S*)\s", line)
        if m and not m.group(2).startswith((".init", ".fini", ".plt")):
            idx.add(m.group(1))
    return idx


def symbol_starts(path):
    out = subprocess.run(["readelf", "--syms", "-W", path], capture_output=True, text=True).stdout
    code = code_sections(path)
    starts, table = set(), ""
    for line in out.splitlines():
        t = re.match(r"Symbol table '([^']+)'", line)
        if t:
            table = t.group(1)
            continue
        m = re.match(r"^\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+FUNC\s+\w+\s+\w+\s+(\S+)", line)
        if m and table == ".symtab" and m.group(3) != "UND" and (int(m.group(2)) > 0 or m.group(3) in code):
            starts.add(int(m.group(1), 16))
    return starts


def graph_starts(path):
    g = json.load(open(path))
    starts = set()
    for n in g["nodes"]:
        addr = n.get("loc", {}).get("address")
        if n["kind"] == "function" and addr:
            starts.add(int(addr, 16))
    return starts


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--corpus", required=True)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--baseline", choices=["symbols", "eh_frame"])
    src.add_argument("--graphs")
    ap.add_argument("--form", choices=["debug", "symbols", "stripped", "bare"], default=None,
                    help="which form to read (default: symbols for --baseline symbols, else stripped)")
    ap.add_argument("--min-recall", type=float, default=0.0)
    ap.add_argument("--min-precision", type=float, default=0.0)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    form = args.form or ("symbols" if args.baseline == "symbols" else "stripped")

    manifest = json.load(open(os.path.join(args.corpus, "manifest.json")))["binaries"]
    rows = collections.defaultdict(lambda: collections.Counter())
    total = collections.Counter()
    for e in manifest:
        if e.get("stub"):
            continue  # stub libraries (tests/corpus/stubs) are scaffolding for other binaries
        t = json.load(open(os.path.join(args.corpus, e["truth"])))
        binary = os.path.join(args.corpus, t["paths"][form])
        if args.baseline == "eh_frame":
            found = eh_frame_starts(binary)
        elif args.baseline == "symbols":
            found = symbol_starts(binary)
        else:
            gpath = os.path.join(args.graphs, e["project"], e["opt"].lstrip("-"), form, e["binary"] + ".json")
            found = graph_starts(gpath) if os.path.exists(gpath) else set()
        truth_all = {int(f["address"], 16) for f in t["functions"]}
        truth_src = {int(f["address"], 16) for f in t["functions"] if "fragment" not in f}
        c = collections.Counter(found=len(found), truth=len(truth_all), hit=len(found & truth_all),
                                truth_src=len(truth_src), hit_src=len(found & truth_src))
        rows[(e["project"], e["opt"])] += c
        total += c
        if args.verbose:
            missed = sorted(truth_all - found)[:5]
            names = {int(f["address"], 16): f["name"] for f in t["functions"]}
            print(f"  {e['project']}/{e['opt']}/{e['binary']}: missed {[names[a] for a in missed]}")

    pct = lambda a, b: 100.0 * a / b if b else 100.0
    label = args.baseline or "optum"
    print(f"function starts ({label}, {form} form)")
    print(f"{'project':8} {'opt':4} {'truth':>6} {'found':>6} {'precision':>10} {'recall':>8} {'recall (source fns)':>20}")
    for (proj, opt), c in sorted(rows.items()):
        print(f"{proj:8} {opt:4} {c['truth']:6} {c['found']:6} {pct(c['hit'], c['found']):9.1f}% "
              f"{pct(c['hit'], c['truth']):7.1f}% {pct(c['hit_src'], c['truth_src']):19.1f}%")
    precision, recall = pct(total["hit"], total["found"]) / 100, pct(total["hit"], total["truth"]) / 100
    print(f"{'all':13} {total['truth']:6} {total['found']:6} {100*precision:9.1f}% {100*recall:7.1f}% "
          f"{pct(total['hit_src'], total['truth_src']):19.1f}%")
    sys.exit(0 if precision >= args.min_precision and recall >= args.min_recall else 1)


if __name__ == "__main__":
    main()
