#!/usr/bin/env python3
"""Merge exact duplicate Python functions and methods: keep one, make every copy refer to it.

    merge_duplicates.py DIR --optum OPTUM [--test "python3 -m pytest -q"] [--apply] [--report out.json]

Reads `optum diagnose DIR -f json` for duplicated-logic findings (docs/diagnose.md) and takes the
exact copies among top-level Python functions and methods of top-level classes. Callers don't
change: the old name now is the kept one.
  function -> an import of the kept function under the copy's name
              (`from shop.pricing import invoice_total as old_total`; same file: `old_total = invoice_total`)
  method   -> deleted when its class directly subclasses the kept method's class (single base);
              else `name = Kept.name` in the class body (with `from mod import Kept` if needed)

A copy is merged only when it is safe (docs/fixes.md):
  1. the same code: equal syntax trees (names and docstrings aside), no decorators, no `global` /
     `nonlocal`; for methods also no `super()`, `__class__` or private `__names` (they refer to the
     class the code is written in), and the kept class defined before the copy's in the same file;
  2. every global name the body uses means the same in both modules: a builtin in both, the same
     import, or the same constant; anything else (a different TAX_RATE, a local helper) refuses;
  3. no import cycle: the kept function's module doesn't import the copy's module;
  4. not regulated code: tax, legal or medical modules and functions (rules_2023.vat_due and
     rules_2024.vat_due are separate on purpose; each year's rules stay as filed), unless
     --allow-regulated;
  5. with --test, the project's tests pass before; after the change they must pass again, or every
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

# Regulated code (tax, legal, medical) stays as it is: the same words as the same-entity analysis
# (docs/diagnose.md, "The same data in several places").
REGULATED = {
    "tax": "tax", "taxes": "tax", "vat": "tax", "gst": "tax", "hst": "tax", "w2": "tax", "w4": "tax", "w8": "tax",
    "w9": "tax", "1099": "tax", "1040": "tax", "irs": "tax", "hmrc": "tax",
    "legal": "legal", "contract": "legal", "contracts": "legal", "agreement": "legal", "consent": "legal",
    "waiver": "legal", "nda": "legal", "affidavit": "legal", "litigation": "legal", "gdpr": "legal", "kyc": "legal",
    "aml": "legal",
    "medical": "medical", "patient": "medical", "patients": "medical", "diagnosis": "medical",
    "prescription": "medical", "clinical": "medical", "hipaa": "medical", "phi": "medical", "ehr": "medical",
    "emr": "medical", "healthcare": "medical", "icd10": "medical",
}


def regulated(node_id):
    """'tax', 'legal' or 'medical' if a word of the id (module path, function name) says so."""
    import re
    text = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", node_id[3:])
    for w in re.split(r"[^A-Za-z0-9]+", text.lower()):
        if w in REGULATED:
            return REGULATED[w]
    return None


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

    def find(t):  # the function's table, at top level or in a class
        for child in t.get_children():
            if child.get_name() == fn.name and child.get_type() == "function" and child.get_lineno() == fn.lineno:
                visit(child)
                return True
            if child.get_type() == "class" and find(child):
                return True
        return False

    find(table)
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


def reaches(start, target, load, added=None):
    """Does module `start` import `target`, directly or not (within the project)? `added`: imports
    that earlier merges of this run add (module -> modules), so merges together can't make a cycle."""
    seen, todo = set(), [start]
    while todo:
        m = todo.pop()
        if m in seen:
            continue
        seen.add(m)
        tree = load(m)
        imps = set(imports_of(tree, m)) if tree is not None else set()
        imps |= (added or {}).get(m, set())
        for imp in imps:
            if imp == target:
                return True
            todo.append(imp)
    return False


def run_tests(cmd, cwd):
    r = subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True)
    return r.returncode == 0, (r.stdout + r.stderr)[-2000:]


class Located:
    """A function or method, found from its node id in the source."""

    def __init__(self, node_id, mod, cls, fn, path):
        self.id, self.mod, self.cls, self.fn, self.path = node_id, mod, cls, fn, path
        self.name = fn.name


