#!/usr/bin/env python3
"""Stage 1 of the test company's unification: the automation glue, replaced by the owned orders
service (docs/plans/phase3.md, "Each stage, in five steps"). Rough: it exists before it's good.

    python3 testco/stages/stage1.py shadow      # owned service computes, writes nothing; old glue counts
    python3 testco/stages/stage1.py reconcile   # its drafts against what the old glue did
    python3 testco/stages/stage1.py switch      # old glue off, owned service live (needs a clean reconcile)
    python3 testco/stages/stage1.py fallback    # back to the old glue (the rehearsed switch-back)
    python3 testco/stages/stage1.py retire      # old glue archived and stopped
    python3 testco/stages/stage1.py check       # the checks every step must pass
    python3 testco/stages/stage1.py measure     # payment to posted invoice: the old glue against the owned service
    python3 testco/stages/stage1.py status

Checks (exit 1 on any failure):
  orders      every live order completed, with exactly one posted invoice of its total
  regulated   the fingerprinted tax documents unchanged
  contract    the owned service uses accounting the way the old glue did: no write the old glue never
              made, the same fields when it creates an invoice (new reads are listed, not failed;
              kept-service traffic, from the recordings)
Run from the repo root, with the test company up. State: testco/data/stage1/.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTCO = os.path.dirname(HERE)
ROOT = os.path.dirname(TESTCO)
DATA = os.path.join(TESTCO, "data")
STATE = os.path.join(DATA, "stage1")
sys.path.insert(0, TESTCO)

OLD_HOOK = "http://hooks.testco.internal:3000/stripe-events"
OWNED_HOOK = "http://owned.testco.internal:7000/stripe-events"
OLD_FLOW = "orders-to-accounting"


def compose(*args):
    return subprocess.run(["docker", "compose", "-f", os.path.join(TESTCO, "compose.yaml"), *args],
                          capture_output=True, text=True)


def set_endpoints(*urls):
    open(os.path.join(DATA, "stripe_endpoints"), "w").write("\n".join(urls) + "\n")


def set_mode(mode):
    os.makedirs(STATE, exist_ok=True)
    open(os.path.join(STATE, "mode"), "w").write(mode + "\n")


def n8n_flow(active):
    out = compose("exec", "-T", "automation", "n8n", "update:workflow", f"--id={OLD_FLOW}", f"--active={str(active).lower()}")
    if out.returncode:
        sys.exit(f"n8n: {out.stderr[-300:]}")
    compose("restart", "automation")
    compose("up", "-d", "--wait", "automation")


def record(step, **info):
    path = os.path.join(STATE, "state.json")
    state = json.load(open(path)) if os.path.exists(path) else {"steps": []}
    import datetime
    state["steps"].append({"step": step, "at": datetime.datetime.now().isoformat(timespec="seconds"), **info})
    state["current"] = step
    json.dump(state, open(path, "w"), indent=1)


# ------------------------------------------------------------------------------------- checks

def check_orders():
    r = subprocess.run([sys.executable, os.path.join(TESTCO, "check_glue.py")], capture_output=True, text=True)
    return r.returncode == 0, r.stdout.strip().splitlines()[0] if r.stdout else r.stderr[-200:]


def check_regulated():
    r = subprocess.run([sys.executable, os.path.join(TESTCO, "check_regulated.py"), "verify"], capture_output=True, text=True)
    return r.returncode == 0, (r.stdout or r.stderr).strip()


def optum_binary():
    """The optum binary: $OPTUM, or the newest build under build/."""
    if os.environ.get("OPTUM"):
        return os.environ["OPTUM"]
    import glob
    found = [p for p in glob.glob(os.path.join(ROOT, "build", "*", "Core", "optum")) if os.access(p, os.X_OK)]
    if not found:
        sys.exit("stage1: no optum binary (build one: cmake --build --preset release, or set $OPTUM)")
    return max(found, key=os.path.getmtime)


def accounting_spec(*clients):
    """What these clients do in accounting, learned from the recordings by `optum infer`."""
    import tempfile
    with tempfile.TemporaryDirectory() as out:
        args = [optum_binary(), "infer", os.path.join(TESTCO, "recordings"), "--tool", "accounting", "-q", "-o", out]
        for c in clients:
            args += ["--client", c]
        subprocess.run(args, check=True, capture_output=True)
        path = os.path.join(out, "accounting.spec.json")
        return json.load(open(path))["operations"] if os.path.exists(path) else {}


def check_contract():
    """The owned service calls accounting with the operations and invoice fields the old glue used."""
    new_ops = accounting_spec("owned")
    if not new_ops:
        return True, "the owned service hasn't called accounting yet"
    old_ops = accounting_spec("automation", "company")
    READS = ("search", "search_read", "search_count", "read", "fields_get", "name_search")
    extra = set(new_ops) - set(old_ops) - {"rpc res.partner.create"}  # creating a missing customer: the script did too
    new_reads = sorted(op for op in extra if op.rsplit(".", 1)[-1] in READS)  # reading more changes nothing
    extra = sorted(extra - set(new_reads))
    create = "rpc account.move.create"
    fields_old = set(old_ops.get(create, {}).get("request_fields", {}))
    fields_new = set(new_ops.get(create, {}).get("request_fields", {}))
    problems = []
    if extra:
        problems.append(f"writes the old glue never made: {extra}")
    if create in new_ops and fields_new != fields_old:
        problems.append(f"invoice fields differ: only new {sorted(fields_new - fields_old)}, only old {sorted(fields_old - fields_new)}")
    reads = f"; new reads only: {new_reads}" if new_reads else ""
    return not problems, "; ".join(problems) or (f"the same writes and invoice fields ({len(fields_new)}) as the old glue"
                                                 + reads)


def check():
    results = {"orders": check_orders(), "regulated": check_regulated(), "contract": check_contract()}
    for name, (ok, detail) in results.items():
        print(f"{'ok  ' if ok else 'FAIL'}  {name:9} {detail}")
    return all(ok for ok, _ in results.values())


def reconcile():
    """The owned service's shadow drafts against the invoices the old glue really posted."""
    from load import Odoo
    odoo = Odoo()
    agree, differ, waiting = 0, [], 0
    folder = os.path.join(STATE, "shadow")
    for f in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        draft = json.load(open(os.path.join(folder, f)))
        want = draft["invoice"]
        moves = odoo("account.move", "search_read", [["ref", "=", want["ref"]]],
                     fields=["amount_total", "partner_id", "invoice_line_ids", "state"])
        if not moves:
            waiting += 1  # the old glue hasn't invoiced it yet
            continue
        m = moves[0]
        lines = odoo("account.move.line", "read", m["invoice_line_ids"], fields=["name", "quantity", "price_unit"])
        partner = odoo("res.partner", "read", [m["partner_id"][0]], fields=["email"])[0]["email"]
        got = sorted((ln["name"], ln["quantity"], round(ln["price_unit"], 2)) for ln in lines)
        exp = sorted((ln["name"], ln["quantity"], round(ln["price_unit"], 2)) for ln in want["lines"])
        same = (len(moves) == 1 and round(m["amount_total"], 2) == round(want["total"], 2) and got == exp
                and (partner or "").lower() == want["partner_email"])
        if same:
            agree += 1
        else:
            differ.append({"order": draft["order"], "old": {"total": m["amount_total"], "lines": got, "email": partner,
                                                            "invoices": len(moves)}, "owned": want})
    print(f"reconcile: {agree} agree, {len(differ)} differ, {waiting} not yet invoiced by the old glue")
    for d in differ[:10]:
        print("  differ:", json.dumps(d)[:300])
    return agree, differ


