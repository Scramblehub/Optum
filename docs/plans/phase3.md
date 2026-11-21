# Plan: Phase 3, unifying a company's tools in stages

*Draft of 2026-11-19, for review before anything is built. Refines the Phase 3 rows of the
[roadmap](../ROADMAP.md).*

## The goal

A company pays for many SaaS tools, keeps the same customers in several of them, and glues them
together with automations and scripts. Some of those tools can't be replaced (payments,
accounting and tax filing); the rest can, by a system the company owns.

Phase 3 proves Optum can do that **in stages, gently**: one service at a time, with the old tool
kept until the new one has earned trust, every step reversible, nothing breaking for the people
who use them, and the services that stay integrated throughout.

To do it without a real company's systems, we build one: **the test company**.

## The test company: ten services, two kept

A generic small online retailer to start (the interviews will say what to swap in). Real SaaS
can't run locally, so each gets an open-source stand-in with the same role and a similar API:

| # | Role (real-world example) | Stand-in | Fate |
|---|---|---|---|
| 1 | Payments (Stripe) | **stripe-mock** (Stripe's own mock of its API) | **kept:** integrated |
| 2 | Accounting and tax filing (QuickBooks) | **Odoo** accounting | **kept:** regulated, the accountant files from it |
| 3 | Automation glue (Zapier) | n8n | replaced, stage 1 |
| 4 | Shared spreadsheets (Google Sheets) | a sheet-like store | replaced, stage 2 |
| 5 | Forms (Typeform) | Formbricks | replaced, stage 2 |
| 6 | Email marketing (Mailchimp) | Listmonk | replaced, stage 3 |
| 7 | Scheduling (Calendly) | Cal.com | replaced, stage 4 |
| 8 | Help desk (Zendesk) | Zammad | replaced, stage 5 |
| 9 | CRM (HubSpot) | EspoCRM or Twenty | replaced, stage 6 |
| 10 | Shop (Shopify) | WooCommerce | replaced, stage 7 |

Why these two are kept is something Optum should explain, not assume: payments touch card data
and banks; accounting holds tax documents (regulated: never merged or rewritten) and is where the
accountant files. Both match the rules already in [diagnose.md](../diagnose.md).

"Replaced" means replacing **what this company actually uses** of a tool: the records it keeps,
the actions its staff and glue perform, the flows that run. Optum reads that from the graph (the
endpoints, fields and flows in use), so each replacement stays small.

The sprawl is planted, with an answer key, as in Phase 2: orders copied to accounting twice (an
automation and a script), a customer kept in five tools, two different customers named John
Smith and one customer with two emails, an automation nobody triggers, the same logic in two
scripts, tax invoices that must never be merged. A data generator (about 500 customers, 2,000
orders, tickets, form answers, bookings) writes a **ground truth**: which records are the same
person or order.

## Learning a tool we can't read

Most of these tools are proprietary: their code can't be read, only their behaviour seen. So
Optum learns each tool from outside, the way [REA](https://github.com/morluto/rea) works from an
app's behaviour down to its binaries: every conclusion with its evidence. What can be seen, most
reliable first:

1. **Its published contract:** API docs, OpenAPI specs, webhook payloads, schema endpoints
   (Odoo's `fields_get`), data exports.
2. **Its observed input and output:** a recording proxy between the company and each tool, from
   the shadow step on, captures every request, response and webhook (the company's own glue and
   staff browsers make that traffic), plus browser recordings (HAR files). From recorded pairs:
   the data model (fields, types, allowed values, what's required), the operations (what each
   call changes: data before and after), state machines (a ticket's open → pending → solved), and
   derived values (`total = subtotal × (1 + rate)`, checked on every example).
3. **Its client code:** the web app's JavaScript bundle, a desktop or mobile app, read with
   Optum's JS and binary frontends: endpoints, fields, client-side validation.

From these, Optum writes a **behaviour spec** per tool (entities, operations, states, rules), each
part with its confidence and evidence, as the graph has. The owned replacement is built from the
spec, and the parallel step becomes **differential testing**: recorded inputs replayed against
both, outputs compared, every mismatch fed back. A stage switches only when the replay agrees.

**What can't be learned says what to keep.** A hidden algorithm (a fraud score, a spam filter)
or value that is a network rather than software (card rails, an email sender's reputation, a tax
authority) can't be reproduced from examples. So "keep or replace" is measured: how much of the
tool's used behaviour the spec explains, judged by the replay.

**The test company plays fair:** our stand-ins are open source, but Optum may not use their
source. Only their APIs, exports, recorded traffic and client bundles count, as with a real
SaaS.

**Within the rules:** the company's own traffic, its data, published APIs and exports are its to
use. Decompiling a vendor's client or getting around a protection can break its terms, or
anti-circumvention law in some countries; client analysis is opted into per tool, never assumed.

## What we can't see: filled by standards

A spec covers what can be observed. How a tool stores data, backs it up or undoes a mistake can't
be seen, and those are what matter when something goes wrong. The owned system doesn't guess at
them: it fills them with explicit standards, the same for every replaced tool, each proved by a
drill in the test company.

| Can't see | The standard | Proved by |
|---|---|---|
| How data is stored | one transactional database (Postgres); every write a transaction | kill the process mid-write: no partial record, nothing lost |
| Backups | nightly backups and point-in-time recovery | restore to a minute ago and reconcile with the ground truth |
| Undoing a mistake | an append-only audit log of every change (who, what, when, before and after); any change revertible | revert one bad bulk edit: only it changes |
| Rolling back a stage | migrations that run both ways; the old tool kept in sync in the fallback window | the switch-back rehearsed in every stage |
| Retried or repeated calls | idempotency keys on writes; retries with backoff towards the kept services | the same webhook twice: one charge, one invoice |
| Who may do what | roles from those observed in the old tool, confirmed by the company | the access matrix, tested per role |
| How long data is kept | retention per record type; tax documents kept the legal period (6 years in the UK) and never altered | a retention check; regulated records byte for byte unchanged |
| Personal data | export and erase per person (GDPR), except records the law says to keep | export and erase a test person; their invoices kept, anonymised where allowed |
| Security | encryption at rest and in transit, secrets out of code, least privilege | a configuration check and a secrets scan |
| Knowing it works | health checks, structured logs, alerts on a failed sync or on drift | break a sync: an alert within minutes |
| Changes over time | versioned schemas and migrations; an old record always readable | upgrade and downgrade on a copy of the data |

**A hidden behaviour that matters to people is observed or asked, never guessed.** An "undo send"
window or a 30-day trash shows in the traffic; the spec picks it up. What only the company knows
("we never delete tickets") comes from a short questionnaire per stage, recorded in the spec with
the company as its evidence.

## What it costs

Replacing a tool is only worth it if owning it costs less, everything counted, and owning is easy
to undercount: hosting, backups, security and, above all, the people who keep it running. So
cost is measured in every stage, and it can make a tool a **keep** just as a hidden algorithm can.

Counted per stage, over several years:

| Side | Costs | How the number is got |
|---|---|---|
| The SaaS today | subscription (seats, tier), usage overages, add-ons, integration tools (Zapier tasks), staff time its quirks cost | the company's bills; the observed usage (seats, calls, records) |
| Owning: running | compute, database, storage and its growth, backups (copies kept for the retention period), data transfer, monitoring and logs | **measured in the test company**: each replacement's CPU, memory, storage per 1,000 records, backup size; priced at a real host's rates |
| Owning: securing | encryption, secrets, patching, vulnerability scans, access reviews, audits (GDPR records; SOC 2 if the company needs it) | tooling costs, and hours per month |
| Owning: people | maintenance, upgrades, on-call, staff support | hours per month per service: an assumption the company sets |
| One-time | building or generating it, migrating, the parallel period, training | the stage's actual effort |
| Risk | an outage or data loss, times its likelihood | the drills' results; the company's cost of an hour of downtime |

Each stage has a **business case before it starts and a measured one after**: the yearly cost of
each side, break-even, and what would flip the decision (staff hours doubling, data growing ten
times). A tool whose owned cost doesn't beat the SaaS by a margin is kept, and the report says why.

Shared costs are counted as shared: eight tools on one platform means one database, one backup
system, one monitoring setup, so each stage costs less than the last; the model shows that
instead of pricing each tool as if it stood alone. And people are the largest risk: for a small
company, a few hours a month of upkeep can outweigh a $50 subscription, an honest reason to keep
a cheap tool or group it into another stage.

## Each stage, in five steps

Every replaced service goes through the same steps, and moves on only when the step's checks
are green:

| Step | What happens | The company's experience |
|---|---|---|
| 1. Shadow | Traffic to the tool is recorded and its behaviour spec learned; the owned system receives copies of the data and computes the same results; nothing uses them | no change |
| 2. Parallel | Both run; recorded inputs replayed against both, results reconciled record by record; the old tool still counts | no change; a daily "do they agree?" report |
| 3. Switch | The owned system counts; the old tool is kept in sync as a fallback | staff move over, for this one service |
| 4. Fallback window | The old tool is read-only, still synced; switching back is one step | a safety net while people adjust |
| 5. Retire | Data exported and archived, the subscription cancelled | one bill less |

While stages are under way, the owned system and the remaining tools **coexist**: Optum keeps
them in sync both ways, linking records by id or a temporary id (the record matching from
Phase 2's rules, used every day, not once).

**Order: least disruptive first.** The automation glue (staff never see it), then tools only the
glue touches, then tools that send things out, then the tools staff use all day (help desk, CRM,
shop) last, when everything under them is proven.

## What "without breaking anything" means

Checked in every stage:

1. **The kept services see the same traffic.** Every call to payments and accounting is recorded
   before; after the switch, the owned system makes the same calls with equivalent payloads
   (contract tests).
2. **Outcomes reconcile.** Old and new run on the same generated orders; compared record by
   record (each order paid, invoiced, confirmed).
3. **Data moves by identity.** Every record lands once, linked to the ground truth by id or
   temporary id; the two John Smiths stay two people (≥ 99% link precision, recall reported).
4. **Regulated records are untouched.** Tax invoices in accounting are byte for byte unchanged.

And what "easy on the company" means, in numbers per stage: **zero downtime**; **exact
agreement** before each switch; a **rollback rehearsed**, not just planned; the **staff steps
that change** (which screens, for which roles), few and stated in advance; and **subscriptions
retired**.

## Rehearsing iffy choices: the what-if layer

Optum makes choices it isn't sure of: an inferred call edge, a merge, two records linked as one
customer, a stage switch. Today three tools rehearse their own: the merge fixer runs the tests and
restores on failure, a stage's shadow step computes without writing, and `verify` checks inferred
edges against a real run. The what-if layer makes that one mechanism for every uncertain choice:

1. **Decide.** A choice is flagged when its confidence is low, it's inferred rather than exact, or
   it changes data or code.
2. **Mirror.** It's applied to a copy: a worktree for code; for a running company, a second copy
   of the test company (or the customer's staging) started from a snapshot of the data.
3. **Replay.** The copy and the untouched original get the same inputs: recorded traffic, the
   tests, generated business days.
4. **Compare.** Outputs record by record, and the checks (regulated records, the contract,
   invariants such as one invoice per order).
5. **Verdict, with evidence.** Confirmed, rejected (with the first difference), or **not covered**
   when the replay never reached what the choice changed. A mirror shows a choice broke nothing on
   the inputs it saw, not that it's right, so every verdict says how much of the change was
   exercised.

What it gives: safety a customer can see (every iffy choice rehearsed first), and labelled
examples for the plateau loop (each confirmed or rejected choice shows where Optum's confidence is
off). Cost: code mirrors are cheap; company mirrors take minutes and double the memory, so they're
for stage-sized decisions, and small graph choices are batched.

## Branches, in order

| # | Branch | Job | Size |
|---|---|---|---|
| 1 | `feat/testco` | `testco/`: Compose file for the ten stand-ins, the company's glue and scripts, the data generator and its ground truth | 1–2 weeks |
| 2 | `feat/observe` | A recording proxy in front of each stand-in (requests, responses, webhooks) and HAR import; recordings stored as fixtures | 1 week |
| 3 | `feat/infer-spec` | The behaviour spec per tool from published contracts, recordings and exports (and client bundles where opted in): entities, operations, states, rules, with evidence; checked against what each stand-in really does, **without reading its source** | 2 weeks |
| 4 | `feat/frontend-saas` | Adapters reading each tool's schema and API into the graph (the [protocol](../protocol.md) exists): fields, endpoints in use, flows. `optum diagnose testco/` finds every planted waste and no unplanted ones | 2 weeks |
| 5 | `feat/unified-model` | One company graph: shared entities across tools; what each tool is actually used for; which to keep and why; the stage order | 1 week |
| 6 | `feat/coexist-sync` | Two-way sync between the owned system and the remaining tools, by identity; the record-matching measurement against the ground truth | 1–2 weeks |
| 6b | `feat/what-if` | The what-if layer above: one interface for rehearsing an uncertain choice on a mirror (code worktree or a snapshot copy of the company), replay, compare, a verdict with coverage. Folds in the merge fixer's test gate, a stage's shadow step and `verify`; the platform's restore and rollback drills build on it | 1–2 weeks |
| 7 | `feat/platform` | The standards filled in for what can't be seen (storage, backups, audit log and revert, idempotency, roles, retention, GDPR, security, monitoring, migrations) and their drills | 2 weeks |
| 7b | `feat/cost-model` | Resource measurement per replacement in the test company (CPU, memory, storage per 1,000 records, backups); the cost model (both sides, shared costs, one-time, risk) and each stage's business case, before and after | 1 week |
| 8 | `feat/stage-runner` (first rough version for stage 1: `testco/stages/stage1.py`, all five steps run green with a rehearsed fallback) | The five steps as a harness: shadow, parallel with replay comparison and daily reconciliation, switch, fallback, retire; contract tests for the kept services; rollback | 1–2 weeks |
| 9 | `feat/stage-1-glue`, `feat/stage-2-...` | The owned replacements, built from each spec on the platform, one stage at a time | 1 week each |

**Gate v0.3:** stages 1 and 2 go through all five steps in the test company with every check
green, including one rehearsed rollback; their specs learned without reading the stand-ins'
source, and the replay agreeing; the platform drills passing; the measured cost of each stage set
against the SaaS it replaces, with the decision it supports; the later stages are planned with their order and
checks; before/after numbers reported (services paid for, places a customer is kept, lines of
glue). Stages 3 to 7 continue after v0.3.

## Decisions for you

- **Docker** isn't installed on this machine (installing it needs sudo). The test company is about
  ten containers (roughly 8–12 GB of memory; this machine has 62 GB). Either you install Docker
  locally, or it runs in CI only; local is far better for building it.
- **The two kept services:** payments and accounting, or another pair.
- **Cost assumptions:** staff hours per service and the company's cost of an hour of downtime are
  assumptions in the model; the interviews should give real ones, and a hosting provider to price
  against.
- **The stand-ins:** the ones above unless the interviews point elsewhere; swapping one later
  costs only its adapter.

## Risks

| Risk | What to do |
|---|---|
| Ten tools make the test company slow and fragile | Pinned images; a smoke test that starts them; most work against recorded API responses, not live tools |
| The two-way sync loops or conflicts (an edit in both places) | Every record has one owner per stage (the old tool, then the new); the other side follows; conflicts logged, never silently resolved |
| "Replacing a tool" grows into rebuilding a product | Only what this company uses, read from the graph; anything unused isn't rebuilt |
| Generated data is too clean | Plant the messiness on purpose (typos, two emails, missing fields, shared names); later the interviewed company's patterns |
| A stage looks fine on generated data but wouldn't with people | The staff-steps list per stage, and the interviews, before anything runs at a real company |
