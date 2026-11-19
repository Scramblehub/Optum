#!/usr/bin/env python3
"""exp/bin-skeletons round 3: seeds, then call-graph propagation.

Rounds 1-2 judged each function alone and found too little across optimisation levels
(docs/notes/2026-11-18-bin-skeletons.md). Round 3 uses the relations between functions:

  1. seeds      confident names: a unique exact-hash match, or a strong skeleton+anchor match
                (combined space, cosine >= 0.85 with a 0.05 margin; round 2's no-mistake setting)
  2. propagate  for each named function, its unnamed callees and callers are matched only against
                the callees and callers its counterpart has in memory. The candidate set is tiny,
                so a much weaker similarity can decide. Repeated until nothing changes.

Pre-registered (set before any test run, docs/notes/2026-11-18-bin-skeletons-r3.md):
  - tune on zlib and cJSON only; test on lz4 and parson (sealed corpus projects), once
  - selection rule: the highest pooled tuning recall with precision >= 98% and false names <= 1%
    (both on functions of the query that aren't in memory and in the negative control)
  - the bar on the sealed test: recall >= 60% at precision >= 98%, false names <= 1%

Per project: the query is a program at level L; memory is the project's other binaries at the
other four levels (held out by program and by level). Negative control: memory from all other
projects.

Usage:
  round3.py --optum OPTUM --corpus DIR --phase tune
  round3.py --optum OPTUM --corpus DIR --phase test --config '{"tau": ..., "delta": ..., "structural": ...}'
"""
import argparse
import collections
import itertools
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import experiment as r2  # noqa: E402  (round 2's features, memory and disassembly cache)

LEVELS = r2.LEVELS
PROJECTS = {  # query program, memory binaries
    "zlib": ("minigzip", ["libz.so", "example"]),
    "cJSON": ("cjson_test", ["libcjson.so"]),
    "lz4": ("lz4_frame", ["liblz4.so", "lz4_file"]),
    "parson": ("parson_tests", ["libparson.so"]),
}
TUNE, TEST = ["zlib", "cJSON"], ["lz4", "parson"]
SEED_TAU, SEED_MARGIN = 0.85, 0.05


class Binary:
    """One binary's functions: features, label (from the answer key, None for runtime code) and
    direct callees in call order."""

    def __init__(self, optum, corpus, cache, project, opt, binary, form="stripped"):
        labs = r2.labels(corpus, project, opt, binary)
        fns = r2.disasm(optum, corpus, cache, project, opt, form, binary)
        self.starts = [int(f["start"], 16) for f in fns]
        self.label = {s: labs.get(s) for s in self.starts}
        self.feat = {s: r2.features(f) for s, f in zip(self.starts, fns)}
        self.callees = {}
        for s, f in zip(self.starts, fns):
            seen = []
            for i in f["insns"]:
                if i.get("to_function"):
                    t = int(i["to_function"], 16)
                    if t != s and t not in seen:
                        seen.append(t)
            self.callees[s] = seen
        self.callers = collections.defaultdict(list)
        for s, cs in self.callees.items():
            for t in cs:
                self.callers[t].append(s)


class GraphMemory:
    """Round 2's similarity memory, plus each label's callee and caller labels in memory."""

    def __init__(self, binaries):
        entries, self.callee_labels, self.caller_labels = [], collections.defaultdict(set), collections.defaultdict(set)
        for b in binaries:
            for s in b.starts:
                lab = b.label[s]
                if lab is None:
                    continue
                entries.append((lab, b.feat[s]))
                for t in b.callees[s]:
                    if b.label.get(t) and b.label[t] != lab:
                        self.callee_labels[lab].add(b.label[t])
                        self.caller_labels[b.label[t]].add(lab)
        self.mem = r2.Memory(entries)
        space = self.mem.spaces["combined"]
        self.space = space
        self.by_label = collections.defaultdict(list)
        for k, (lab, _) in enumerate(space.vectors):
            self.by_label[lab].append(k)

    def restricted(self, f, candidates):
        """Best cosine per candidate label, against that label's memory instances only."""
        v = {s: c * self.space.idf.get(s, 0) for s, c in f["combined"].items() if s in self.space.idf}
        norm = math.sqrt(sum(x * x for x in v.values()))
        out = []
        for lab in candidates:
            best = 0.0
            for k in self.by_label.get(lab, []):
                vec = self.space.vectors[k][1]
                if norm:
                    best = max(best, sum(x / norm * vec.get(s, 0) for s, x in v.items()))
            out.append((lab, best))
        return sorted(out, key=lambda kv: -kv[1])


