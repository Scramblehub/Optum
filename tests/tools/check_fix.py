#!/usr/bin/env python3
"""Checks the merge-duplicates fixer (docs/fixes.md) on its sample project, in a fresh copy.

    check_fix.py apply|dry|revert --optum OPTUM --sample DIR --work DIR

apply   merges 6 copies (3 functions; 3 methods: inherited, same file, another module), refuses 6
        (different globals x2, a cycle, a cycle two merges would make together, regulated tax code,
        super()), tests pass before and after
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
        assert rep["applied"] and len(rep["merged"]) == 6 and len(rep["refused"]) == 6, rep
        merged = {m["copy"] for m in rep["merged"]}
        assert {"py:shop.money.Price.as_text", "py:shop.money.Refund.as_text",
                "py:shop.legacy.money.LegacyAmount.as_text"} <= merged, merged
        whys = sorted(r["why"].split(":")[0].split(" (")[0] for r in rep["refused"])
        assert sum(w.startswith("regulated") for w in whys) == 1, whys
        assert sum(w.startswith("globals mean") for w in whys) == 2, whys
        assert sum(w.startswith("import cycle") for w in whys) == 2, whys  # one only with another merge's import
        assert any("another merge adds" in r["why"] for r in rep["refused"]), rep["refused"]
        assert sum(w.startswith("uses super") for w in whys) == 1, whys
        money = open(os.path.join(a.work, "shop", "money.py")).read()
        assert "pass  # as_text is inherited from shop.money.Money.as_text" in money, money  # Price(Money): deleted
        assert "    as_text = Money.as_text  # merged by optum" in money, money                  # Refund: assigned
        legacy = open(os.path.join(a.work, "shop", "legacy", "money.py")).read()
        assert "from shop.money import Money" in legacy and "as_text = Money.as_text" in legacy, legacy
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
