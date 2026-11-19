#!/usr/bin/env python3
"""Checks the merge-duplicates fixer (docs/fixes.md) on its sample project, in a fresh copy.

    check_fix.py apply|dry|revert --optum OPTUM --sample DIR --work DIR

apply   merges 2 copies, refuses 3, tests pass before and after, and pass again afterwards
dry     prints the diff and changes nothing
revert  a test the merge breaks (a merged copy takes the kept function's __name__) fails after
        the change, so every file is restored
"""
import argparse
import filecmp
import json
import os
import shutil
import subprocess
import sys

FIXER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Core", "fixers", "merge_duplicates.py")
TESTS = f"{sys.executable} -B -m unittest -q"


def same_tree(a, b):
    cmp = filecmp.dircmp(a, b, ignore=["__pycache__"])
    if cmp.left_only or cmp.right_only or cmp.diff_files or cmp.funny_files:
        return False
    return all(same_tree(os.path.join(a, d), os.path.join(b, d)) for d in cmp.common_dirs)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=100, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["apply", "dry", "revert"])
    ap.add_argument("--optum", required=True)
    ap.add_argument("--sample", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    shutil.copytree(a.sample, a.work)
    report = a.work.rstrip("/") + ".json"
    cmd = [sys.executable, "-I", FIXER, a.work, "--optum", a.optum, "--report", report]
    if a.mode == "apply":
        r = run(cmd + ["--test", TESTS, "--apply"])
        assert r.returncode == 0, r.stderr
        rep = json.load(open(report))
        assert rep["applied"] and len(rep["merged"]) == 2 and len(rep["refused"]) == 3, rep
        assert rep["tests"] == {"before": True, "after": True}, rep
        t = run(TESTS, shell=True, cwd=a.work)
        assert t.returncode == 0, t.stderr
    elif a.mode == "dry":
        r = run(cmd)
        assert r.returncode == 0 and "merged by optum" in r.stdout, r.stderr
        assert same_tree(a.sample, a.work), "the dry run changed files"
    else:
        check = (f"{sys.executable} -B -c \"import shop.legacy.totals as t; "
                 f"assert t.old_invoice_total.__name__ == 'old_invoice_total'\"")
        r = run(cmd + ["--test", check, "--apply"])
        assert r.returncode == 1 and "every file restored" in r.stderr, r.stderr
        assert same_tree(a.sample, a.work), "files were not restored"
    print(f"{a.mode}: ok")


if __name__ == "__main__":
    main()
