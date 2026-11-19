#!/usr/bin/env python3
"""Recall of optum diagnose's duplicated-logic finding on real code: plant mutated copies of real
functions into a copy of a Python project, then check which copies are reported with their
original (docs/diagnose.md, "Duplicated logic").

    plant_clones.py plant  --src /usr/lib/python3.12 --out WORK/ [--n 40] [--seed 1]
    optum diagnose WORK/ -q -f json -o diagnosis.json
    plant_clones.py score  --key WORK/planted_key.json --diagnosis diagnosis.json

Copies, one of each type per sampled function (10 to 60 lines and 60+ code tokens, so above
optum's size limits; top-level or method):
  exact    the same source, under a new name
  renamed  every local name (parameters and assigned names) and every string literal changed
  edited   one statement added inside the body (a small edit)
They go into WORK/planted_clones.py as top-level functions, so ids are py:planted_clones.<name>.
"""
import argparse
import ast
import io
import json
import os
import random
import shutil
import sys
import textwrap
import tokenize


def module_name(rel):
    rel = rel[:-3].replace(os.sep, ".")
    return rel[: -len(".__init__")] if rel.endswith(".__init__") else rel


def candidates(src):
    out = []
    for dirpath, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in ("test", "tests", "idlelib", "lib2to3", "__pycache__", "site-packages")
                   and not d.startswith(".")]
        for f in files:
            if not f.endswith(".py"):
                continue
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, src)
            try:
                text = open(path, encoding="utf-8").read()
                tree = ast.parse(text)
            except (SyntaxError, UnicodeDecodeError, ValueError):
                continue
            mod = module_name(rel)
            for node in tree.body:
                defs = [(node, node.name)] if isinstance(node, ast.FunctionDef) else []
                if isinstance(node, ast.ClassDef):
                    defs = [(m, node.name + "." + m.name) for m in node.body if isinstance(m, ast.FunctionDef)]
                for fn, qual in defs:
                    lines = fn.end_lineno - fn.lineno + 1
                    if 10 <= lines <= 60 and not fn.decorator_list:
                        seg = ast.get_source_segment(text, fn, padded=True)
                        if seg and code_tokens(textwrap.dedent(seg)) >= 60:
                            out.append(("py:" + mod + "." + qual, textwrap.dedent(seg), fn))
    return out


def code_tokens(code):
    """Tokens of the body, without the docstring and comments: above optum's size limit (50
    tokens) means the function is in scope for clone detection."""
    try:
        fn = ast.parse(code).body[0]
        doc_end = fn.body[0].end_lineno if (fn.body and isinstance(fn.body[0], ast.Expr)
                                            and isinstance(fn.body[0].value, ast.Constant)) else 0
        n = 0
        for t in tokenize.generate_tokens(io.StringIO(code).readline):
            if t.start[0] <= max(doc_end, fn.body[0].lineno - 1 if fn.body else 0):
                continue
            if t.type in (tokenize.COMMENT, tokenize.NL, tokenize.NEWLINE, tokenize.INDENT, tokenize.DEDENT, tokenize.ENDMARKER):
                continue
            n += 1
        return n
    except (SyntaxError, tokenize.TokenError, IndentationError):
        return 0


def rename_def(code, new):
    tree = ast.parse(code)
    old = tree.body[0].name
    return code.replace("def " + old + "(", "def " + new + "(", 1)


def renamed(code, new):
    """Every local name and string literal changed, keeping the token structure."""
    fn = ast.parse(code).body[0]
    local = {a.arg for a in fn.args.args + fn.args.kwonlyargs + fn.args.posonlyargs}
    for n in ast.walk(fn):
        if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Store):
            local.add(n.id)
    local.discard("self")
    toks, prev = [], None
    for t in tokenize.generate_tokens(io.StringIO(code).readline):
        s = t.string
        if t.type == tokenize.NAME and prev == "def":
            s = new  # the function's own name (untokenize respaces "def f(", so rename it here)
        elif t.type == tokenize.NAME and s in local:
            s = "v_" + s
        elif t.type == tokenize.STRING and not s.lstrip("rbuRBU").startswith(('"""', "'''")) and "f" not in s[:2].lower():
            s = repr("x_" + str(abs(hash(s)) % 1000))
        toks.append((t.type, s))
        if t.type not in (tokenize.NL, tokenize.NEWLINE, tokenize.COMMENT):
            prev = t.string
    return tokenize.untokenize(toks)


