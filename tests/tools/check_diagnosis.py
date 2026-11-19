#!/usr/bin/env python3
"""Compare `optum diagnose -f json` with an answer key: the findings must match exactly, as
(kind, units, targets). Extra or missing findings fail; so does a finding with other members.

Usage: check_diagnosis.py report.json expected_diagnosis.json
"""
import json
import sys


def key(f):
    return (f["kind"], tuple(sorted(f["units"])), tuple(sorted(f["targets"])))


report, expected = (json.load(open(p)) for p in sys.argv[1:3])
got = {key(f) for f in report["findings"]}
want = {key(f) for f in expected["findings"]}
for k in sorted(want - got):
    print("missing:", k)
for k in sorted(got - want):
    print("unexpected:", k)
print(f"findings: {len(got & want)} of {len(want)} expected, {len(got - want)} unexpected")
sys.exit(0 if got == want else 1)
