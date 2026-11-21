#!/usr/bin/env python3
"""Edge cases of the tracers (Core/tracers/) that the verify samples don't reach.

    check_tracers.py --work DIR

python_trace.py: calls made in a thread the program starts are recorded; the tracer exits as the
program does (a string given to sys.exit() is printed and means status 1).
js_profile_trace.py: a profiles folder with no .cpuprofile files is an error, not an empty trace.
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
    print("tracers: ok")


if __name__ == "__main__":
    main()
