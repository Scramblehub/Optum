#!/usr/bin/env python3
"""Score Optum's binary frontend on the corpus (docs/plans/binary.md, stage 3).

Runs `optum graph` on every corpus binary in each form, then compares with the answer keys:
  - function starts: precision and recall (all functions, and source functions only);
  - calls: Optum's call edges between the binary's own functions, mapped to source ids through the
    answer key's addresses (GCC's .cold / .part pieces map to their parent), against the calls the
    compiler emitted at that level. Calls to imported functions and through pointers aren't scored;
  - source ids (debug form): with debug info, every project function's node id must be its source
    id from the answer key (c:<file>#<name>; GCC's split-off pieces keep their suffix, .cold...);
  - reach (binaries whose answer key names a source graph): the APIs and tables reached (reads /
    writes / sends_to to http: and sql: nodes) must equal the hand-checked source graph's, as
    (node, kind) pairs; at -O0, where functions aren't inlined, also which function reaches each.

Usage:
  score_binary.py --optum build/release/Core/optum --corpus build/corpus [--forms bare,stripped]
                  [--min-func-recall F --min-func-precision F --min-call-recall F --min-call-precision F]
                  [--graphs DIR] [--verbose]
The minimums apply to every form given, summed over all binaries.
"""
import argparse
import collections
import json
import os
import subprocess
import sys