def locate(node_id, root, load):
    """py:pkg.mod.func or py:pkg.mod.Class.method -> Located, or None (nested, missing...)."""
    dotted = node_id[3:].split("@")[0]
    parts = dotted.split(".")
    for take in (1, 2):  # a function in a module, or a method in a class
        if len(parts) <= take:
            continue
        mod, qual = ".".join(parts[:-take]), parts[-take:]
        tree = load(mod)
        if tree is None:
            continue
        if take == 1:
            for node in tree.body:
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == qual[0]:
                    return Located(node_id, mod, None, node, module_file(root, mod)[0])
        else:
            for node in tree.body:
                if isinstance(node, ast.ClassDef) and node.name == qual[0]:
                    for m in node.body:
                        if isinstance(m, (ast.FunctionDef, ast.AsyncFunctionDef)) and m.name == qual[1]:
                            return Located(node_id, mod, node, m, module_file(root, mod)[0])
    return None


def class_bound(fn):
    """Names in a method that mean its own class: super(), __class__, private __names (mangled)."""
    for n in ast.walk(fn):
        names = []
        if isinstance(n, ast.Name):
            names.append(n.id)
        elif isinstance(n, ast.Attribute):
            names.append(n.attr)
        for x in names:
            if x in ("super", "__class__") or (x.startswith("__") and not x.endswith("__")):
                return x
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--optum", required=True, help="the optum binary")
    ap.add_argument("--test", help="the project's test command, run in DIR before and after")
    ap.add_argument("--apply", action="store_true", help="change the files (default: print the diff)")
    ap.add_argument("--report", help="write what was merged and refused, as JSON")
    ap.add_argument("--allow-regulated", action="store_true",
                    help="also merge copies in tax, legal or medical code (they're refused by default)")
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
    added = {}  # imports this run's merges add: copy's module -> kept module
    for f in findings:
        if f["kind"] != "duplicated_logic" or f["evidence"].get("clone_type") != "exact":
            continue
        ids = [m["id"] for m in f["evidence"]["members"] if m["id"].startswith("py:")]
        found = [(i, locate(i, root, load)) for i in ids]
        for i, loc in found:
            if loc is None:
                refused.append({"copy": i, "keep": None, "why": "not a function or a method of a top-level class"})
        members = [loc for _, loc in found if loc is not None]
        if len(members) < 2:
            continue
        # Keep: a function over a method, then the shortest module path (the most central), then the
        # one defined first in its file, then the first by id.
        members.sort(key=lambda d: (d.cls is not None, d.mod.count("."), d.fn.lineno, d.id))
        keep = members[0]
        if keep.fn.decorator_list:
            refused += [{"copy": c.id, "keep": keep.id, "why": "the kept one has decorators"} for c in members[1:]]
            continue
        kglobals = globals_used(sources[keep.mod], keep.fn)
        for c in members[1:]:
            def refuse(why):
                refused.append({"copy": c.id, "keep": keep.id, "why": why})

            if c.fn.decorator_list:
                refuse("has decorators")
                continue
            if (c.cls is None) != (keep.cls is None):
                refuse("a function and a method: not merged")
                continue
            kind = regulated(c.id) or regulated(keep.id)
            if kind and not args.allow_regulated:
                refuse(f"regulated ({kind}) code stays as it is (--allow-regulated to merge)")
                continue
            if body_dump(c.fn) != body_dump(keep.fn):
                refuse("not the same syntax tree")
                continue
            if any(isinstance(n, (ast.Global, ast.Nonlocal)) for n in ast.walk(c.fn)):
                refuse("uses global or nonlocal")
                continue
            if c.cls is not None:
                bound = class_bound(c.fn)
                if bound:
                    refuse(f"uses {bound}, which means the class it's written in")
                    continue
                if c.name in ("__init_subclass__", "__class_getitem__"):
                    # Implicitly class methods: Kept.__init_subclass__ would come back bound to Kept.
                    refuse(f"{c.name} is implicitly a class method")
                    continue
            cglobals = globals_used(sources[c.mod], c.fn)
            if cglobals != kglobals:
                refuse("reads different globals")
                continue
            ctree, ktree = load(c.mod), load(keep.mod)
            if c.mod != keep.mod:
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
                if reaches(keep.mod, c.mod, load, added):
                    refuse(f"import cycle: {keep.mod} imports {c.mod}" +
                           (" (with an import another merge adds)" if not reaches(keep.mod, c.mod, load) else ""))
                    continue

            start = c.fn.lineno
            end = c.fn.end_lineno
            indent = " " * c.fn.col_offset
            note = "  # merged by optum: an exact copy of " + keep.id[3:]
            imports = []
            if c.cls is None:  # a function
                if c.mod == keep.mod:
                    if keep.fn.lineno > c.fn.lineno:
                        refuse("in the same file, the copy comes first")
                        continue
                    line = f"{c.name} = {keep.name}"
                else:
                    line = f"from {keep.mod} import {keep.name}" + (f" as {c.name}" if c.name != keep.name else "")
            else:  # a method
                same_class = c.mod == keep.mod and c.cls is keep.cls
                if same_class:
                    if keep.fn.lineno > c.fn.lineno:
                        refuse("in the same class, the copy comes first")
                        continue
                    line = f"{c.name} = {keep.name}"
                else:
                    if c.mod == keep.mod and keep.cls.lineno > c.cls.lineno:
                        refuse("in the same file, the copy's class comes first")
                        continue
                    if c.mod != keep.mod:
                        b = binding(ctree, keep.cls.name)
                        if b is not None and b != ("from", 0, keep.mod, keep.cls.name):
                            refuse(f"{keep.cls.name} means something else in {c.mod}")
                            continue
                        if b is None:
                            imports.append(f"from {keep.mod} import {keep.cls.name}")
                    direct = (len(c.cls.bases) == 1 and isinstance(c.cls.bases[0], ast.Name)
                              and c.cls.bases[0].id == keep.cls.name and c.name == keep.name and not c.cls.keywords)
                    if direct:
                        line = None  # inherited: the copy just goes
                        imports = []
                    else:
                        line = f"{c.name} = {keep.cls.name}.{keep.name}"
            if c.mod != keep.mod:  # the copy's module now depends on the kept one's
                added.setdefault(c.mod, set()).add(keep.mod)
            merges.append({"copy": c.id, "keep": keep.id, "file": c.path, "start": start, "end": end,
                           "line": None if line is None else indent + line + note,
                           "imports": imports, "class_end": c.cls.end_lineno if c.cls else None,
                           "class_body": len(c.cls.body) if c.cls else None, "class_indent": indent,
                           "deleted_note": indent + "# merged-out: " + c.name + " is inherited from " + keep.id[3:] + " (merged by optum)"})

    # Rewrite: per file, bottom up so line numbers stay valid; imports go after the file's last
    # top-level import.
    new_text = {}
    for path in sorted({m["file"] for m in merges}):
        lines = open(path, encoding="utf-8").read().split("\n")
        for m in sorted((m for m in merges if m["file"] == path), key=lambda m: -m["start"]):
            replacement = [m["line"]] if m["line"] is not None else [m["deleted_note"]]
            lines[m["start"] - 1: m["end"]] = replacement
        needed = []
        for m in merges:
            if m["file"] == path:
                needed += [i for i in m["imports"] if i not in needed]
        if needed:
            tree = ast.parse(open(path, encoding="utf-8").read())
            after = 0
            for node in tree.body:
                if isinstance(node, (ast.Import, ast.ImportFrom)):
                    after = node.end_lineno
                elif after == 0 and isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant):
                    after = node.end_lineno  # the module docstring
                elif not isinstance(node, (ast.Import, ast.ImportFrom)):
                    break
            lines[after:after] = [i + "  # added by optum (merged copies)" for i in needed]
        new_text[path] = "\n".join(lines)
    for path, text in list(new_text.items()):  # the result must still parse
        try:
            ast.parse(text)
        except SyntaxError:
            # A class left with no statement once its copies went: a `pass` keeps it valid.
            text = text.replace("# merged-out:", "pass  #")
            try:
                ast.parse(text)
            except SyntaxError as e:
                sys.exit(f"internal error: the rewrite of {path} doesn't parse ({e}); nothing changed")
        new_text[path] = text.replace("# merged-out:", "#")

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
