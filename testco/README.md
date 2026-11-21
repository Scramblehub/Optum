# The test company

A small online retailer made of open-source stand-ins for the ten services a company like it pays
for, its glue, and generated data with a known answer for everything
([plan](../docs/plans/phase3.md)). Optum unifies it in stages; this is where that's tested.

```sh
docker compose -f testco/compose.yaml up -d --wait   # start (first start: ~2 min, Odoo sets up)
python3 testco/smoke.py                              # each service answers as the glue will use it
python3 testco/generate.py                           # data for every tool, and its ground truth
docker compose -f testco/compose.yaml down           # stop (add -v to delete the data too)
```

Everything listens on `127.0.0.1` only, and only through **`observe`**, the recording proxy
(mitmproxy in reverse mode): the tools publish no ports of their own, so every exchange with them
is recorded in `recordings/<tool>.jsonl` with secrets redacted (credential headers, fields named
like secrets, Odoo's positional password). That's what each tool's behaviour is learned from
(the [plan](../docs/plans/phase3.md): "Learning a tool we can't read"). Browser sessions saved as
HAR join them in the same format:

```sh
python3 testco/observe/har_import.py session.har --tool 127.0.0.1:8069=accounting -o testco/recordings/
```

## Services so far

| Service | Stand-in | Port | Fate |
|---|---|---|---|
| Payments | stripe-mock 0.197 (Stripe's mock of its API; keeps no data, answers in Stripe's shape) | 12111 | kept |
| Accounting and tax filing | Odoo 18 with Invoicing (database `testco`, login `admin` / `admin`) | 8069 | kept |
| Automation glue | n8n 1.80 | 5678 | replaced in stage 1 |
| Web shop | WooCommerce 9.4 on WordPress 6.7 (API: `admin` / `testcoShopApiPassword00`, a fixed test credential) | 8080 | replaced in stage 7 |

The other six (spreadsheets, forms, email marketing, scheduling, help desk, CRM) come next.

## The data

`generate.py` writes `data/<tool>.json` for every tool (each with its own record ids and its own
spelling of people) and `data/ground_truth.json`: which records are the same person, which are the
same order. Deterministic for a seed. The traps for record matching:

- **two different people named John Smith**, spelled exactly alike everywhere: never one person;
- **one person with two emails** (Catherine Novak: the shop and the help desk know the second);
- nicknames and dropped letters in some tools; missing phones where a tool doesn't ask.

Every order is a shop order, a payment, and an invoice in accounting. Invoices are tax documents:
`"regulated": "tax"`, never merged or rewritten.

## Loading it into the tools

```sh
python3 testco/load.py                       # through the proxy, so it's recorded (~2 min)
python3 testco/check_regulated.py snapshot   # fingerprint every posted invoice
python3 testco/check_regulated.py verify     # later: exit 1 if any tax document changed
```

- **Accounting** becomes a UK company (GBP, VAT 20% included in prices, as shop prices are), with
  its customers and 2,000 **posted** invoices; every total matches its order.
- **Shop**: 8 products, its 302 customers and 2,000 completed orders (every total matches the
  ground truth; the two John Smiths are two customers). WooCommerce is slow to load: ~8 minutes.
- **Payments** (stripe-mock keeps nothing): 300 checkout calls, as recorded traffic.
- `data/ids.json` maps each generated record to the id the live tool gave it, for checking what
  Optum finds against the ground truth. Optum never reads it, nor `ground_truth.json`.

Why `check_regulated.py`: Odoo locks a posted invoice's partner and date, but its API still lets
the total and line prices be overwritten. A tool's own locking isn't evidence that tax documents
are untouched; a fingerprint before a stage and a comparison after are.

## The company's glue

`company/` is the company's own code, the glue Optum analyses and, in stage 1, replaces:

| Part | Does |
|---|---|
| n8n flow "Orders to accounting" (every 5 minutes) | paid shop orders → an invoice in accounting (if none yet) → the order completed |
| `nightly_sync.py` | the same job, written before the flow and never switched off |
| `vip_report.py` | customers with five or more orders; copies two helpers from `nightly_sync.py` |
| `payments_webhook.js` (container `hooks`) | a "charge succeeded" event → the shop order paid |
| n8n flow "Abandoned cart reminder" | active, listening on a webhook nothing calls |

All of it calls the tools by internal names (`shop.testco.internal`, `accounting.testco.internal`,
...) that lead to the recording proxy, so every call is recorded and the same endpoint has the same
id whether n8n, Python or JS calls it.

```sh
sh testco/company/setup_glue.sh                                        # import and switch on the n8n flows
docker compose -f testco/compose.yaml exec driver python /driver/business_day.py --orders 20   # a day of trading
docker compose -f testco/compose.yaml exec company python nightly_sync.py                    # the company's nightly run
python3 testco/check_glue.py                                           # every new order: completed, one invoice
```

`driver/business_day.py` stands in for customers and Stripe (new orders, card charges, payment
events); it's kept out of `company/` because it isn't the company's code.

**What Optum finds** (`optum diagnose testco/company --adapter "python3 Core/adapters/n8n/optum_n8n.py"`,
answer key [expected_diagnosis.json](expected_diagnosis.json), checked by a ctest): the flow and the
nightly script doing the same job (overlapping glue: the same 3 endpoints), and the two copied
helpers (duplicated logic). The unused cart flow can't be seen from code; that's for the recorded
traffic.

## Learning the tools' behaviour (`optum infer`)

```sh
optum infer testco/recordings --n8n testco/company/n8n -o specs/
```

From the recordings alone it writes a behaviour spec per tool (`specs/<tool>.spec.json`,
`specs/spec.md`): its operations (method and endpoint with ids folded to `{}`, or a JSON-RPC model
and action), how often each is called, by whom, with which statuses; the fields sent and received;
the resources and their fields; and the status changes seen (shop orders: pending → processing →
completed). And `observed.json`: findings only traffic shows, so far flows that never ran (the
abandoned-cart flow: its webhook was never called).

Two things keep the recordings honest:
- **who called:** the proxy records the calling container (`automation`, `company`, `hooks`,
  `driver`), so the n8n flow and the nightly script show up separately: both create invoices;
- **our own tools aren't the company:** loading, the smoke test and the checks send an
  `X-Optum-Harness` header, recorded as `harness` and left out of the specs; the business driver
  runs in its own container (`driver`), so its calls are the customers' and Stripe's.

## Stage 1: the automation glue, replaced (`feat/stage1`, rough)

The n8n flow and the nightly script turn a paid order into a posted invoice. Stage 1 replaces
both with an owned service, `owned/orders_service.py` (the `owned` container, port 7000), that
reacts to the payment event instead of polling. It goes through the five steps:

```sh
python3 testco/stages/stage1.py shadow      # Stripe sends to the old webhook and the owned service; the owned one only writes drafts
python3 testco/stages/stage1.py reconcile   # each draft against the invoice the old glue posted
python3 testco/stages/stage1.py switch      # needs 5+ agreeing orders, none differing, every check green
python3 testco/stages/stage1.py fallback    # one step back to the old glue; the owned service keeps shadowing
python3 testco/stages/stage1.py retire      # only once switched and green: n8n flows archived, n8n and the old webhook stopped
python3 testco/stages/stage1.py check       # orders, regulated, contract (below)
python3 testco/stages/stage1.py measure     # payment to posted invoice, old glue against owned service
```

Between steps, run a business day (`docker compose -f testco/compose.yaml exec -T driver python
/driver/business_day.py --orders 8 --seed N`). The driver sends each payment event to every URL in
`data/stripe_endpoints`, which the steps rewrite: that's how the "Stripe" side is switched.

The checks, after every step:
- **orders:** every live order completed, with exactly one posted invoice of its total;
- **regulated:** the 2,000 fingerprinted tax invoices unchanged;
- **contract:** from the recordings, the owned service makes no write to accounting that the old
  glue never made, and creates invoices with the same fields (new reads are listed, not failed).

Every action the owned service takes is in `data/stage1/audit.jsonl` (what, when, before and
after); each step it takes checks first, so a retried payment event finishes the job without
doing anything twice.

**First run (2026-11-20):** shadow (8 orders, 8 agreeing), switch, a live day, fallback with a
day on the old glue, switch again, retire, a day without n8n: 84 live orders, every check green.
Payment to posted invoice: **0.25 s** median for the owned service against **96 s** for the n8n
flow (it polls every 5 minutes). The live day found a bug: invoices were created but not posted
(Odoo's `create` returns a list when given one), and a retry would have skipped them; fixed, and
the 8 orders recovered by replaying their events (they account for the owned service's 65 s
maximum).

Not yet: the owned service is written by hand from the spec, not generated; reconciling replays
recorded inputs only through live shadowing; and the platform pieces (backups, restore drill)
come with `feat/platform`.

## What could go: `optum model`

```sh
optum model testco/company --adapter "python3 Core/adapters/n8n/optum_n8n.py" \
  --recordings testco/recordings --n8n testco/company/n8n --deployment testco/deployment.json
```

From the company's code (static facts), the recordings (observed facts) and
[deployment.json](deployment.json) (what runs where: declared facts), it works out what each
component does to which data, and classes it, with the reasons:

| Component | Class | Why |
|---|---|---|
| payments webhook | essential | only it marks orders paid (sets status processing) |
| abandoned-cart flow | independently removable | never ran in the recorded window; nothing depends on it |
| orders-to-accounting flow | independently removable | the nightly script makes every write it makes, with the same invoice fields; *mutual*, so keep one of the two |
| nightly script | independently removable | the same, the other way round |
| `common.py` | not a component | shared code; reaches no system itself |
| VIP report | unknown | it only reads; whether anyone uses what it prints can't be seen |
| accounting (system) | essential | holds the regulated tax invoices |

Each conclusion names its evidence and its weakest kind (a conclusion that needed the declared
deployment is never more certain than "declared"), keeps the four kinds of equivalence apart
(functional, data, behavioural: not claimed without a replay, operational), and lists the inputs
it rests on by content hash: `optum model --check model.json` shows which conclusions went stale
when a file changed. The answer key is checked by a ctest (`testco_model`) on hand-made recordings.