def pct(a, b):
    return 100.0 * a / b if b else 100.0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--optum", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--forms", default="debug,symbols,stripped,bare")
    ap.add_argument("--graphs", help="keep the graphs here (default: <corpus>/graphs)")
    ap.add_argument("--min-func-recall", type=float, default=0)
    ap.add_argument("--min-func-precision", type=float, default=0)
    ap.add_argument("--min-call-recall", type=float, default=0)
    ap.add_argument("--min-call-precision", type=float, default=0)
    ap.add_argument("--require-reach", action="store_true", help="fail unless every reach check matches")
    ap.add_argument("--min-source-ids", type=float, default=0, help="debug form: share of project functions with the right source id")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    corpus = os.path.abspath(args.corpus)
    graphs = os.path.abspath(args.graphs or os.path.join(corpus, "graphs"))
    manifest = json.load(open(os.path.join(corpus, "manifest.json")))["binaries"]

    failed = False
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    reach_rows = []
    sid_total, sid_hit, sid_wrong = 0, 0, []
    print(f"{'form':9} {'opt':4} {'functions':>9} {'precision':>10} {'recall':>8} {'calls':>6} {'precision':>10} {'recall':>8}")
    for form in args.forms.split(","):
        per_opt = collections.defaultdict(collections.Counter)
        for e in manifest:
            t = json.load(open(os.path.join(corpus, e["truth"])))
            binary = os.path.join(corpus, t["paths"][form])
            out = os.path.join(graphs, e["project"], e["opt"].lstrip("-"), form, e["binary"] + ".json")
            os.makedirs(os.path.dirname(out), exist_ok=True)
            r = subprocess.run([args.optum, "graph", binary, "-q", "--indent", "-1", "-o", out], capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"optum failed on {binary}:\n{r.stderr}")
            g = json.load(open(out))
            addr_of = {n["id"]: int(n["loc"]["address"], 16) for n in g["nodes"]
                       if n["kind"] == "function" and n.get("loc", {}).get("address")}
            found = set(addr_of.values())
            truth_addr = {int(f["address"], 16): f for f in t["functions"]}
            c = per_opt[e["opt"]]
            c["f_truth"] += len(truth_addr)
            c["f_found"] += len(found)
            c["f_hit"] += len(found & set(truth_addr))
            # Calls, as source ids.
            sid = {a: f["source_id"] for a, f in truth_addr.items() if f["source_id"]}
            ours = set()
            for edge in g["edges"]:
                if edge["kind"] != "calls" or edge["from"] not in addr_of or edge["to"] not in addr_of:
                    continue
                a, b = sid.get(addr_of[edge["from"]]), sid.get(addr_of[edge["to"]])
                if a and b and a != b:
                    ours.add((a, b))
            truth_calls = {(x["from"], x["to"]) for x in t["calls"]}
            c["c_truth"] += len(truth_calls)
            c["c_found"] += len(ours)
            c["c_hit"] += len(ours & truth_calls)
            if form == "debug":
                ids_at = {int(n["loc"]["address"], 16): n["id"] for n in g["nodes"]
                          if n["kind"] == "function" and n.get("loc", {}).get("address")}
                # Per address: identical-code folding can give one body several source names
                # (sqlite3SelectDelete and sqlite3SelectDeleteGeneric); any of them is right.
                acceptable = collections.defaultdict(set)
                for f in t["functions"]:
                    if f["origin"] != "project" or f.get("fragment") == "localalias":
                        continue  # a .localalias is a second name at the same address, not code
                    want = f["source_id"] if "fragment" not in f else f["source_id"].rsplit("#", 1)[0] + "#" + f["name"]
                    acceptable[int(f["address"], 16)].add(want)
                for a, wants in acceptable.items():
                    got = ids_at.get(a)
                    sid_total += 1
                    if got in wants:
                        sid_hit += 1
                    elif len(sid_wrong) < 5:
                        sid_wrong.append((e["project"], e["opt"], e["binary"], sorted(wants), got))
            if "source_graph" in t:
                src = json.load(open(os.path.join(repo, t["source_graph"])))
                outward = ("reads", "writes", "sends_to")
                want = {(x["to"], x["kind"]) for x in src["edges"] if x["kind"] in outward and x["to"].startswith(("http:", "sql:"))}
                got = {(x["to"], x["kind"]) for x in g["edges"] if x["kind"] in outward and x.get("frontend") == "binary"}
                ok = want == got
                if e["opt"] == "-O0":  # no inlining: the function reaching each must match too
                    want_f = {(x["from"], x["to"], x["kind"]) for x in src["edges"] if (x["to"], x["kind"]) in want}
                    got_f = {(sid.get(addr_of.get(x["from"])), x["to"], x["kind"]) for x in g["edges"] if (x["to"], x["kind"]) in got}
                    ok = ok and want_f == got_f
                reach_rows.append((form, e["opt"], e["binary"], ok, sorted(want - got), sorted(got - want)))
            if args.verbose:
                missed = sorted(truth_calls - ours)[:3]
                extra = sorted(ours - truth_calls)[:3]
                lost = sorted(truth_addr[a]["name"] for a in set(truth_addr) - found)[:4]
                print(f"   {form}/{e['project']}/{e['opt']}/{e['binary']}: functions missed {lost}; calls missed {missed}; extra {extra}")
        total = collections.Counter()
        for opt in sorted(per_opt, key=lambda o: ["-O0", "-O1", "-O2", "-O3", "-Os"].index(o)):
            c = per_opt[opt]
            total += c
            print(f"{form:9} {opt:4} {c['f_truth']:9} {pct(c['f_hit'], c['f_found']):9.1f}% {pct(c['f_hit'], c['f_truth']):7.1f}% "
                  f"{c['c_truth']:6} {pct(c['c_hit'], c['c_found']):9.1f}% {pct(c['c_hit'], c['c_truth']):7.1f}%")
        fp, fr = pct(total["f_hit"], total["f_found"]), pct(total["f_hit"], total["f_truth"])
        cp, cr = pct(total["c_hit"], total["c_found"]), pct(total["c_hit"], total["c_truth"])
        print(f"{form:9} {'all':4} {total['f_truth']:9} {fp:9.1f}% {fr:7.1f}% {total['c_truth']:6} {cp:9.1f}% {cr:7.1f}%")
        if fp < 100 * args.min_func_precision or fr < 100 * args.min_func_recall or \
           cp < 100 * args.min_call_precision or cr < 100 * args.min_call_recall:
            failed = True
    if sid_total:
        print(f"source ids (debug form): {sid_hit}/{sid_total} project function addresses ({pct(sid_hit, sid_total):.1f}%)")
        for w in sid_wrong:
            print(f"   {w[0]}/{w[1]}/{w[2]}: want {w[3]}, got {w[4]}")
        if sid_hit < args.min_source_ids * sid_total:
            failed = True
    if reach_rows:
        bad = [r for r in reach_rows if not r[3]]
        print(f"reach (APIs and tables vs the source graph): {len(reach_rows) - len(bad)}/{len(reach_rows)} binaries match")
        for form, opt, name, _, missing, extra in bad:
            print(f"   {form}/{opt}/{name}: missing {missing}; extra {extra}")
        if bad and args.require_reach:
            failed = True
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
