#!/usr/bin/env python3
"""Record which functions call which while a Python program runs, as an Optum trace.

    python3 python_trace.py --root DIR -o trace.json -- script.py [args...]
    python3 python_trace.py --root DIR -o trace.json -m package.module [args...]

Only code under DIR is recorded, with node ids named as Optum's Python frontend names them
(docs/frontends.md): modules from paths relative to the outermost package around DIR. Then
`optum verify graph.json trace.json` says how many of the observed calls the static graph has.

Recorded: every call made by a call instruction in the caller, direct or indirect (through a
dispatch table, a callback, getattr...). Left out, because they aren't calls in the source: class
bodies running, operators and implicit special methods, property access (the caller's current
bytecode instruction isn't a call). Generator
expressions, comprehensions and lambdas count as their enclosing function. `Cls()` is recorded as
a call to the class (Optum's rule), `super().__init__()` as a call to that `__init__`.
"""
import argparse
import bisect
import dis
import inspect
import json
import linecache
import os
import re
import runpy
import sys


def package_base(root):
    base = os.path.realpath(root)
    while os.path.exists(os.path.join(base, "__init__.py")) and os.path.dirname(base) != base:
        base = os.path.dirname(base)
    return base


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], usage="%(prog)s --root DIR -o OUT (-m MODULE | script.py) [args...]")
    ap.add_argument("--root", required=True, help="only code under this directory is recorded")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("-m", dest="module", help="run a module, like python -m")
    ap.add_argument("target", nargs=argparse.REMAINDER, help="script.py and its arguments (after --)")
    args = ap.parse_args()
    target = [t for t in args.target if t != "--"] if args.target else []
    if not args.module and not target:
        ap.error("give a script to run, or -m MODULE")

    root = os.path.realpath(args.root)
    base = package_base(root)
    main_file = os.path.realpath(target[0]) if target else None

    def node_id(code):
        f = code.co_filename
        if f.startswith("<"):
            return None
        f = os.path.realpath(f)
        if not f.startswith(root + os.sep) and f != root:
            return None
        mod = os.path.relpath(f, base)[:-3].replace(os.sep, ".")
        if mod.endswith(".__init__"):
            mod = mod[: -len(".__init__")]
        q = code.co_qualname
        if q == "<module>":
            return "py:" + mod
        q = re.sub(r"\.<locals>", "", q)
        # Optum's rule: a definition named like a submodule (pkg.cli() next to pkg/cli.py) gets
        # "@def", the module keeps the plain id.
        sub = os.path.join(base, *(mod + "." + q.split(".")[0]).split("."))
        if "." not in q and (os.path.exists(sub + ".py") or os.path.exists(os.path.join(sub, "__init__.py"))):
            q += "@def"
        q = re.sub(r"(\.)?<[^>]*>$", "", q)  # genexpr, lambda, comprehension, 3.12 annotation scope -> enclosing
        return "py:" + mod + ("." + q if q else "")

    calls = {}
    instructions = {}  # code object -> (sorted offsets, their opnames)

    def is_call_site(code, lasti):
        # The caller's f_lasti can point past the call's inline cache entries (3.12), so the
        # instruction executing is the last one at or before it.
        if code not in instructions:
            ins = list(dis.get_instructions(code))
            instructions[code] = ([i.offset for i in ins], [i.opname for i in ins])
        offsets, names = instructions[code]
        k = bisect.bisect_right(offsets, lasti) - 1
        return k >= 0 and names[k].startswith("CALL")

    def profile(frame, event, arg):
        if event != "call":
            return
        code, back = frame.f_code, frame.f_back
        if back is None or code.co_name.startswith("<"):
            return
        if not code.co_flags & inspect.CO_OPTIMIZED:
            return  # a class body running, not a call
        callee, caller = node_id(code), node_id(back.f_code)
        if not callee or not caller:
            return
        # Only real calls: the caller must be at a call instruction (operators, property access
        # and other implicit calls come from other instructions).
        if not is_call_site(back.f_code, back.f_lasti):
            return
        if code.co_name == "__init__":
            line = linecache.getline(back.f_code.co_filename, back.f_lineno)
            if "super(" not in line and ".__init__(" not in line:
                callee = callee[: -len(".__init__")]  # Cls() -> the class, as Optum draws it
        if callee != caller:
            calls[(caller, callee)] = calls.get((caller, callee), 0) + 1

    sys.argv = target if target else [args.module] + target
    sys.path.insert(0, os.path.dirname(main_file) if main_file else os.getcwd())
    sys.setprofile(profile)
    status = 0
    try:
        if args.module:
            runpy.run_module(args.module, run_name="__main__", alter_sys=True)
        else:
            runpy.run_path(main_file, run_name="__main__")
    except SystemExit as e:
        status = e.code if isinstance(e.code, int) else 0
    finally:
        sys.setprofile(None)
        with open(args.out, "w") as f:
            json.dump({"trace_version": "0.1", "source": root, "language": "python",
                       "calls": [{"from": a, "to": b, "count": n} for (a, b), n in sorted(calls.items())]}, f, indent=1)
    sys.exit(status)


if __name__ == "__main__":
    main()
