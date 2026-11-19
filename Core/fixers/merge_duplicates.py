#!/usr/bin/env python3
"""Merge exact duplicate Python functions: keep one, make every copy an import of it.

    merge_duplicates.py DIR --optum OPTUM [--test "python3 -m pytest -q"] [--apply] [--report out.json]

Reads `optum diagnose DIR -f json` for duplicated-logic findings (docs/diagnose.md) and takes the
exact copies among top-level Python functions. Each copy's `def` is replaced by an import of the
kept function under the copy's name (`from shop.pricing import invoice_total as old_total`; in the
same file, `old_total = invoice_total`), so callers don't change: the old name now is the kept
function.

A copy is merged only when it is safe (docs/fixes.md):
  1. the same code: equal syntax trees (names of the functions and docstrings aside), top level,
     no decorators, no `global` / `nonlocal`;
  2. every global name the body uses means the same in both modules: a builtin in both, the same
     import, or the same constant; anything else (a different TAX_RATE, a local helper) refuses;
  3. no import cycle: the kept function's module doesn't import the copy's module;
  4. with --test, the project's tests pass before; after the change they must pass again, or every
     file is restored.

Without --apply it prints the diff and changes nothing.
"""
import argparse
import ast
import difflib
import json
import os
import shutil
import subprocess
import symtable
import sys
import tempfile

BUILTINS = set(dir(__builtins__)) if not isinstance(__builtins__, dict) else set(__builtins__)


def module_file(root, mod):
    """The file of a dotted module under the analysis root (or its parents: package roots)."""
    rel = mod.replace(".", os.sep)
    base = os.path.abspath(root)
    for _ in range(4):
        for cand in (os.path.join(base, rel + ".py"), os.path.join(base, rel, "__init__.py")):
            if os.path.isfile(cand):
                return cand, base
        parent = os.path.dirname(base)
        if parent == base:
            break
        base = parent
    return None, None


def split_id(node_id):
    """py:pkg.mod.func -> (pkg.mod, func). Only top-level functions: one dot past the module."""
    dotted = node_id[3:]
    if "@" in dotted:
        dotted = dotted.split("@")[0]
    mod, _, name = dotted.rpartition(".")
    return mod, name


def top_function(tree, name):
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == name:
            return node
    return None


def body_dump(fn):
    """The function's syntax tree without its name and docstring, for comparing copies."""
    body = fn.body
    if body and isinstance(body[0], ast.Expr) and isinstance(getattr(body[0], "value", None), ast.Constant) \
            and isinstance(body[0].value.value, str):
        body = body[1:]
    return ast.dump(ast.Module(body=body, type_ignores=[])) + ast.dump(fn.args) + str(isinstance(fn, ast.AsyncFunctionDef))


def globals_used(source, fn):
    """The global names a function's body reads (symtable: exact, nested scopes included)."""
    table = symtable.symtable(source, "<module>", "exec")
    out = set()

    def visit(t):
        for s in t.get_symbols():
            if s.is_global() or (s.is_free() and t.get_type() == "function" and not s.is_local()):
                out.add(s.get_name())
        for c in t.get_children():
            visit(c)

    for child in table.get_children():
        if child.get_name() == fn.name and child.get_type() == "function" and child.get_lineno() == fn.lineno:
            visit(child)
    return out