def identify(q, gm, tau, delta, structural):
    """Names for the query binary's functions: seeds, then propagation."""
    names = {}
    for s in q.starts:
        f = q.feat[s]
        lab = gm.mem.identify("exact", f) or gm.mem.identify("combined", f, SEED_TAU)
        if lab:
            names[s] = lab
    used = collections.Counter(names.values())
    changed = True
    rounds = 0
    while changed and rounds < 20:
        changed, rounds = False, rounds + 1
        for s, lab in list(names.items()):
            for neighbours, cand in ((q.callees[s], gm.callee_labels[lab]), (q.callers[s], gm.caller_labels[lab])):
                open_q = [t for t in neighbours if t not in names and q.feat[t]["n"] >= 4]
                open_l = [c for c in cand if not used[c]]
                if not open_q or not open_l:
                    continue
                for t in open_q:
                    ranked = gm.restricted(q.feat[t], open_l)
                    if not ranked:
                        continue
                    best, score = ranked[0]
                    second = ranked[1][1] if len(ranked) > 1 else 0.0
                    if score >= tau and score - second >= delta:
                        names[t], changed = best, True
                        used[best] += 1
                        open_l = [c for c in open_l if c != best]
                if structural and len([t for t in neighbours if t not in names and q.feat[t]["n"] >= 4]) == 1 and len(open_l) == 1:
                    t = next(t for t in neighbours if t not in names and q.feat[t]["n"] >= 4)
                    names[t], changed = open_l[0], True
                    used[open_l[0]] += 1
    return names


def score(q, gm, names):
    c = collections.Counter()
    for s in q.starts:
        lab = q.label[s]
        if lab is None or q.feat[s]["n"] < 4:
            continue
        known = lab in gm.mem.labels
        c["known" if known else "unknown"] += 1
        if s in names:
            if known:
                c["right" if names[s] == lab else "wrong"] += 1
            else:
                c["false_id"] += 1
    return c


_memories = {}  # memories don't depend on the configuration: build each once


def memory(key, build):
    if key not in _memories:
        _memories[key] = build()
    return _memories[key]


def run(projects, load, config):
    total, negative = collections.Counter(), collections.Counter()
    per = {}
    for p in projects:
        query, mem_bins = PROJECTS[p]
        pc = collections.Counter()
        for L in LEVELS:
            others = [o for o in LEVELS if o != L]
            q = load(p, L, query)
            gm = memory(("main", p, L), lambda: GraphMemory([load(p, o, b) for b in mem_bins for o in others]))
            pc += score(q, gm, identify(q, gm, **config))
            other_projects = [x for x in PROJECTS if x != p and x in projects + [y for y in PROJECTS if y not in TEST]]
            neg = memory(("negative", p, L, tuple(other_projects)), lambda: GraphMemory(
                [load(x, o, b) for x in other_projects for b in [PROJECTS[x][0]] + PROJECTS[x][1] for o in others]))
            negative += score(q, neg, identify(q, neg, **config))
        per[p] = pc
        total += pc
    return total, negative, per


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def report(label, c, neg):
    named = c["right"] + c["wrong"]
    prec, rec = pct(c["right"], named), pct(c["right"], c["known"])
    fid = pct(c["false_id"], c["unknown"])
    nfid = pct(neg["false_id"], neg["unknown"])
    print(f"  {label:42} precision {prec:5.1f}%  recall {rec:5.1f}% (of {c['known']})  "
          f"false names: not-in-memory {fid:4.1f}%  negative control {nfid:4.1f}%")
    return prec, rec, fid, nfid


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--optum", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--phase", choices=["tune", "test"], required=True)
    ap.add_argument("--config", help="test phase: the configuration chosen in the tune phase (JSON)")
    args = ap.parse_args()
    cache = os.path.join(args.corpus, "disasm")
    os.makedirs(cache, exist_ok=True)
    loaded = {}

    def load(p, opt, b):
        if (p, opt, b) not in loaded:
            loaded[(p, opt, b)] = Binary(args.optum, args.corpus, cache, p, opt, b)
        return loaded[(p, opt, b)]

    if args.phase == "tune":
        print("tuning set: zlib, cJSON (the sealed projects are not loaded)")
        base = {"tau": 2.0, "delta": 1.0, "structural": False}  # seeds only: propagation never fires
        c, n, _ = run(TUNE, load, base)
        report("seeds only (no propagation)", c, n)
        best = None
        for tau, delta, structural in itertools.product((0.2, 0.3, 0.4, 0.5, 0.6), (0.05, 0.1, 0.2), (False, True)):
            cfg = {"tau": tau, "delta": delta, "structural": structural}
            c, n, _ = run(TUNE, load, cfg)
            prec, rec, fid, nfid = report(json.dumps(cfg), c, n)
            if prec >= 98 and fid <= 1 and nfid <= 1 and (best is None or rec > best[0]):
                best = (rec, cfg)
        print("\nchosen by the rule:", json.dumps(best[1]) if best else "none meets the rule")
        return
    if not args.config:
        sys.exit("the test phase needs --config, the configuration chosen in the tune phase")
    cfg = json.loads(args.config)
    print(f"held-out test: lz4, parson, run once with {json.dumps(cfg)}")
    c, n, per = run(TEST, load, cfg)
    for p, pc in per.items():
        report(p, pc, collections.Counter())
    prec, rec, fid, nfid = report("sealed test, pooled", c, n)
    base_c, base_n, _ = run(TEST, load, {"tau": 2.0, "delta": 1.0, "structural": False})
    report("same data, seeds only (reference)", base_c, base_n)
    met = prec >= 98 and rec >= 60 and fid <= 1 and nfid <= 1
    print(f"\npre-registered bar (recall >= 60% at precision >= 98%, false names <= 1%): {'MET' if met else 'NOT MET'}")


if __name__ == "__main__":
    main()
