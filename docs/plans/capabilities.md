# Plan: from code to capabilities, replaceability, and the interface

*Draft of 2026-11-20. Folds a new set of directions into the existing plan ([phase3.md](phase3.md),
[builders-suite.md](builders-suite.md), [plateau.md](plateau.md)). It doesn't restart anything:
each item says which planned branch it sharpens.*

The question Optum is built to answer: **what is the minimum software a company needs to keep
every business capability it actually uses, and what would it cost to get there?** Answering it
takes a chain of conclusions, each derived from the one before:

`function → operation → data → workflow → capability → replaceability → cost`

Today Optum has the first link solid (the graph, measured on real code) and pieces of the next
three: frontends evaluate the URLs code calls; `infer_spec` learns operations, resources and state
machines from traffic; the n8n adapter reads flows; `diagnose` finds overlapping glue and the same
entity in two places. What's missing is the chain itself, and a way for conclusions far from the
source to stay honest.

## 1. Operations, data and workflows: a derived layer, not new IR

Source facts stay in the IR as they are. Everything above them is **derived**, kept in a separate
file that names the facts it rests on, so facts and conclusions never mix.

| Layer | What it is | Derived from |
|---|---|---|
| **Operation** | (system, action, resource, reads or writes): `accounting: create account.move` | code: HTTP calls with evaluated URLs, JSON-RPC model and method, later SQL; traffic: `infer_spec` operations |
| **Data** | each resource, who reads it, who writes it, and who **owns** it (holds the copy others defer to) | operations' read/write sets; `same_entity` for copies under other names |
| **Workflow** | operations chained through the same record over time: payment → order paid → invoice → completed | traffic status changes; automation flows; call paths in code |
| **Capability** | a set of workflows over a set of resources: *billing* = invoices created for paid orders | matched by **what is done to which data**, never by names; a small catalogue of signatures (billing, order processing, customer management, ...) |

Rules: a capability can have several implementations (the n8n flow, the nightly script and the
owned service all bill orders), and an implementation can cover part of one. Nodes are not forced
into a capability; most code is plumbing and stays unclassified. Contradicting evidence is kept and
shown, not averaged away.

## 2. Replaceability: rules over the graph, not scores

"If this component disappeared, what would break, and could something else take over?" Four
separate properties, each with its own evidence:

| Property | Means | Evidence Optum already has |
|---|---|---|
| Functional equivalence | another component covers the same operations | the operation layer |
| Data equivalence | it writes the same resources with the same fields | the stage contract check |
| Behavioural equivalence | same outputs on the same inputs | reconcile, replay, `verify` |
| Operational replaceability | it can run in its place: latency, availability, who operates it | `stage1.py measure`, the platform drills |

Classes, each a rule with its evidence listed:

- **Independently removable:** nothing depends on it, it owns no data, it does nothing unique (the
  never-triggered cart flow).
- **Replaceable with migration:** it owns data, and another component covers all its operations.
- **Replaceable after dependents change:** others call it directly; they must move first.
- **Essential:** it does something nobody else does, or owns regulated data (accounting).
- **Unknown:** the evidence doesn't cover enough of what it does to say.

## 3. Evidence that stays honest as conclusions get abstract

Every derived conclusion records:

- **provenance:** the inputs it rests on (source files by content hash, recording windows, flow
  exports) and the rule that produced it;
- **evidence quality and coverage, separately:** how strong each piece is (static, traced,
  recorded, declared, estimate) and how much of the subject it covers;
- **no confidence from weak chains:** a conclusion is never more certain than the weakest
  evidence it needs, so a chain of guesses stays a guess;
- **abstention:** below a coverage floor the answer is *unknown*, with what's missing;
- **staleness:** when an input's hash changes, every conclusion resting on it is marked stale until
  re-derived.

## 4. Generalisation

Covered by [the plateau loop](plateau.md): tuning, held-out and sealed repositories; fixes at the
layer where the failure starts; a gate per relationship type that fails on any loss. Next styles to
add as repositories plateau: plugins, dependency injection, event-driven code, generated code,
configuration-driven behaviour, cross-language calls.

## 5. Counterfactual architecture

This is the **what-if layer** already in [phase3.md](phase3.md) (`feat/what-if`), scaled up from a
single choice to a proposed architecture. A transformation (remove an application, consolidate two
services) is applied to a mirror, inputs are replayed, outputs and invariants compared, and the
verdict says what was confirmed, what differs, and what wasn't covered. Passing tests is not
behavioural equivalence, so the coverage is always reported.

## 6. Economics

`feat/cost-model` ([phase3.md](phase3.md)), with one rule added: **financial facts and estimates
are kept apart.** An invoice from a SaaS vendor is a fact; "this integration costs 3 hours a month
to maintain" is an estimate, labelled with its basis. Source analysis can establish that an
integration exists, not what it costs.

## 7. The interface: optimising how people use the system

The other half of optimisation is the screens. Optum can measure each feature's **usage**
(frequency, by role), its **accessibility** (navigation depth, time to reach it), its **weight in
workflows**, and **abandonment**, and flag mismatches. For example, an editor used constantly but
buried three menus deep. It recommends; the person building the UI with the company decides.
Frequency isn't importance (a rarely used security approval must stay one step away for its
role), so mismatch is a signal per role, never an automatic layout rule. Usage is aggregated by
role by default, not tracked per person.

This sharpens the builder's suite's "what's used today" and "task maps", and the console's views:
**one graph, several projections** (system map, data, workflows, interface, findings), with a
selection kept across them. The isometric view suits the system map. Workflow playback (animated
paths, thicker where more frequent, before and after a change) suits task maps.

## Order

| # | Work | Sharpens | When |
|---|---|---|---|
| 1 | Held-out evaluation and the per-relationship gate | plateau loop | now (gate in #61; held-out run with round 2) |
| 2 | Operation and data layers, then replaceability classes, on the test company with an answer key (stage 1 is the proof: the n8n flow really was replaceable) | `feat/unified-model` | done (`optum model`, #64) |
| 3 | The evidence model (provenance, weakest-link confidence, coverage, abstention, staleness), built into item 2 from its first commit | `feat/unified-model` | done, with item 2 |
| 4 | Workflows (done: process mining over the recordings in `optum model`, cases linked across systems with the evidence for each link) and capabilities over them | `feat/unified-model` | workflows done; capabilities next |
| 5 | Counterfactual architecture | `feat/what-if` | as planned |
| 6 | Facts and estimates in the cost model | `feat/cost-model` | as planned |
| 7 | Interface usage and accessibility: navigation graph and mismatch per role, from recorded UI sessions (the shop's admin first) | builder's suite, console | after the console skeleton |