def binding(tree, name):
    """What a module-level name is bound to: ('import', ...), ('const', dump), or None."""
    found = None
    for node in tree.body:
        if isinstance(node, ast.Import):
            for a in node.names:
                if (a.asname or a.name.split(".")[0]) == name:
                    found = ("import", a.name if a.asname else a.name.split(".")[0])
        elif isinstance(node, ast.ImportFrom):
            for a in node.names:
                if (a.asname or a.name) == name:
                    found = ("from", node.level, node.module, a.name)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and node.name == name:
            found = ("def", id(node))  # a definition of its own: never the same as another module's
        elif isinstance(node, ast.Assign):
            for t in node.targets:
                if isinstance(t, ast.Name) and t.id == name:
                    v = node.value
                    found = ("const", ast.dump(v)) if isinstance(v, ast.Constant) or (
                        isinstance(v, (ast.Tuple, ast.List)) and all(isinstance(e, ast.Constant) for e in v.elts)) \
                        else ("def", id(node))
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name) and node.target.id == name:
            found = ("const", ast.dump(node.value)) if isinstance(node.value, ast.Constant) else ("def", id(node))
        elif isinstance(node, (ast.If, ast.Try, ast.With, ast.For, ast.While)):
            for sub in ast.walk(node):  # bound inside a block: unknown
                if isinstance(sub, ast.Name) and isinstance(sub.ctx, ast.Store) and sub.id == name:
                    found = ("def", id(sub))
    return found


def imports_of(tree, mod):
    """Modules a module imports at top level (absolute names; relative ones resolved from mod)."""
    out = set()
    pkg = mod.rsplit(".", 1)[0] if "." in mod else ""
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            out.update(a.name for a in node.names)
        elif isinstance(node, ast.ImportFrom):
            base = node.module or ""
            if node.level:
                parts = pkg.split(".") if pkg else []
                parts = parts[: len(parts) - (node.level - 1)] if node.level > 1 else parts
                base = ".".join(p for p in parts + ([node.module] if node.module else []) if p)
            out.add(base)
            out.update(base + "." + a.name for a in node.names)
    return out


def reaches(start, target, load):
    """Does module `start` import `target`, directly or not (within the project)?"""
    seen, todo = set(), [start]
    while todo:
        m = todo.pop()
        if m in seen:
            continue
        seen.add(m)
        tree = load(m)
        if tree is None:
            continue
        for imp in imports_of(tree, m):
            if imp == target:
                return True
            todo.append(imp)
    return False


