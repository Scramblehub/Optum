#!/usr/bin/env python3
"""A browser recording (HAR, as saved from the browser's developer tools) as tool recordings.

    python3 testco/observe/har_import.py session.har --tool shop.example.com=shop --tool 127.0.0.1:8069=accounting \\
        -o testco/recordings/

Requests to hosts not named with --tool are left out (other sites the browser talked to). Lines
are appended to <out>/<tool>.jsonl, in the same format and with the same redaction as the proxy's.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from record import har_to_records  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("har")
    ap.add_argument("--tool", action="append", default=[], metavar="HOST=TOOL", help="a host and the tool it is")
    ap.add_argument("-o", "--out", required=True, help="directory of recordings")
    a = ap.parse_args()
    hosts = dict(t.split("=", 1) for t in a.tool)
    def tool_for(host):
        return hosts.get(host) or next((t for h, t in hosts.items() if h.split(":")[0] == host), None)
    records = har_to_records(json.load(open(a.har, encoding="utf-8")), tool_for)
    os.makedirs(a.out, exist_ok=True)
    counts = {}
    for r in records:
        with open(os.path.join(a.out, r["tool"] + ".jsonl"), "a", encoding="utf-8") as f:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
        counts[r["tool"]] = counts.get(r["tool"], 0) + 1
    print(", ".join(f"{t}: {n}" for t, n in counts.items()) or "no requests to the named hosts")


if __name__ == "__main__":
    main()