def measure():
    """Seconds from a customer's payment to its posted invoice, old glue against owned service.
    Old glue: the payment event at the old webhook to the automation tool creating the invoice (recordings).
    Owned: the order marked paid to the invoice posted (its audit log, live mode)."""
    import datetime
    import statistics

    def at(s):
        return datetime.datetime.fromisoformat(s.replace("Z", "+00:00"))

    paid, owned = {}, []
    for line in open(os.path.join(STATE, "audit.jsonl")):
        r = json.loads(line)
        if r["mode"] == "live" and r["event"] == "order_paid":
            paid[r["order"]] = at(r["at"])
        elif r["mode"] == "live" and r["event"] == "invoice_posted" and r["order"] in paid:
            owned.append((at(r["at"]) - paid.pop(r["order"])).total_seconds())
    pay, old = {}, []
    for line in open(os.path.join(TESTCO, "recordings", "hooks.jsonl")):
        r = json.loads(line)
        try:
            pay.setdefault(str(r["request"]["body"]["data"]["object"]["metadata"]["order_id"]), at(r["started"]))
        except (KeyError, TypeError):
            continue
    for line in open(os.path.join(TESTCO, "recordings", "accounting.jsonl")):
        r = json.loads(line)
        args = ((r["request"].get("body") or {}).get("params") or {}).get("args") or []
        if r.get("client") == "automation" and len(args) > 5 and args[3:5] == ["account.move", "create"]:
            vals = args[5][0][0] if isinstance(args[5][0], list) else args[5][0]
            order = str(vals.get("ref", "")).replace("order ", "")
            if order in pay:
                old.append((at(r["started"]) - pay[order]).total_seconds())
    for name, xs in (("old glue", old), ("owned service", owned)):
        print(f"{name:13} " + (f"{len(xs)} orders, median {statistics.median(xs):.2f} s, max {max(xs):.2f} s" if xs
                               else "no orders measured"))
    return old, owned