def edited(code, new):
    """One statement added: after the first simple statement of the body, at its indentation."""
    fn = ast.parse(code).body[0]
    body = [s for s in fn.body if not (isinstance(s, ast.Expr) and isinstance(getattr(s, "value", None), ast.Constant))]
    if not body:
        return None
    first = body[0]
    lines = code.split("\n")
    indent = lines[first.lineno - 1][: len(lines[first.lineno - 1]) - len(lines[first.lineno - 1].lstrip())]
    lines.insert(first.end_lineno, indent + "_planted_counter = len(str(_planted_counter_base))")
    out = "\n".join(lines)
    try:
        ast.parse(out)
    except SyntaxError:
        return None
    return rename_def(out, new)


def plant(args):
    if os.path.exists(args.out):
        sys.exit(f"{args.out} exists: give a new directory")
    shutil.copytree(args.src, args.out, ignore=shutil.ignore_patterns("__pycache__", "site-packages", "test", "tests"))
    pool = candidates(args.src)
    random.seed(args.seed)
    random.shuffle(pool)
    key, chunks, n = [], ["_planted_counter_base = 0\n"], 0
    for orig, code, fn in pool:
        if n >= args.n:
            break
        try:
            ast.parse(code)
            copies = {"exact": rename_def(code, f"planted_{n}_exact"),
                      "renamed": renamed(code, f"planted_{n}_renamed"),
                      "edited": edited(code, f"planted_{n}_edited")}
        except (SyntaxError, tokenize.TokenError, IndentationError):
            continue
        if any(c is None for c in copies.values()):
            continue
        try:
            for c in copies.values():
                ast.parse(c)
        except SyntaxError:
            continue
        for kind, c in copies.items():
            chunks.append(c.rstrip() + "\n")
            key.append({"original": orig, "copy": f"py:planted_clones.planted_{n}_{kind}", "type": kind})
        n += 1
    with open(os.path.join(args.out, "planted_clones.py"), "w") as f:
        f.write("\n\n".join(chunks))
    with open(os.path.join(args.out, "planted_key.json"), "w") as f:
        json.dump({"source": args.src, "planted": key}, f, indent=1)
    print(f"planted {len(key)} copies of {n} functions in {args.out}/planted_clones.py")


def score(args):
    key = json.load(open(args.key))["planted"]
    d = json.load(open(args.diagnosis))
    group = {}
    for i, f in enumerate(d["findings"]):
        if f["kind"] == "duplicated_logic":
            for t in f["targets"]:
                group[t] = i
    found = {"exact": [0, 0], "renamed": [0, 0], "edited": [0, 0]}
    missed = []
    for k in key:
        found[k["type"]][1] += 1
        if k["original"] in group and group[k["original"]] == group.get(k["copy"]):
            found[k["type"]][0] += 1
        else:
            missed.append(k)
    for t, (hit, total) in found.items():
        print(f"{t:8} recall {hit}/{total} = {100 * hit / max(1, total):.0f}%")
    for k in missed[:12]:
        print("  missed", k["type"], k["original"], "->", k["copy"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plant")
    p.add_argument("--src", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--n", type=int, default=40)
    p.add_argument("--seed", type=int, default=1)
    s = sub.add_parser("score")
    s.add_argument("--key", required=True)
    s.add_argument("--diagnosis", required=True)
    args = ap.parse_args()
    plant(args) if args.cmd == "plant" else score(args)


if __name__ == "__main__":
    main()
