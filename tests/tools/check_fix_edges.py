#!/usr/bin/env python3
"""Edge cases of the merge-duplicates fixer (docs/fixes.md) that the sample project doesn't cover.

    check_fix_edges.py --optum OPTUM --sample DIR --work DIR

annotations  two functions that differ only in their return annotation are not the same code
crlf         a project with Windows line endings keeps them after a merge (no file is rewritten
             with \\n), and a failed test run restores every file byte for byte
"""
import argparse
import ast
import importlib.util
import os
import shutil
import subprocess
import sys

FIXER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Core", "fixers", "merge_duplicates.py")
TESTS = f"{sys.executable} -B -m unittest -q"


def load_fixer():
    spec = importlib.util.spec_from_file_location("merge_duplicates", FIXER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def check_annotations(fixer):
    def fn(src):
        return ast.parse(src).body[0]

    a = fn("def f(x) -> int:\n    return x + 1\n")
    b = fn("def g(x) -> str:\n    return x + 1\n")
    c = fn("def h(x) -> int:\n    return x + 1\n")
    d = fn("def k(x):\n    return x + 1\n")
    assert fixer.body_dump(a) != fixer.body_dump(b), "a different return annotation must not compare equal"
    assert fixer.body_dump(a) != fixer.body_dump(d), "an annotation against none must not compare equal"
    assert fixer.body_dump(a) == fixer.body_dump(c), "the same code under another name compares equal"


def all_bytes(root):
    out = {}
    for d, _, files in os.walk(root):
        if "__pycache__" in d:
            continue
        for f in files:
            with open(os.path.join(d, f), "rb") as fh:
                out[os.path.relpath(os.path.join(d, f), root)] = fh.read()
    return out


def to_crlf(root):
    for path in all_bytes(root):
        full = os.path.join(root, path)
        with open(full, "rb") as fh:
            data = fh.read()
        with open(full, "wb") as fh:
            fh.write(data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=100)


def check_crlf(optum, sample, work):
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(sample, work)
    to_crlf(work)
    before = all_bytes(work)
    cmd = [sys.executable, "-I", FIXER, work, "--optum", optum]

    # A merge that the tests reject: every file back, byte for byte, CRLF included.
    broken = f"{sys.executable} -B -c \"import shop.legacy.totals as t; assert t.old_invoice_total.__name__ == 'old_invoice_total'\""
    r = run(cmd + ["--test", broken, "--apply"])
    assert r.returncode == 1 and "every file restored" in r.stderr, r.stderr
    assert all_bytes(work) == before, "a restore changed bytes"

    # A merge that is kept: the files it touched still use CRLF throughout.
    r = run(cmd + ["--test", TESTS, "--apply"])
    assert r.returncode == 0, r.stderr
    after = all_bytes(work)
    changed = [p for p in after if after[p] != before.get(p)]
    assert changed, "nothing was merged"
    for p in changed:
        assert b"\r\n" in after[p] and b"\n" not in after[p].replace(b"\r\n", b""), f"{p} lost its CRLF line endings"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--optum", required=True)
    ap.add_argument("--sample", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    check_annotations(load_fixer())
    check_crlf(a.optum, a.sample, a.work)
    print("fix edge cases: ok")


if __name__ == "__main__":
    main()
