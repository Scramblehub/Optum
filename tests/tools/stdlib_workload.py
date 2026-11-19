"""A small standard-library workload, for measuring the Python frontend's recall on real code.

    optum graph "$(python3 -c 'import sysconfig; print(sysconfig.get_paths()["stdlib"])')" -q -o stdlib.json
    python3 -I Core/tracers/python_trace.py --root <that stdlib path> -o trace.json -- tests/tools/stdlib_workload.py
    optum verify stdlib.json trace.json

Results: docs/notes/2026-11-18-python-frontend-eval.md and docs/verify.md.
"""
import json, sys
import argparse, difflib, textwrap, string, fractions, statistics, csv, io, configparser, shlex, email.parser, email.policy, pprint, ipaddress, calendar, urllib.parse, decimal, heapq, dataclasses

json.loads(json.dumps({"a": [1, 2, {"b": None}]}, indent=2))
p = argparse.ArgumentParser(); p.add_argument("--x", type=int, default=3); p.add_argument("files", nargs="*"); p.parse_args(["--x", "4", "a", "b"])
list(difflib.unified_diff(["a\n", "b\n", "c\n"], ["a\n", "B\n", "c\n"])); difflib.SequenceMatcher(None, "abcd", "bcde").ratio()
textwrap.fill("hello world " * 20, width=30); textwrap.dedent("   a\n   b")
string.Template("$x and $y").substitute(x=1, y=2)
fractions.Fraction(3, 4) + fractions.Fraction(1, 6); statistics.median([3, 1, 2]); statistics.stdev([1.0, 2.0, 4.0])
list(csv.reader(io.StringIO("a,b\n1,2\n"))); w = csv.DictWriter(io.StringIO(), ["a"]); w.writeheader(); w.writerow({"a": 1})
c = configparser.ConfigParser(); c.read_string("[s]\nk = v\n"); c.get("s", "k")
shlex.split("a 'b c' d"); email.parser.Parser(policy=email.policy.default).parsestr("Subject: hi\n\nbody")
pprint.pformat({"k": list(range(30))}); ipaddress.ip_network("10.0.0.0/24").num_addresses; calendar.month(2026, 11)
urllib.parse.urlparse("https://a.example.com/x?y=1"); urllib.parse.urlencode({"a": 1, "b": "c d"})
@dataclasses.dataclass
class P:
    x: int = 0
P(1)