# -------------------------------------------------------------------------------------- steps

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    step = sys.argv[1]
    os.makedirs(STATE, exist_ok=True)
    if step == "shadow":
        set_mode("shadow")
        set_endpoints(OLD_HOOK, OWNED_HOOK)  # Stripe sends to both; the old glue still counts
        n8n_flow(True)
        record("shadow")
        print("shadow: the owned service gets every payment event and only writes drafts; the old glue counts")
    elif step == "reconcile":
        agree, differ = reconcile()
        record("reconcile", agree=agree, differ=len(differ))
        sys.exit(1 if differ or agree == 0 else 0)
    elif step == "switch":
        agree, differ = reconcile()
        if differ or agree < 5:
            sys.exit(f"not switching: needs 5+ agreeing orders and none differing (have {agree}, {len(differ)} differing)")
        if not check():
            sys.exit("not switching: a check fails before the switch")
        set_endpoints(OWNED_HOOK)
        set_mode("live")
        n8n_flow(False)  # the nightly script stays unscheduled: it's run by hand, and no longer is
        record("switch", agreed=agree)
        print("switch: the owned service is live; the n8n flow is off, Stripe sends to the owned service only")
    elif step == "fallback":
        set_endpoints(OLD_HOOK, OWNED_HOOK)  # the owned service keeps shadowing: a new switch needs fresh agreement
        set_mode("shadow")
        n8n_flow(True)
        record("fallback")
        print("fallback: the old glue counts again (webhook and n8n flow on); the owned service is back to shadowing")
    elif step == "retire":
        state = json.load(open(os.path.join(STATE, "state.json")))
        if state.get("current") != "switch":
            sys.exit("not retiring: the stage must be switched (and its checks green) first")
        if not check():
            sys.exit("not retiring: a check fails")
        archive = os.path.join(STATE, "archive")
        os.makedirs(archive, exist_ok=True)
        out = compose("exec", "-T", "automation", "n8n", "export:workflow", "--all")
        open(os.path.join(archive, "n8n-workflows.json"), "w").write(out.stdout[out.stdout.find("["):])
        compose("stop", "automation", "hooks")
        record("retire", archived="archive/n8n-workflows.json")
        print("retire: the n8n flows are archived and the automation tool and old webhook stopped")
    elif step == "check":
        sys.exit(0 if check() else 1)
    elif step == "measure":
        measure()
    elif step == "status":
        print(json.dumps(json.load(open(os.path.join(STATE, "state.json"))), indent=1))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
