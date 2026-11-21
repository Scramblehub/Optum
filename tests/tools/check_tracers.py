#!/usr/bin/env python3
"""Edge cases of the tracers (Core/tracers/) that the verify samples don't reach.

    check_tracers.py --work DIR

python_trace.py: calls made in a thread the program starts are recorded; the tracer exits as the
program does (a string given to sys.exit() is printed and means status 1).
js_profile_trace.py: a profiles folder with no .cpuprofile files is an error, not an empty trace;
a skip-level A -> C (A -> B -> C in the same profile, A -> C rare) is set aside, not counted.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

TRACERS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Core", "tracers")


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=60)


def python_trace(work, name, source):
    app = os.path.join(work, name)
    os.makedirs(app)
    with open(os.path.join(app, "main.py"), "w") as f:
        f.write(source)
    out = os.path.join(work, name + ".json")
    r = run([sys.executable, "-I", os.path.join(TRACERS, "python_trace.py"), "--root", app, "-o", out, "--",
             os.path.join(app, "main.py")])
    return r, json.load(open(out))


def js_skip_level(work):
    root = os.path.join(work, "skip")
    prof = os.path.join(root, "prof")
    os.makedirs(prof)
    path = os.path.join(root, "m.js")
    open(path, "w").write("\n" * 12)
    nodes = [{"id": "js:m.js", "kind": "module", "name": "m.js", "frontend": "js", "loc": {"file": "m.js", "line": 1, "end_line": 12}}]
    for name, line in (("a", 1), ("b", 4), ("c", 7), ("d", 10)):
        nodes.append({"id": "js:m.js#" + name, "kind": "function", "name": name, "frontend": "js",
                      "loc": {"file": "m.js", "line": line, "end_line": line + 1}})
    graph = os.path.join(root, "g.json")
    json.dump({"ir_version": "0.1.0", "nodes": nodes, "edges": []}, open(graph, "w"))
    pnodes, ids = [{"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": 0}, "children": []}], [1]

    def add(parent, fn, line):
        i = len(pnodes) + 1
        pnodes.append({"id": i, "callFrame": {"functionName": fn, "url": "file://" + path, "lineNumber": line}, "children": []})
        pnodes[parent - 1]["children"].append(i)
        return i

    for k in range(12):  # 12 call paths root -> a -> b -> c
        a = add(1, "a", 0)
        add(add(a, "b", 3), "c", 6)
    add(add(1, "a", 0), "c", 6)  # skip-level: a -> c with b missing
    add(add(1, "b", 3), "d", 9)  # b -> d once, no path through another frame
    json.dump({"nodes": pnodes}, open(os.path.join(prof, "p.cpuprofile"), "w"))
    out = os.path.join(root, "t.json")
    r = run([sys.executable, "-I", os.path.join(TRACERS, "js_profile_trace.py"), "--graph", graph, "--root", root,
             "--profiles", prof, "-o", out])
    assert r.returncode == 0, r.stderr
    t = json.load(open(out))
    pairs = {(c["from"], c["to"]): c["count"] for c in t["calls"]}
    assert pairs.get(("js:m.js#a", "js:m.js#b")) == 12 and pairs.get(("js:m.js#b", "js:m.js#c")) == 12, pairs
    assert ("js:m.js#a", "js:m.js#c") not in pairs, pairs
    assert ("js:m.js#b", "js:m.js#d") in pairs, pairs
    assert t["skip_level_artifacts"] == [{"from": "js:m.js#a", "to": "js:m.js#c", "count": 1, "via": ["js:m.js#b"]}], t
    r = run([sys.executable, "-I", os.path.join(TRACERS, "js_profile_trace.py"), "--graph", graph, "--root", root,
             "--profiles", prof, "-o", out, "--keep-skip-level"])
    assert ("js:m.js#a", "js:m.js#c") in {(c["from"], c["to"]) for c in json.load(open(out))["calls"]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)

    r, trace = python_trace(a.work, "threads", """\
import threading

def helper():
    return 1

def worker():
    helper()

def main():
    t = threading.Thread(target=worker)
    t.start()
    t.join()

main()
""")
    assert r.returncode == 0, r.stderr
    pairs = {(c["from"], c["to"]) for c in trace["calls"]}
    assert ("py:main.worker", "py:main.helper") in pairs, pairs

    r, trace = python_trace(a.work, "exit_text", """\
import sys

def fail():
    sys.exit("went wrong")

fail()
""")
    assert r.returncode == 1 and "went wrong" in r.stderr, (r.returncode, r.stderr)
    assert ("py:main", "py:main.fail") in {(c["from"], c["to"]) for c in trace["calls"]}, trace

    r, _ = python_trace(a.work, "exit_code", "import sys\nsys.exit(3)\n")
    assert r.returncode == 3, r.returncode

    empty = os.path.join(a.work, "no_profiles")
    os.makedirs(empty)
    graph = os.path.join(a.work, "g.json")
    with open(graph, "w") as f:
        json.dump({"ir_version": "0.1.0", "nodes": [], "edges": []}, f)
    r = run([sys.executable, "-I", os.path.join(TRACERS, "js_profile_trace.py"), "--graph", graph, "--root", a.work,
             "--profiles", empty, "-o", os.path.join(a.work, "t.json")])
    assert r.returncode != 0 and "no .cpuprofile files" in r.stderr, (r.returncode, r.stderr)
    js_skip_level(a.work)
    print("tracers: ok")


if __name__ == "__main__":
    main()
