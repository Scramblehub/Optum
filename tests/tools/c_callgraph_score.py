"""Score Optum's C call graph against GCC's own: -fcallgraph-info writes every direct call the
compiler emits (after macro expansion, for one build configuration) to a .ci file per source file.

    for f in src/*.c; do gcc -O0 -w -fcallgraph-info -c "$f" -o ci/$(basename "$f" .c).o -dumpdir ci/; done
    optum graph <project> -q -o graph.json
    python3 -I tests/tools/c_callgraph_score.py graph.json ci/

Only callers in the compiled files are compared. Optum's graph also keeps calls inside #if blocks
for other configurations (attrs.condition) and calls by function pointer (inferred), so
"unconditional exact" precision is the like-for-like number. Results: docs/notes/2026-11-18-c-frontend-eval.md.
"""
import collections, glob, json, os, random, re, sys
g = json.load(open(sys.argv[1]))
nodes = {n["id"]: n for n in g["nodes"]}
by_file_name = {}
globals_ = collections.defaultdict(list)
for i, n in nodes.items():
    if n["kind"] == "function" and i.startswith("c:"):
        f, name = i[2:].split("#", 1)
        by_file_name[(f, name)] = i
        if not n.get("attrs", {}).get("static"): globals_[name].append(i)
def resolve(title, tu):
    if ":" in title:  # static: "file.c:name"
        f, name = title.split(":", 1)
    else:
        f, name = tu, title
    if (f, name) in by_file_name: return by_file_name[(f, name)]
    if ":" in title:  # a static from a header, compiled into this TU
        hs = [k for k in by_file_name if k[1] == name and k[0].endswith(".h")]
        return by_file_name[hs[0]] if len(hs) == 1 else None
    defs = [i for i in globals_.get(name, []) if i.split("#")[0].endswith(".c")]
    if len(defs) == 1 and defs[0][2:].split("#")[0] in tus: return defs[0]
    return None  # outside the project (libc etc.)
tus = set()
for ci in glob.glob(os.path.join(sys.argv[2], "*.ci")):
    tus.add(re.search(r'graph: \{ title: "([^"]+)"', open(ci).read()).group(1))
truth = set(); indirect_sites = 0
for ci in glob.glob(os.path.join(sys.argv[2], "*.ci")):
    txt = open(ci).read()
    tu = re.search(r'graph: \{ title: "([^"]+)"', txt).group(1)
    for src, dst in re.findall(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"', txt):
        if dst == "__indirect_call": indirect_sites += 1; continue
        a, b = resolve(src, tu), resolve(dst, tu)
        if a and b: truth.add((a, b))
ours = collections.defaultdict(set)
for e in g["edges"]:
    # Only callers in the files the compiler was given, so both sides cover the same code.
    if e["kind"] == "calls" and e["from"][2:].split("#")[0] in tus:
        ours[e.get("confidence", "exact")].add((e["from"], e["to"]))
exact = ours["exact"]; allc = set().union(*ours.values())
cond_edges = {(e["from"], e["to"]) for e in g["edges"] if e["kind"] == "calls" and e.get("attrs", {}).get("condition")}
uncond = exact - cond_edges
hit = truth & exact
print(f"compiler direct calls inside the project: {len(truth)}  (+{indirect_sites} indirect call sites)")
print(f"optum exact call edges: {len(exact)} | precision {100*len(hit)/max(1,len(exact)):.1f}% | recall {100*len(hit)/max(1,len(truth)):.1f}%")
print(f"optum inferred (function pointer) edges: {len(ours['inferred'])}")
print(f"  unconditional exact edges (not inside any #if): {len(uncond)} | precision {100*len(uncond & truth)/max(1,len(uncond)):.1f}%")
random.seed(1)
miss = sorted(truth - exact); extra = sorted(exact - truth)
for a, b in random.sample(miss, min(12, len(miss))): print("  MISS ", a, "->", b)
for a, b in random.sample(sorted(uncond - truth), min(8, len(uncond - truth))): print("  EXTRA", a, "->", b)