def run_tests(cmd, cwd):
    r = subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True)
    return r.returncode == 0, (r.stdout + r.stderr)[-2000:]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--optum", required=True, help="the optum binary")
    ap.add_argument("--test", help="the project's test command, run in DIR before and after")
    ap.add_argument("--apply", action="store_true", help="change the files (default: print the diff)")
    ap.add_argument("--report", help="write what was merged and refused, as JSON")
    args = ap.parse_args()
    root = os.path.abspath(args.dir)

    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        out = tmp.name
    r = subprocess.run([args.optum, "diagnose", root, "-q", "-f", "json", "-o", out], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"optum diagnose failed: {r.stderr.strip()}")
    findings = json.load(open(out))["findings"]
    os.unlink(out)

    sources, trees = {}, {}

    def load(mod):
        if mod not in trees:
            path, _ = module_file(root, mod)
            trees[mod] = None
            if path:
                try:
                    sources[mod] = open(path, encoding="utf-8").read()
                    trees[mod] = ast.parse(sources[mod])
                except (SyntaxError, UnicodeDecodeError):
                    pass
        return trees[mod]

    merges, refused = [], []
    for f in findings:
        if f["kind"] != "duplicated_logic" or f["evidence"].get("clone_type") != "exact":
            continue
        members = [m["id"] for m in f["evidence"]["members"] if m["id"].startswith("py:")]
        if len(members) < 2:
            continue
        # Keep the copy in the shortest module path (the most central), then the first.
        members.sort(key=lambda i: (split_id(i)[0].count("."), i))
        keep = members[0]
        kmod, kname = split_id(keep)
        ktree = load(kmod)
        kfn = top_function(ktree, kname) if ktree else None
        if kfn is None or kfn.decorator_list:
            refused += [{"copy": m, "keep": keep, "why": "the kept function isn't a plain top-level function"} for m in members[1:]]
            continue
        kglobals = globals_used(sources[kmod], kfn)
        for copy in members[1:]:
            cmod, cname = split_id(copy)
            ctree = load(cmod)
            cfn = top_function(ctree, cname) if ctree else None

            def refuse(why):
                refused.append({"copy": copy, "keep": keep, "why": why})

            if cfn is None or cfn.decorator_list:
                refuse("not a plain top-level function")
                continue
            if body_dump(cfn) != body_dump(kfn):
                refuse("not the same syntax tree")
                continue
            if any(isinstance(n, (ast.Global, ast.Nonlocal)) for n in ast.walk(cfn)):
                refuse("uses global or nonlocal")
                continue
            cglobals = globals_used(sources[cmod], cfn)
            if cglobals != kglobals:
                refuse("reads different globals")
                continue
            if cmod != kmod:
                differ = []
                for name in sorted(cglobals):
                    bk, bc = binding(ktree, name), binding(ctree, name)
                    if bk is None and bc is None and name in BUILTINS:
                        continue
                    if bk is None or bc is None or bk != bc or bk[0] == "def":
                        differ.append(name)
                if differ:
                    refuse("globals mean different things in the two modules: " + ", ".join(differ))
                    continue
                if reaches(kmod, cmod, load):
                    refuse(f"import cycle: {kmod} imports {cmod}")
                    continue
            elif kfn.lineno > cfn.lineno:
                refuse("in the same file, the copy comes first")
                continue
            merges.append({"copy": copy, "keep": keep, "file": module_file(root, cmod)[0],
                           "start": (cfn.decorator_list[0].lineno if cfn.decorator_list else cfn.lineno),
                           "end": cfn.end_lineno,
                           "line": (f"{cname} = {kname}" if cmod == kmod else
                                    f"from {kmod} import {kname}" + (f" as {cname}" if cname != kname else ""))})

    # Rewrite: per file, bottom up so line numbers stay valid.
    new_text = {}
    for path in sorted({m["file"] for m in merges}):
        lines = open(path, encoding="utf-8").read().split("\n")
        for m in sorted((m for m in merges if m["file"] == path), key=lambda m: -m["start"]):
            indent = ""
            lines[m["start"] - 1: m["end"]] = [indent + m["line"] + "  # merged by optum: an exact copy of " + m["keep"][3:]]
        new_text[path] = "\n".join(lines)

    report = {"merged": merges, "refused": refused, "applied": False, "tests": None}
    for path, text in new_text.items():
        old = open(path, encoding="utf-8").read()
        sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), text.splitlines(True),
                                                   os.path.relpath(path, root), os.path.relpath(path, root)))
    for r in refused:
        print(f"refused: {r['copy']} (copy of {r['keep']}): {r['why']}", file=sys.stderr)
    print(f"{len(merges)} copies to merge, {len(refused)} refused", file=sys.stderr)

    if args.apply and merges:
        if args.test:
            ok, log = run_tests(args.test, root)
            if not ok:
                report["tests"] = {"before": False, "log": log}
                print("tests fail before the change: nothing applied", file=sys.stderr)
                write_report(args.report, report)
                sys.exit(1)
        backup = {p: open(p, encoding="utf-8").read() for p in new_text}
        for p, t in new_text.items():
            open(p, "w", encoding="utf-8").write(t)
        if args.test:
            ok, log = run_tests(args.test, root)
            if not ok:
                for p, t in backup.items():
                    open(p, "w", encoding="utf-8").write(t)
                report["tests"] = {"before": True, "after": False, "log": log}
                print("tests fail after the change: every file restored", file=sys.stderr)
                write_report(args.report, report)
                sys.exit(1)
            report["tests"] = {"before": True, "after": True}
        report["applied"] = True
        print(f"applied: {len(merges)} copies merged" + (", tests pass before and after" if args.test else ""),
              file=sys.stderr)
    write_report(args.report, report)


def write_report(path, report):
    if path:
        with open(path, "w") as f:
            json.dump(report, f, indent=1)


if __name__ == "__main__":
    main()
