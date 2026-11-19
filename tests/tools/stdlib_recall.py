"""Prototype for feat/trace-verify: recall of the Python frontend on real code.

Runs a set of standard-library workloads under sys.setprofile, keeps only calls written in the
source (not class bodies, operators or property access), and reports how many of them the static
graph contains. Usage:

    optum graph "$(python3 -c 'import sysconfig; print(sysconfig.get_paths()["stdlib"])')" -q -o stdlib.json
    python3 -I tests/tools/stdlib_recall.py stdlib.json

Known artifact: a constructor called from inside another __init__ is recorded as X.__init__,
not X, so it shows as missed. See docs/notes/2026-11-18-python-frontend-eval.md.
"""
import json, os, re, sys, collections, inspect, linecache, sysconfig
ROOT = sysconfig.get_paths()["stdlib"]
graph = json.load(open(sys.argv[1]))
static = {(e["from"], e["to"]) for e in graph["edges"] if e["kind"] == "calls"}
nodes = {n["id"] for n in graph["nodes"]}
def node_id(code):
    f = code.co_filename
    if f.startswith("<") or not f.startswith(ROOT + "/"): return None
    rel = os.path.relpath(f, ROOT)
    if "site-packages" in rel or "dist-packages" in rel: return None
    mod = rel[:-3].replace("/", ".")
    if mod.endswith(".__init__"): mod = mod[:-9]
    q = code.co_qualname
    if q == "<module>": return "py:" + mod
    q = re.sub(r"\.<locals>", "", q); q = re.sub(r"(\.)?<[a-z]+>$", "", q)
    return "py:" + mod + ("." + q if q else "")
seen = collections.Counter()
def prof(frame, event, arg):
    if event != "call": return
    callee = node_id(frame.f_code); back = frame.f_back
    if not callee or back is None: return
    caller = node_id(back.f_code)
    if not caller or caller == callee and frame.f_code.co_name.startswith("<"): return
    if frame.f_code.co_name.startswith("<") : return  # genexpr/lambda frames
    if not frame.f_code.co_flags & inspect.CO_OPTIMIZED: return  # class bodies
    # Only calls written in the source: the caller's line must name the callee followed by "(".
    short = frame.f_code.co_name
    if short == "__init__": short = callee.rsplit(".", 2)[-2]
    line = linecache.getline(back.f_code.co_filename, back.f_lineno)
    if not re.search(r"\b" + re.escape(short) + r"\s*\(", line) and not line.lstrip().startswith("@"): return
    if callee.endswith(".__init__") and not back.f_code.co_name == "__init__":
        callee = callee[:-9]
    seen[(caller, callee)] += 1
import argparse, difflib, textwrap, string, fractions, statistics, csv, io, configparser, shlex, email.parser, email.policy, pprint, ipaddress, calendar, urllib.parse, decimal, heapq, dataclasses
sys.setprofile(prof)
json.loads(json.dumps({"a": [1, 2, {"b": None}]}, indent=2))
p = argparse.ArgumentParser(); p.add_argument("--x", type=int, default=3); p.add_argument("files", nargs="*"); p.parse_args(["--x", "4", "a", "b"])
list(difflib.unified_diff(["a\n", "b\n", "c\n"], ["a\n", "B\n", "c\n"])); difflib.SequenceMatcher(None, "abcd", "bcde").ratio()
textwrap.fill("hello world " * 20, width=30); textwrap.dedent("   a\n   b")
string.Template("$x and $y").substitute(x=1, y=2)
fractions.Fraction(3, 4) + fractions.Fraction(1, 6); statistics.median([3, 1, 2]); statistics.stdev([1.0, 2.0, 4.0])
list(csv.reader(io.StringIO("a,b\n1,2\n"))); w = csv.DictWriter(io.StringIO(), ["a"]); w.writeheader(); w.writerow({"a": 1})
c = configparser.ConfigParser(); c.read_string("[s]\nk = v\n"); c.get("s", "k")
shlex.split("a 'b c' d"); email.parser.Parser(policy=email.policy.default).parsestr("Subject: hi\n\nbody")
pprint.pformat({"k": list(range(30))}); ipaddress.ip_network("10.0.0.0/24").num_addresses; calendar.month(2026, 11)
urllib.parse.urlparse("https://a.example.com/x?y=1"); urllib.parse.urlencode({"a": 1, "b": "c d"})
@dataclasses.dataclass
class P:
    x: int = 0
P(1)
sys.setprofile(None)
observed = set(seen)
hit = observed & static
missing = observed - static
both_nodes = {e for e in observed if e[0] in nodes and e[1] in nodes}
print(f"observed call edges: {len(observed)} | found by static graph: {len(hit)} ({100*len(hit)/len(observed):.1f}%) | edges whose both ends are graph nodes: {len(both_nodes)} ({100*len(hit)/max(1,len(both_nodes)):.1f}% of those found)")
cats = collections.Counter()
for a, b in missing:
    cats[b.rsplit(".", 1)[-1] if b.rsplit(".",1)[-1].startswith("__") else ("callee-not-a-node" if b not in nodes else "other")] += 1
print("missing by kind:", cats.most_common(8))
import random; random.seed(3)
print("sample of missing 'other':")
for e in random.sample(sorted(m for m in missing if m[1] in nodes and not m[1].rsplit('.',1)[-1].startswith('__')), 15): print("  ", e[0], "->", e[1])
