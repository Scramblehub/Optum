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
a call to the class (Optum's rule), `super().__init__()` as a call to that `__init__`. The class is
the one actually called, read from the running `__init__`/`__new__`: `Sub()` stays `Sub` when Sub
inherits its constructor from a base.
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
import threading


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

    def module_id(path):
        f = os.path.realpath(path)
        if not f.startswith(root + os.sep) and f != root:
            return None
        mod = os.path.relpath(f, base)[:-3].replace(os.sep, ".")
        return mod[: -len(".__init__")] if mod.endswith(".__init__") else mod

    def class_id(cls):
        """The node id of a class, or None if it isn't defined under the root."""
        f = getattr(sys.modules.get(getattr(cls, "__module__", None)), "__file__", None)
        q = getattr(cls, "__qualname__", None)
        if not f or not f.endswith(".py") or not isinstance(q, str):
            return None
        mod = module_id(f)
        return "py:" + mod + "." + re.sub(r"\.<locals>", "", q) if mod else None

    def constructed_class(frame):
        """For a constructor frame (__init__ or __new__), the id of the class being built."""
        code = frame.f_code
        if not code.co_argcount:
            return None
        first = frame.f_locals.get(code.co_varnames[0])
        if code.co_name == "__new__":
            if not isinstance(first, type) or issubclass(first, type):
                return None  # a metaclass's __new__ building a class: keep it as is
            return class_id(first)
        return class_id(type(first))

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
        if code.co_name in ("__init__", "__new__"):
            line = linecache.getline(back.f_code.co_filename, back.f_lineno)
            if "super(" not in line and "." + code.co_name + "(" not in line:
                # Cls() -> the class, as Optum draws it: the class called, which may inherit its
                # constructor from the base the code belongs to.
                cls = constructed_class(frame)
                if cls:
                    callee = cls
                elif code.co_name == "__init__":
                    callee = callee[: -len(".__init__")]
        if callee != caller:
            calls[(caller, callee)] = calls.get((caller, callee), 0) + 1

    sys.argv = target if target else [args.module] + target
    sys.path.insert(0, os.path.dirname(main_file) if main_file else os.getcwd())
    sys.setprofile(profile)
    threading.setprofile(profile)  # threads the program starts (sys.setprofile covers only this one)
    status = 0
    try:
        if args.module:
            runpy.run_module(args.module, run_name="__main__", alter_sys=True)
        else:
            runpy.run_path(main_file, run_name="__main__")
    except SystemExit as e:
        # As Python itself exits: None is success, an int its own status, anything else is printed
        # and means failure.
        if e.code is None:
            status = 0
        elif isinstance(e.code, int):
            status = e.code
        else:
            print(e.code, file=sys.stderr)
            status = 1
    finally:
        sys.setprofile(None)
        threading.setprofile(None)
        with open(args.out, "w") as f:
            json.dump({"trace_version": "0.1", "source": root, "language": "python",
                       "calls": [{"from": a, "to": b, "count": n} for (a, b), n in sorted(calls.items())]}, f, indent=1)
    sys.exit(status)


if __name__ == "__main__":
    main()
