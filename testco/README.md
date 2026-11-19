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
