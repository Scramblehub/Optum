# Plan: Phase 3, unifying a company's tools

*Draft of 2026-11-19, for review before anything is built. Refines the Phase 3 rows of the
[roadmap](../ROADMAP.md).*

## The goal

Phases 1 and 2 work on code. A company's sprawl is mostly not its own code: it's the SaaS tools
it pays for, the data each one keeps about the same customers and orders, and the glue (n8n
flows, scripts) moving data between them. Phase 3 proves Optum on that: it reads a company's
tools, finds the waste across them, and replaces one flow with a small service the company owns,
run side by side with the old one until the results match record by record.

To do that without a real company's systems, we build one: **the test company**, open-source
stand-ins for a small business's tools, its glue, and generated data with known answers.

## The test company: a small online retailer

A generic small retailer to start (the interviews will say what to swap in). Its tools:

| Role | Stand-in | Why this one | What it holds |
|---|---|---|---|
| Web shop | **WooCommerce** (WordPress + MySQL) | the most common small-shop platform; REST API with a schema | customers, orders, products |
| ERP and invoicing | **Odoo** Community (Postgres) | the usual open-source ERP; models and fields readable over its API | contacts, sales orders, invoices (tax documents: regulated) |
| Help desk | **Zammad** (Postgres) | full REST API, free; customers are stored again here | customers, tickets |
| Glue | **n8n** | Optum already reads n8n workflows | the sync flows |
| The company's own code | a few Python and JS scripts | how real companies patch gaps | a nightly sync, a webhook, a report |

The sprawl is **planted**, so each finding has an answer key:

| Planted | Optum should report |
|---|---|
| Orders copied from the shop to Odoo twice: by an n8n flow *and* by a nightly Python script | overlapping glue |
| A customer kept in four places: shop, Odoo, Zammad, and the scripts' own table | the same data in several places, matched by email (not by name) |
| Two different customers named John Smith; one customer with two emails | record matching links by id or temporary id, never by name |
| An n8n flow nobody triggers; a script function nothing calls | unused |
| The same address-cleaning logic in the Python script and the JS webhook | duplicated logic |
| Odoo invoices (tax documents) | never proposed for merging |

## Branches, in order

| # | Branch | Job | Size | Gate |
|---|---|---|---|---|
| 1 | `feat/testco` | `testco/` with a Compose file for the four tools, the company's glue and scripts, and a data generator: about 500 customers and 2,000 orders with planted overlaps, plus a ground-truth file (which records are the same person) | 1–2 weeks | One command brings it up, loads the data, and the glue runs (orders reach Odoo, tickets reach Zammad) |
| 2 | `feat/frontend-saas` | Adapters reading each tool's schema and API into the graph (the [protocol](../protocol.md) already exists): WooCommerce's REST schema, Odoo's models (`fields_get`), Zammad's object attributes. Their APIs get the same `http:` ids the glue code calls, so tools and code meet | 1–2 weeks | `optum diagnose testco/` reports every planted finding above, and no unplanted ones (an answer key, as in Phase 2) |
| 3 | `feat/unified-model` | One company graph: shared entities (customer, order) across tools, and one flow traced end to end (order to invoice). **Record matching** on the generated data: links by shared id or key, or a temporary id while comparing | 1 week | Record links against the ground truth: ≥ 99% precision (no two John Smiths merged), recall reported |
| 4 | `feat/generate-one-flow` | Generate a small owned service replacing the glue for order-to-invoice (both the n8n flow and the script) | 1–2 weeks | It runs and handles the generated orders |
| 5 | `feat/parallel-run` | Run old and new side by side on the same orders; reconcile Odoo's results record by record | 1 week | **Gate v0.3:** exact reconciliation, and before/after numbers (tools, flows, lines of glue, places a customer is kept) |

## Decisions for you

- **Docker isn't installed on this machine** (installing it needs sudo). The test company needs
  Docker or Podman. Either you install it, or I write the Compose file now and it runs in CI only.
  Locally is far better for building it.
- **The tools.** WooCommerce, Odoo, Zammad and n8n, unless the interviews point elsewhere. Swapping
  one later costs only its adapter.
- **Size.** The four tools need about 6–8 GB of memory together; this machine has 62 GB.

## Risks

| Risk | What to do |
|---|---|
| Heavy tools make `testco` slow to start and fragile in CI | Pin image versions; a smoke test that starts them once; most work against recorded API responses (fixtures), not live tools |
| Adapters for each tool's API become a long tail | Schemas and the endpoints the glue uses only, not the whole API |
| Generated data is too clean to be realistic | Plant the messiness on purpose: typos, two emails, missing fields, the same name for different people; and replace it with the interviewed company's patterns later |
| The "owned service" grows into a product | One flow only, generated and small; the parallel run decides whether it's right |
