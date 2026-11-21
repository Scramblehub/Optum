"""mitmproxy addon: record every exchange with a tool as a recording line (record.py).

The proxy runs in reverse mode, one port per tool (compose.yaml); the tool is the upstream's host
name (payments, accounting, automation...). Lines go to $RECORDINGS_DIR/<tool>.jsonl.
"""
import datetime
import json
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from record import make_record  # noqa: E402

DIR = os.environ.get("RECORDINGS_DIR", "/recordings")


class Recorder:
    def __init__(self):
        os.makedirs(DIR, exist_ok=True)
        self.names = {}

    def client(self, flow):
        """Who called: the calling container's service name (testco-automation-1... -> automation), from
        its address; the address itself if it has no name."""
        ip = flow.client_conn.peername[0] if flow.client_conn.peername else ""
        if ip not in self.names:
            try:
                name = socket.gethostbyaddr(ip)[0].split(".")[0]
                parts = name.split("-")
                if len(parts) >= 3 and parts[-1].isdigit():  # compose names: <project>-<service>-<n>
                    name = "-".join(parts[1:-1])
            except (OSError, IndexError):
                name = ip
            self.names[ip] = name
        return self.names[ip]

    @staticmethod
    def _write(tool, line):
        # Opened per write, not kept open: a recording deleted or moved while the proxy runs is
        # simply started again (a kept-open file would go on writing into the deleted one).
        with open(os.path.join(DIR, f"{tool}.jsonl"), "a", encoding="utf-8") as f:
            f.write(line + "\n")

    def response(self, flow):
        req, resp = flow.request, flow.response
        if resp is None:
            return
        tool = req.host  # the upstream in reverse mode: the compose service name
        start = flow.request.timestamp_start
        end = resp.timestamp_end or resp.timestamp_start or start
        started = datetime.datetime.fromtimestamp(start, datetime.timezone.utc).isoformat(timespec="milliseconds")
        rec = make_record(
            tool, started.replace("+00:00", "Z"), (end - start) * 1000.0, req.method, req.path.split("?")[0],
            dict(req.query.items(multi=False)) if req.query else {}, dict(req.headers.items(multi=False)),
            req.get_content(strict=False), req.headers.get("content-type"), resp.status_code,
            dict(resp.headers.items(multi=False)), resp.get_content(strict=False), resp.headers.get("content-type"))
        rec["client"] = self.client(flow)
        harness = req.headers.get("x-optum-harness")
        if harness:  # our own loading and checking tools, not the company's traffic
            rec["harness"] = harness
        self._write(tool, json.dumps(rec, ensure_ascii=False))


addons = [Recorder()]
