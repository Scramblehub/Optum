#!/usr/bin/env python3
"""exp/bin-skeletons: name functions in stripped binaries from memory of other binaries.

Methods (docs/plans/binary.md, stage 5; CGL structure memory, docs/notes/2026-11-18-bin-skeletons.md):
  exact     FLIRT-style: a function's bytes with address fields masked, hashed
  shape     counts: instructions, calls, conditional jumps, returns (bucketed)
  skeleton  CGL structure memory: windows of 3-5 normalised instructions with typed holes
            (registers, immediates and memory operands abstracted; imported functions' names kept),
            weighted by rarity across the memory (IDF); identified by cosine vote, abstaining below
            a threshold or without a margin over the next different name
  anchors   contents instead of structure: string literals referenced, large constants, imported
            functions called (round 2: they survive a change of optimisation level)
  combined  skeletons and anchors in one vector

Protocol (round 2, set before running it): tune on zlib (minigzip queries), then evaluate the
chosen configuration once on cJSON (cjson_test queries), the held-out test.

Settings (held out by program and/or by optimisation level):
  same_level      memory: libz.so + example at level L      query: minigzip at L
  cross_level     memory: all zlib binaries at levels != L  query: minigzip at L
  cross_both      memory: libz.so + example at levels != L  query: minigzip at L
  self_vs_xfer    query minigzip at L; memory = minigzip at other levels (self), or the other
                  programs at other levels (xfer), or both  (the CGL round 5b question)
  cjson_cross     memory: libcjson.so at levels != L        query: cjson_test at L
  negative        memory: all cJSON                         query: minigzip (nothing is in memory)

Usage: experiment.py --optum OPTUM --corpus DIR [--json results.json]
"""
import argparse
import collections
import hashlib
import json
import math
import os
import subprocess
import sys

LEVELS = ["-O0", "-O1", "-O2", "-O3", "-Os"]
THRESHOLD, MARGIN = 0.85, 0.05  # fixed before running: name only clear matches


def disasm(optum, corpus, cache, project, opt, form, binary):
    path = os.path.join(cache, f"{project}_{opt}_{form}_{binary}.json")
    if not os.path.exists(path):
        src = os.path.join(corpus, project, opt.lstrip("-"), form, binary)
        r = subprocess.run([optum, "disasm", src, "-o", path], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr)
    return json.load(open(path))["functions"]


def labels(corpus, project, opt, binary):
    """Address -> source function (fragments map to their parent); runtime code is left out."""
    t = json.load(open(os.path.join(corpus, project, opt.lstrip("-"), "truth", binary + ".json")))
    return {int(f["address"], 16): f["source_id"] for f in t["functions"] if f["origin"] == "project"}


# ---------------------------------------------------------------- features

def reg_class(r):
    if r in ("rsp", "esp"):
        return "SP"
    if r in ("rbp", "ebp"):
        return "BP"
    if r == "rip":
        return "IP"
    if r.startswith(("xmm", "ymm", "zmm")):
        return "V"
    return "R"


def token(i):
    """One instruction as a skeleton token: the structure kept, the contents as typed holes."""
    if i.get("to_import"):
        return f"{i['m']}:{i['to_import']}"  # library names survive stripping: keep them
    if i.get("to_function"):
        return f"{i['m']}:F"
    if "to_offset" in i:
        return f"{i['m']}:L"
    ops = []
    for o in i["o"]:
        if o["k"] == "reg":
            ops.append(reg_class(o["r"]) + str(o["s"]))
        elif o["k"] == "imm":
            ops.append("I" if abs(o["v"]) > 8 else f"I{o['v']}")  # tiny constants carry meaning
        else:
            ops.append(f"M{o['s']}[{reg_class(o['base']) if o['base'] else ''}{'+X' if o['index'] else ''}]")
    return i["m"] + " " + ",".join(ops)


def anchors_of(fn):
    a = collections.Counter()
    for i in fn["insns"]:
        if "str" in i:
            a["STR " + i["str"]] += 1
        if i.get("to_import"):
            a["CALLS " + i["to_import"]] += 1
        for o in i["o"]:
            if o["k"] == "imm" and abs(o["v"]) >= 0x1000 and not i.get("to_function") and "to_offset" not in i:
                a[f"CONST {o['v'] & 0xffffffff:x}"] += 1  # large constants: magic numbers, masks
    return a


