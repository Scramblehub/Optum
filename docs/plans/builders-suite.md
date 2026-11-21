# Plan: the builder's suite and Optum's console

*Draft of 2026-11-20, for review. Builds on the [Phase 3 plan](phase3.md); a direction still being
refined, so this is a starting shape, not a commitment.*

## Who this is for

Optum learns a company's tools and generates their replacements (the behaviour spec, built on the
platform standards: [phase3.md](phase3.md)). What it deliberately doesn't do is the part people
see and use: the screens. Those need the company: its staff, its habits, its brand. So a person
builds them, working with the company. Two kinds of front end follow, for different people:

| | For | Job |
|---|---|---|
| **The builder's suite** | the person building a replacement's UI with the company | everything they need to build the screens without reverse engineering anything themselves |
| **Optum's console** | the architect running a unification | answers to the questions a staged migration raises: what talks to what, where the data lives, how each stage is going, what it costs |

Neither is a general dashboard. Each view exists because it answers a specific question someone
in that role asks; anything that doesn't is left out.

## 1. The builder's suite

What Optum hands the UI builder for each replacement, generated from what it learned. The rule:
the builder never has to guess how the back end behaves, and never builds a screen nobody needs.

| Part | What it is | Generated from |
|---|---|---|
| **What's used today** | the screens and actions staff actually use in the old tool, by role and how often; the ones never used | recorded traffic and browser sessions (HAR) |
| **Task maps** | a staff task as it is today, across tools ("refund an order": 5 steps in 3 tools), and the steps the new system removes | the recordings, the company graph |
| **The API contract** | an OpenAPI description of the replacement, and a typed client (TypeScript first) | the behaviour spec |
| **Rules the UI must respect** | required fields, allowed values, validation, and **state machines** (which actions are possible in which state: the buttons a screen shows) | the behaviour spec |
| **Who may do what** | the roles and the actions each may take | the platform's access matrix |
| **A mock back end** | a local server that behaves like the replacement, with realistic data (the test company's, or anonymised from the company) | the spec and the generator |
| **Errors and edge cases** | every error the back end can return, with when it happens; the awkward cases from real data (missing fields, two people with one name) | the spec, the recordings |
| **A starting scaffold** (optional) | plain generated screens for every entity and action (lists, forms, detail views), to refine or throw away; never the final design | the spec |
| **UI acceptance checks** | the UI only calls what the contract allows; replaying recorded staff sessions against the new back end; an accessibility baseline | the contract, the recordings |

Per stage, the suite also carries the **change brief** the Phase 3 plan already asks for: which
screens and steps change for which roles, stated before the stage switches.

Built in this order, because each needs the one before: what's used today and task maps (they need
only the recordings, which exist now), then the contract, rules, roles and mock back end (they need
`feat/infer-spec` and `feat/platform`), then the scaffold and the acceptance checks.

## 2. Optum's console

Today Optum's front end is its command line, JSON reports and graph drawings (SVG, DOT, Mermaid).
They work, but a whole-system graph is a hairball, and a staged migration needs watching over weeks.
The console is a local web app over the same JSON Optum already writes; the command line stays the
way to automate.

Its views, each built around one question:

| View | Answers |
|---|---|
| **System map** | What tools does the company run, and what moves data between them? Tools and flows at the level of a whiteboard sketch; drill into one flow for the code and steps behind it |
| **Where the data lives** | Where is a customer (an order, a product) kept, under which names and keys, and which of those copies agree? |
| **Findings** | What's wasted, how sure Optum is, and what it would take to fix: the diagnose findings as a list to work through |
| **Stage board** | Where is each stage in its five steps, and which checks pass? |
| **Agreement and drift** | Do old and new still agree? The replay and reconciliation results over time, and every disagreement |
| **Sync and safety** | Is the two-way sync healthy? Are the tax documents untouched? Did a drill pass? Alerts when not |
| **Cost** | What does each stage cost against the SaaS it replaces, measured, and what would flip the decision? |

Design rules, so it simplifies instead of overwhelming:

- **A question first, then the graph.** Each view opens on its answer (a sentence, a number, a short
  list); the graph is where you go for detail, not where you start.
- **Show little, reveal on demand.** Tools and flows by default; functions only when you open one.
  Collapse what's healthy; put what needs attention on top.
- **Every number has its evidence a click away:** the edge, the recording, the check that produced
  it, the same way findings carry evidence today.
- **Calm by default.** Status colours only for state (fine, attention, broken), with a word and an
  icon, never colour alone; charts follow a validated palette and work in light and dark.
- **The same report for people and agents.** Every view reads the JSON the command line writes, so
  anything shown can also be scripted.

## 3. Improving what's there

Functionality first, so the suite and console have something solid to show:

- the follow-ups already in the [roadmap's backlog](../ROADMAP.md): copied methods across modules
  sharing a base class, near-copy merging, reading binaries once, the Python frontend's memory;
- findings from the review of earlier work (in progress), turned into fixes;
- `feat/infer-spec`, `feat/platform` and `feat/cost-model` from Phase 3: the suite's contract, rules
  and the console's stage, safety and cost views all depend on them.

## Order

| # | Piece | Depends on | Size |
|---|---|---|---|
| 1 | Console skeleton: system map and findings, over today's graph and diagnose JSON | nothing new | 1–2 weeks |
| 2 | Builder's suite: what's used today, task maps | recordings (done) | 1 week |
| 3 | `feat/infer-spec` (Phase 3) | recordings | 2 weeks |
| 4 | Builder's suite: contract, rules, roles, mock back end | infer-spec, platform | 2 weeks |
| 5 | Console: stage board, agreement and drift, sync and safety, cost | stage runner, cost model | 2 weeks |
| 6 | Builder's suite: scaffold and UI acceptance checks | 4 | 1–2 weeks |

## Decisions for you

- **Who builds the UIs:** someone in-house, a contractor, an agency? It decides the contract's
  language and framework (TypeScript and React assumed), and how much the scaffold should do.
- **The console's form:** a local web app served by `optum` (assumed), or something else.
- **How much design to own:** a neutral default look for the console and the scaffold, with the
  company's own design system plugged in for their screens.
