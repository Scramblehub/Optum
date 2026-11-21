# Plan: putting Optum inside a business, with permissions in place

*Draft of 2026-11-21. How Optum gets the access it needs at a real company, and no more. It
builds on [phase3.md](phase3.md) (the stages) and [capabilities.md](capabilities.md) (the
evidence model). It's a starting shape, to refine before any engagement.*

Optum needs to read a company's code, watch its traffic, read its tools' configuration and,
later, switch live systems over. Each of those is sensitive: code is intellectual property,
traffic carries customers' personal data, and switching a system can stop a business. A company
will only let Optum in if the access is **small, visible, temporary and reversible**, and if it
can check that for itself. Safety is what earns that trust, so access is designed as part of
the product, not added on.

## What Optum touches, and what that needs

| Activity | Touches | Access needed | Today in the test company |
|---|---|---|---|
| Graph, diagnose, model (static) | source code, tool exports (n8n flows) | **read** on repositories and exports | reads a folder |
| Recording traffic | requests and responses between tools: personal data, credentials | a **position on the network path**, no credentials of its own | a proxy in front of every tool; secrets redacted, personal data kept |
| Learning specs (`optum infer`) | recordings | none beyond the recordings | offline |
| Tracing (`verify`) | runs the company's code under a profiler | **execute** in a test environment | runs on a laptop |
| Fixers | changes code | **open pull requests**, never push | writes to a working copy |
| Stage harness | switches webhooks and flows, turns a service live, retires a tool | **write** on production configuration | uses Docker (which is root-equivalent on the host) |
| Owned services | live business writes (invoices) | a service account on the kept systems | the test credentials |
| What-if mirrors | copies of production data | wherever the copy lives | the test company's own volumes |

The current test setup is fine for a test company and wrong for a real one in three places:
recordings keep personal data in the clear, the stage harness needs Docker, and nothing stops a
step that should need a person's approval.

## Principles

1. **It runs inside the company.** Optum is installed in the company's own environment (their
   cloud account or servers). Recordings, models and copies of data never leave it. What
   anyone outside sees is what the company chooses to export (reports, counts).
2. **Access grows by phase and shrinks when it's done.** Most of the work needs only reading.
   Write access exists only during a stage switch, for that stage's systems.
3. **One identity per job.** No single Optum account can read everything *and* change
   anything. Losing one identity limits the damage to that job.
4. **Optum asks for exactly what it needs, in writing, first.** It generates its own access
   request from the deployment file (systems, hosts, what each phase does), and the company's
   security team approves that list, not a vague "admin access".
5. **People approve every change to production.** Optum proposes and rehearses; a named person
   approves the switch, the retirement and anything touching regulated records (two people for
   those).
6. **Everything Optum does is logged, where Optum can't edit it.**
7. **Sensitive data is reduced where it's captured,** not after it's stored.

## Phases and the access each needs

| Phase | Identity | Access | Lasts |
|---|---|---|---|
| **1. Read** | `optum-reader` | read-only on repositories, config, tool exports (flows, schemas) | the engagement |
| **2. Observe** | `optum-recorder` | a place on the network path, set up by the company's operations team (a sidecar, gateway logs or HAR exports). Holds no credentials of its own; redacts and pseudonymises as it records | weeks, then switched off |
| **3. Analyse** | `optum-analyst` | **no network access at all**: works on the read-only copies and recordings | the engagement |
| **4. Rehearse** | `optum-rehearse` | a staging environment or what-if mirror with **pseudonymised** data; may run the company's code under a tracer there, never in production | per rehearsal |
| **5. Change** | `optum-operator` | write access to *this stage's* switch points (webhook targets, flow on/off, routing) through the company's own deployment pipeline, not Docker or SSH; each action needs a recorded approval | the switch window, then revoked |
| **6. Run** | the owned service's own account | only the operations its spec lists (the stage's contract check already compares them with the old glue's) | ongoing, owned by the company |
| **7. Fix** | `optum-fixer` | opens pull requests; the company's own review and CI decide | per fix |

## Sensitive data