def features(fn):
    toks = [token(i) for i in fn["insns"] if i["m"] not in ("nop", "endbr64", "int3")]
    exact = hashlib.sha1("".join(i["b"] for i in fn["insns"]).encode()).hexdigest()
    m = [i["m"] for i in fn["insns"]]
    bucket = lambda n: n if n < 8 else 8 + int(math.log2(n))
    shape = (bucket(len(m)), sum(x == "call" for x in m), bucket(sum(x.startswith("j") and x != "jmp" for x in m)),
             sum(x == "ret" for x in m))
    skel = collections.Counter()
    for k in (3, 4, 5):
        for a in range(len(toks) - k + 1):
            skel["|".join(toks[a:a + k])] += 1
    for t in toks:
        if ":" in t and not t.endswith((":F", ":L")):
            skel["IMPORT " + t] += 3  # calls to named library functions: strong evidence
    anchors = anchors_of(fn)
    combined = collections.Counter(skel)
    for k, v in anchors.items():
        combined[k] += 4 * v
    return {"exact": exact, "shape": shape, "skeleton": skel, "anchors": anchors, "combined": combined, "n": len(toks)}


# ---------------------------------------------------------------- memory and identification

class Space:
    """One kind of feature (skeletons, anchors, both): IDF-weighted, normalised vectors."""

    def __init__(self, entries, kind):
        df = collections.Counter()
        for _, f in entries:
            df.update(f[kind].keys())
        n = max(1, len(entries))
        self.kind = kind
        self.idf = {s: math.log(n / c) + 1 for s, c in df.items()}
        self.vectors = []
        self.index = collections.defaultdict(list)  # feature -> entry numbers
        for k, (lab, f) in enumerate(entries):
            v = {s: c * self.idf[s] for s, c in f[kind].items()}
            norm = math.sqrt(sum(x * x for x in v.values())) or 1
            self.vectors.append((lab, {s: x / norm for s, x in v.items()}))
            for s in v:
                self.index[s].append(k)

    def ranked(self, f):
        v = {s: c * self.idf.get(s, 0) for s, c in f[self.kind].items() if s in self.idf}
        norm = math.sqrt(sum(x * x for x in v.values()))
        if not norm:
            return []
        dots = collections.defaultdict(float)
        for s, x in v.items():
            for k in self.index[s]:
                dots[k] += x / norm * self.vectors[k][1][s]
        scores = collections.defaultdict(float)
        for k, d in dots.items():
            lab = self.vectors[k][0]
            scores[lab] = max(scores[lab], d)
        return sorted(scores.items(), key=lambda kv: -kv[1])


class Memory:
    def __init__(self, entries):
        """entries: (label, features)."""
        self.entries = [(lab, f) for lab, f in entries if f["n"] >= 4]
        self.exact = collections.defaultdict(set)
        self.shape = collections.defaultdict(set)
        for lab, f in self.entries:
            self.exact[f["exact"]].add(lab)
            self.shape[f["shape"]].add(lab)
        self.spaces = {k: Space(self.entries, k) for k in ("skeleton", "anchors", "combined")}
        self.labels = {lab for lab, _ in self.entries}

    def identify(self, method, f, threshold=THRESHOLD):
        if f["n"] < 4:
            return None
        if method == "exact":
            labs = self.exact.get(f["exact"], set())
            return next(iter(labs)) if len(labs) == 1 else None
        if method == "shape":
            labs = self.shape.get(f["shape"], set())
            return next(iter(labs)) if len(labs) == 1 else None
        # cosine against every memory function sharing a feature, best per label
        ranked = self.spaces[method].ranked(f)
        if not ranked or ranked[0][1] < threshold:
            return None
        if len(ranked) > 1 and ranked[0][1] - ranked[1][1] < MARGIN:
            return None
        return ranked[0][0]


METHODS = ("exact", "shape", "skeleton", "anchors", "combined")


