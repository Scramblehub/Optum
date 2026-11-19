#!/usr/bin/env python3
"""The recording proxy's records (testco/observe/record.py): secrets redacted, everything else
kept readable (the behaviour spec is learned from it), HAR import in the same format."""
import base64, json, os, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "testco", "observe"))
from record import REDACTED, body_value, har_to_records, is_secret_key, make_record, redact_headers  # noqa: E402

# Headers: credentials go, the rest stays.
h = redact_headers({"Authorization": "Bearer sk_live_x", "Cookie": "s=1", "Content-Type": "application/json"})
assert h == {"authorization": REDACTED, "cookie": REDACTED, "content-type": "application/json"}, h

# Keys: whole words, so look-alikes stay readable.
for k in ["password", "api_key", "apiKey", "access_token", "client_secret", "X-Auth", "cvc", "ssn", "tin"]:
    assert is_secret_key(k), k
for k in ["destination", "passenger", "setting", "routing", "number", "invoice_number", "tokenizer_name", "pinned"]:
    assert not is_secret_key(k), k
assert is_secret_key("number", card_context=True) and is_secret_key("card[number]")

# JSON bodies: nested secrets, card numbers in a card only.
v, _ = body_value(json.dumps({"email": "a@b.c", "password": "p", "invoice": {"number": "INV-7"},
                              "card": {"number": "4242424242424242", "exp_month": 12, "cvc": "123"},
                              "items": [{"api_key": "k", "sku": "S1"}]}).encode(), "application/json")
assert v == {"email": "a@b.c", "password": REDACTED, "invoice": {"number": "INV-7"},
             "card": {"number": REDACTED, "exp_month": 12, "cvc": REDACTED},
             "items": [{"api_key": REDACTED, "sku": "S1"}]}, v

# Form bodies (Stripe's style).
v, _ = body_value(b"email=a%40b.c&card[number]=4242&amount=500", "application/x-www-form-urlencoded")
assert v == {"email": "a@b.c", "card[number]": REDACTED, "amount": "500"}, v

# Odoo's JSON-RPC: the password is an argument, not a key.
rpc = {"jsonrpc": "2.0", "method": "call", "params": {"service": "object", "method": "execute_kw",
       "args": ["testco", 2, "hunter2", "res.partner", "read", [[7]]]}}
v, _ = body_value(json.dumps(rpc).encode(), "application/json", tool="accounting")
assert v["params"]["args"] == ["testco", 2, REDACTED, "res.partner", "read", [[7]]], v
v, _ = body_value(json.dumps(rpc).encode(), "application/json", tool="payments")  # the rule is Odoo's only
assert v["params"]["args"][2] == "hunter2"

# Binary and oversized bodies.
v, cut = body_value(b"\xff\xfe\x00", "application/octet-stream")
assert v == {"base64": base64.b64encode(b"\xff\xfe\x00").decode()} and not cut
v, cut = body_value(b"x" * (300 * 1024), "text/plain")
assert cut and len(v) == 256 * 1024

# A full record: secrets in query strings too.
r = make_record("crm", "2026-11-19T20:00:00Z", 3.14159, "GET", "/api/contacts", {"api_key": "k", "q": "smith"},
                {"Accept": "application/json"}, None, None, 200, {"Set-Cookie": "s=2"}, b'{"total": 1}', "application/json")
assert r["query"] == {"api_key": REDACTED, "q": "smith"} and r["response"]["headers"]["set-cookie"] == REDACTED
assert r["response"]["body"] == {"total": 1} and r["duration_ms"] == 3.14 and r["recording_version"] == "0.1"

# HAR: the named hosts only, same format and redaction.
har = {"log": {"entries": [
    {"startedDateTime": "2026-11-19T20:00:00Z", "time": 5,
     "request": {"method": "POST", "url": "http://127.0.0.1:8069/jsonrpc?debug=1",
                 "headers": [{"name": "Cookie", "value": "session_id=abc"}],
                 "postData": {"mimeType": "application/json", "text": json.dumps(rpc)}},
     "response": {"status": 200, "headers": [], "content": {"mimeType": "application/json", "text": '{"result": []}'}}},
    {"startedDateTime": "2026-11-19T20:00:01Z", "time": 2,
     "request": {"method": "GET", "url": "https://fonts.example.net/x.woff2", "headers": []},
     "response": {"status": 200, "headers": [], "content": {"mimeType": "font/woff2", "text": "AAEC", "encoding": "base64"}}}]}}
recs = har_to_records(har, lambda host: "accounting" if host == "127.0.0.1" else None)
assert len(recs) == 1 and recs[0]["tool"] == "accounting" and recs[0]["query"] == {"debug": "1"}
assert recs[0]["request"]["headers"]["cookie"] == REDACTED and recs[0]["request"]["body"]["params"]["args"][2] == REDACTED
print("observe: secrets redacted, the rest readable; HAR import in the same format")