- **Secrets** are already redacted when recorded (keys like password, token and card number,
  plus Odoo's password position).
- **Personal data is pseudonymised when recorded.** Names, emails, addresses and phone numbers
  are replaced with a keyed hash (HMAC) under a key the company holds. The same email always
  becomes the same token, so linking still works: workflows, same-entity matching and identity
  keys only need equality, not the values. Optum never holds the key. Which fields count as
  personal data is set by a field policy the company reviews, with a safe default (anything
  matching email, name, address or phone patterns).
- **Specs and models hold no values**, only operations, field names, types, status names and
  counts. They are the low-risk outputs, and the ones meant to be shared with the people
  building replacements.
- **Retention:** recordings expire after a set period (30 days by default) and on request.
  `optum` gets a command that deletes them and proves it (a log of what was removed, by hash).
- **Regulated records are read, never written,** except by an owned service doing its job
  under the stage's contract. The fingerprint check that runs before and after every stage step
  stays mandatory.
- **Recording staff's use of a UI** (for the interface work in the builder's suite) is
  aggregated by role and never kept per person. Where the law requires it (works councils in
  much of the EU, for example) the company consults its staff first.

## On the machine

- Optum runs as its own **unprivileged user**: no sudo, no Docker group (Docker access equals
  root), in a container with a read-only root filesystem, and outbound network access limited to
  the hosts in the access request.
- **Tracing** needs no special privilege (Python's profiling hooks and Node's `--cpu-prof` run
  as an ordinary user), but it runs the company's code, so only in phase 4's environment.
- **Binaries** are read as files; Optum never runs a binary it is analysing.
- **Credentials** are short-lived tokens from the company's own secrets manager, scoped to one
  identity and one phase. Optum stores none at rest.

## Approvals, audit and the way out

- **Approvals:** the stage harness refuses `switch`, `retire` and any regulated write without an
  approval record: who approved, when, and which checks were green at the time. Fallback stays
  one step and needs no approval, because getting back to safety should never wait for anyone.
- **Audit:** every action by every Optum identity goes into an append-only log in which each
  entry includes the hash of the one before (a hash chain), shipped to the company's own log
  store, so a gap or an edit shows.
- **Kill switch:** one command revokes every Optum identity and stops the recorder; the stage
  harness falls back to the old systems.
- **Offboarding:** recordings deleted with proof, identities removed, the specs, models,
  owned services and audit log handed over. The company keeps running without Optum.

## What it takes to build

Each of these sharpens existing work rather than starting a new track.

| # | Piece | Builds on | Test |
|---|---|---|---|
| 1 | **Pseudonymisation at capture:** a keyed hash for personal data, with a field policy | the recorder's redaction | in the test company, workflows, same-entity links and the model come out the same on pseudonymised recordings as on clear ones; no email or name appears in any recording |
| 2 | **`optum access-plan`:** the exact access request per phase (identity, system, read or write, duration), generated from the deployment file and the stage plan | `deployment.json`, the model's operations | for stage 1 it lists only the switch points stage 1 changes; reviewed by hand against the stage |
| 3 | **Approvals in the stage harness** | `stage1.py`'s gates | a switch without an approval record is refused; fallback works without one |
| 4 | **A hash-chained audit log** for Optum's own actions | the owned service's audit log | removing or editing a line is detected |
| 5 | **Retention and deletion with proof** | the recordings | deletion leaves a log of hashes; nothing recoverable remains |
| 6 | **A locked-down deployment profile:** non-root, read-only filesystem, an egress allow-list, no Docker access for the operator | the test company's Compose file | the test company runs all five stage steps under it |
| 7 | **A threat model:** what an attacker gets if each identity is stolen, and how that's limited | all of the above | reviewed before the first real engagement |

Items 1 and 3 come first: personal data in recordings and unapproved switches are the two
biggest risks today. Item 6 also removes the stage harness's dependence on Docker, which a real
company would never grant.

## Decisions

- **Hosting: open, to be settled by interviewing companies.** Inside the company's environment is
  assumed here; an Optum-hosted option for small companies changes the data-protection position
  completely (Optum would hold their data). Questions for the interviews: would you run it in your
  own cloud or have it hosted; what your vendor security review requires; who would approve
  Optum's access and who approves production changes today; whether traffic from your SaaS tools
  can be recorded, and what your data-protection officer needs first.
- **Approvals: agreed.** One approver per switch; two for anything touching regulated systems;
  fallback never waits for approval.
- **Certifications (SOC 2, ISO 27001): deferred until Optum is a full product.** They certify Optum as a vendor (its access
  control, change review, incident handling), not the product, and take months and real money.
  Not needed for first pilots, and much more pressing if Optum hosts customers' data. Meanwhile
  the audit log, the per-phase access plan, reviewed changes through CI, and deletion with proof
  are built so that the evidence exists when a customer asks.