def evaluate(memory, queries, methods=METHODS, threshold=THRESHOLD):
    """queries: (label, features). Counts: named correctly, named wrongly (when the function is
    in memory), and named at all when it isn't (false identifications)."""
    out = {}
    for method in methods:
        c = collections.Counter()
        for lab, f in queries:
            if f["n"] < 4:
                continue
            known = lab in memory.labels
            guess = memory.identify(method, f, threshold)
            c["known"] += known
            c["unknown"] += not known
            if guess is None:
                continue
            if known:
                c["right" if guess == lab else "wrong"] += 1
            else:
                c["false_id"] += 1
        out[method] = c
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--optum", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--form", default="stripped")
    ap.add_argument("--json")
    args = ap.parse_args()
    cache = os.path.join(args.corpus, "disasm")
    os.makedirs(cache, exist_ok=True)

    def load(project, opt, binary):
        labs = labels(args.corpus, project, opt, binary)
        return [(labs[int(fn["start"], 16)], features(fn)) for fn in
                disasm(args.optum, args.corpus, cache, project, opt, args.form, binary) if int(fn["start"], 16) in labs]

    zlib = {(b, o): load("zlib", o, b) for b in ("libz.so", "example", "minigzip") for o in LEVELS}
    cj = {(b, o): load("cJSON", o, b) for b in ("libcjson.so", "cjson_test") for o in LEVELS}

    settings = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    for L in LEVELS:
        others = [o for o in LEVELS if o != L]
        q = zlib[("minigzip", L)]
        runs = {
            "same_level": zlib[("libz.so", L)] + zlib[("example", L)],
            "cross_level": [e for b in ("libz.so", "example", "minigzip") for o in others for e in zlib[(b, o)]],
            "cross_both": [e for b in ("libz.so", "example") for o in others for e in zlib[(b, o)]],
            "self_vs_xfer:self": [e for o in others for e in zlib[("minigzip", o)]],
            "self_vs_xfer:xfer": [e for b in ("libz.so", "example") for o in others for e in zlib[(b, o)]],
            "self_vs_xfer:both": [e for b in ("libz.so", "example", "minigzip") for o in others for e in zlib[(b, o)]],
            "negative": [e for v in cj.values() for e in v],
        }
        for name, mem in runs.items():
            for method, c in evaluate(Memory(mem), q).items():
                settings[name][method] += c
        for method, c in evaluate(Memory([e for o in others for e in cj[("libcjson.so", o)]]), cj[("cjson_test", L)]).items():
            settings["cjson_cross"][method] += c

    pct = lambda a, b: f"{100 * a / b:5.1f}%" if b else "   - "
    print(f"form: {args.form}; threshold {THRESHOLD}, margin {MARGIN}; summed over the five levels")
    print(f"{'setting':20} {'method':9} {'in memory':>9} {'precision':>9} {'recall':>7} {'not in memory':>13} {'false IDs':>9}")
    results = {}
    for name, per in settings.items():
        for method, c in per.items():
            named = c["right"] + c["wrong"]
            print(f"{name:20} {method:9} {c['known']:9} {pct(c['right'], named):>9} {pct(c['right'], c['known']):>7} "
                  f"{c['unknown']:13} {pct(c['false_id'], c['unknown']):>9}")
            results.setdefault(name, {})[method] = dict(c)
    # Round 2: precision and recall as the threshold moves, on the tuning set (zlib cross_both)
    # and, only to be read once a threshold is chosen, the held-out test (cJSON).
    print("\nthreshold sweep (precision / recall / false IDs on the negative control)")
    for kind in ("skeleton", "anchors", "combined"):
        for tau in (0.5, 0.6, 0.7, 0.8, 0.9):
            tune, neg = collections.Counter(), collections.Counter()
            for L in LEVELS:
                others = [o for o in LEVELS if o != L]
                mem = Memory([e for b in ("libz.so", "example") for o in others for e in zlib[(b, o)]])
                tune += evaluate(mem, zlib[("minigzip", L)], (kind,), tau)[kind]
                neg += evaluate(Memory([e for v in cj.values() for e in v]), zlib[("minigzip", L)], (kind,), tau)[kind]
            named = tune["right"] + tune["wrong"]
            results.setdefault("sweep", {})[f"{kind}@{tau}"] = {"tune": dict(tune), "negative": dict(neg)}
            print(f"  {kind:9} tau {tau}: zlib {pct(tune['right'], named)} / {pct(tune['right'], tune['known'])}"
                  f"   negative false IDs {pct(neg['false_id'], neg['unknown'])}")
    # The held-out test, run once with the configuration chosen on zlib by the rule set beforehand:
    # highest recall with tuning precision >= 99% and negative false IDs <= 1% -> combined @ 0.6.
    CHOSEN = ("combined", 0.6)
    print(f"\nheld-out test, cJSON (memory libcjson.so at other levels, query cjson_test), chosen: {CHOSEN[0]} @ {CHOSEN[1]}")
    for method, tau in (CHOSEN, ("skeleton", 0.6), ("exact", THRESHOLD)):
        test, neg = collections.Counter(), collections.Counter()
        for L in LEVELS:
            others = [o for o in LEVELS if o != L]
            test += evaluate(Memory([e for o in others for e in cj[("libcjson.so", o)]]), cj[("cjson_test", L)], (method,), tau)[method]
            neg += evaluate(Memory([e for v in zlib.values() for e in v]), cj[("cjson_test", L)], (method,), tau)[method]
        named = test["right"] + test["wrong"]
        results.setdefault("held_out", {})[f"{method}@{tau}"] = {"test": dict(test), "negative": dict(neg)}
        print(f"  {method:9} @ {tau}: precision {pct(test['right'], named)}  recall {pct(test['right'], test['known'])}"
              f"  (of {test['known']})   negative control (zlib memory) false IDs {pct(neg['false_id'], neg['unknown'])}")
    if args.json:
        json.dump(results, open(args.json, "w"), indent=1)


if __name__ == "__main__":
    main()
