"""Recordings of a tool's traffic, for learning its behaviour from outside (docs/plans/phase3.md,
"Learning a tool we can't read"). Pure functions: no mitmproxy or Docker needed, so they're
tested on their own (tests/tools/check_observe.py) and shared by the proxy addon and HAR import.

A recording is JSON lines, one exchange per line:

    {"recording_version": "0.1", "tool": "accounting", "started": "2026-11-19T20:01:02.123Z",
     "duration_ms": 12.5, "method": "POST", "path": "/jsonrpc", "query": {},
     "request": {"headers": {...}, "body": <JSON, text, or {"base64": ...}>},
     "response": {"status": 200, "headers": {...}, "body": ...}}

Secrets never reach a recording: credential headers, and body fields named like secrets
(password, token, api key, card number, ...) become "[redacted]"; tools whose APIs pass secrets
positionally (Odoo's JSON-RPC: the password is an argument) have their own rule.
"""
import base64
import json
import re
import urllib.parse

RECORDING_VERSION = "0.1"
REDACTED = "[redacted]"
MAX_BODY = 256 * 1024  # bigger bodies are cut, with "truncated": true

SECRET_HEADERS = {"authorization", "proxy-authorization", "cookie", "set-cookie", "x-api-key", "api-key",
                  "x-auth-token", "x-n8n-api-key", "stripe-signature", "x-hub-signature", "x-hub-signature-256"}
# Words that make a field a secret. Whole words of the key (split at _, -, [, ], camelCase), so
# "destination" or "passenger" stay readable: over-redacting would hide what the spec must learn.
SECRET_WORDS = {"password", "passwd", "passphrase", "pwd", "secret", "token", "apikey", "authorization", "auth",
                "cvc", "cvv", "ssn", "tin", "itin", "iban", "pin", "otp", "sessionid", "csrf"}
SECRET_PAIRS = {("api", "key"), ("access", "token"), ("refresh", "token"), ("client", "secret"), ("session", "id")}


def key_words(key):
    key = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", str(key))
    return [w for w in re.split(r"[^A-Za-z0-9]+", key.lower()) if w]


def is_secret_key(key, card_context=False):
    words = key_words(key)
    if any(w in SECRET_WORDS for w in words) or any(p in zip(words, words[1:]) for p in SECRET_PAIRS):
        return True
    # A card number: "number" inside a card, or a form key like card[number].
    return "number" in words and (card_context or "card" in words)


def redact_headers(headers):
    return {k.lower(): (REDACTED if k.lower() in SECRET_HEADERS else v) for k, v in headers.items()}


def _redact_value(obj, card_context=False):
    if isinstance(obj, dict):
        out = {}
        for k, v in obj.items():
            in_card = card_context or str(k).lower() in ("card", "payment_method_data", "source")
            if isinstance(v, (dict, list)):
                out[k] = _redact_value(v, in_card)
            elif is_secret_key(k, in_card):
                out[k] = REDACTED
            else:
                out[k] = v
        return out
    if isinstance(obj, list):
        return [_redact_value(v, card_context) for v in obj]
    return obj


def _odoo_jsonrpc(body):
    """Odoo's JSON-RPC passes the password positionally: common.login/authenticate(db, login,
    password, ...), object.execute/execute_kw(db, uid, password, ...)."""
    params = body.get("params") if isinstance(body, dict) else None
    if isinstance(params, dict) and params.get("service") in ("common", "object") and isinstance(params.get("args"), list):
        args = list(params["args"])
        if len(args) > 2 and params.get("method") in ("login", "authenticate", "execute", "execute_kw"):
            args[2] = REDACTED
        body = {**body, "params": {**params, "args": args}}
    return body


TOOL_RULES = {"accounting": _odoo_jsonrpc}


def body_value(raw, content_type, tool=None):
    """A body as stored in a recording: parsed JSON or form data (redacted), text, or base64."""
    if raw is None or raw == b"":
        return None, False
    truncated = len(raw) > MAX_BODY
    raw = raw[:MAX_BODY]
    ct = (content_type or "").lower()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return {"base64": base64.b64encode(raw).decode()}, truncated
    if "json" in ct or (text[:1] in "{[" and not truncated):
        try:
            value = _redact_value(json.loads(text))
            rule = TOOL_RULES.get(tool)
            return (rule(value) if rule else value), truncated
        except ValueError:
            pass
    if "x-www-form-urlencoded" in ct:
        pairs = urllib.parse.parse_qsl(text, keep_blank_values=True)
        return _redact_value({k: v for k, v in pairs}), truncated
    return text, truncated


def make_record(tool, started, duration_ms, method, path, query, req_headers, req_body, req_ct,
                status, resp_headers, resp_body, resp_ct):
    req_value, req_cut = body_value(req_body, req_ct, tool)
    resp_value, resp_cut = body_value(resp_body, resp_ct, tool)
    rec = {
        "recording_version": RECORDING_VERSION,
        "tool": tool,
        "started": started,
        "duration_ms": round(duration_ms, 2),
        "method": method,
        "path": path,
        "query": {k: (REDACTED if is_secret_key(k) else v) for k, v in query.items()},
        "request": {"headers": redact_headers(req_headers), "body": req_value},
        "response": {"status": status, "headers": redact_headers(resp_headers), "body": resp_value},
    }
    if req_cut or resp_cut:
        rec["truncated"] = True
    return rec


def har_to_records(har, tool_for_host):
    """A browser recording (HAR) as recording lines. `tool_for_host(host)` names the tool for a
    host, or None to leave the request out (other sites the browser talked to)."""
    out = []
    for e in har.get("log", {}).get("entries", []):
        req, resp = e.get("request", {}), e.get("response", {})
        url = urllib.parse.urlsplit(req.get("url", ""))
        tool = tool_for_host(url.hostname or "")
        if not tool:
            continue
        def headers(lst):
            return {h["name"]: h["value"] for h in lst or []}
        def har_body(text, encoding=None):
            if text is None:
                return None
            return base64.b64decode(text) if encoding == "base64" else text.encode()
        post = req.get("postData") or {}
        content = resp.get("content") or {}
        out.append(make_record(
            tool, e.get("startedDateTime"), float(e.get("time") or 0), req.get("method", "GET"), url.path,
            dict(urllib.parse.parse_qsl(url.query, keep_blank_values=True)), headers(req.get("headers")),
            har_body(post.get("text")), post.get("mimeType"), int(resp.get("status") or 0),
            headers(resp.get("headers")), har_body(content.get("text"), content.get("encoding")),
            content.get("mimeType")))
    return out
