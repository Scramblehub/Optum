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

## Each stage, in five steps

Every replaced service goes through the same steps, and moves on only when the step's checks
are green:

| Step | What happens | The company's experience |
|---|---|---|
| 1. Shadow | The owned system receives copies of the data and computes the same results; nothing uses them | no change |
| 2. Parallel | Both run; results are reconciled record by record; the old tool still counts | no change; a daily "do they agree?" report |
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

## Branches, in order

| # | Branch | Job | Size |
|---|---|---|---|
| 1 | `feat/testco` | `testco/`: Compose file for the ten stand-ins, the company's glue and scripts, the data generator and its ground truth | 1–2 weeks |
| 2 | `feat/frontend-saas` | Adapters reading each tool's schema and API into the graph (the [protocol](../protocol.md) exists): fields, endpoints in use, flows. `optum diagnose testco/` finds every planted waste and no unplanted ones | 2 weeks |
| 3 | `feat/unified-model` | One company graph: shared entities across tools; what each tool is actually used for; which to keep and why; the stage order | 1 week |
| 4 | `feat/coexist-sync` | Two-way sync between the owned system and the remaining tools, by identity; the record-matching measurement against the ground truth | 1–2 weeks |
| 5 | `feat/stage-runner` | The five steps as a harness: shadow, parallel with daily reconciliation, switch, fallback, retire; contract tests for the kept services; rollback | 1–2 weeks |
| 6 | `feat/stage-1-glue`, `feat/stage-2-...` | The owned replacements, one stage at a time | 1 week each |

**Gate v0.3:** stages 1 and 2 go through all five steps in the test company with every check
green, including one rehearsed rollback; the later stages are planned with their order and
checks; before/after numbers reported (services paid for, places a customer is kept, lines of
glue). Stages 3 to 7 continue after v0.3.

## Decisions for you

- **Docker** isn't installed on this machine (installing it needs sudo). The test company is about
  ten containers (roughly 8–12 GB of memory; this machine has 62 GB). Either you install Docker
  locally, or it runs in CI only; local is far better for building it.
- **The two kept services:** payments and accounting, or another pair.
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
