# Plan: Phase 2, from graph to diagnosis

*Draft of 2026-11-18. Refines the Phase 2 rows of the [roadmap](../ROADMAP.md).*

## The goal

Phase 1 made the graph. Phase 2 makes it say something a business would act on:
**`optum diagnose <dir>`**, a report of waste in a codebase or a company's tools:

- the same job done by several tools;
- code nothing uses;
- logic written twice;
- the same data defined in several places.

Each finding comes with the evidence and with how sure Optum is. That report is the concept doc's
first product, the diagnostic (concept doc sections 5 and 11). After it comes one automatic fix,
to show that findings can be acted on safely.

## How it's measured

Phase 1 stayed honest because every number had an answer key. Phase 2 keeps that: every analysis
gets samples with **planted findings**, known in advance, and a check on real code.

| Analysis | Answer key | Real-code check |
|---|---|---|
| Overlapping glue | The mixed sample: 4 tools on 3 APIs and 1 table, all known | Phase 3's test company |
| Unused code | Samples with planted unused functions, including ones only *used as a value* (callbacks, dispatch tables) that must **not** be reported | Hand-check a sample of findings on a real repo. Any function the runtime tracer sees called is a false finding, found automatically |
| Duplicated logic | Planted clones: exact copies, copies with renamed variables, copies with small edits | Plant mutated copies of real functions into a real repo (recall); hand-check the top findings (precision) |
| The same data in several places | Samples where one entity is a Python class, a SQL table, a JS object and an n8n payload | Phase 3's test company |

Precision comes first throughout. A diagnosis that cries wolf won't be read twice.

## The branches, in order

| # | Branch | Job | Size | Gate |
|---|---|---|---|---|
| 1 | `feat/diagnose` | `optum diagnose <dir>`: the report frame (Markdown for people, JSON for agents), plus the first section, **overlapping glue**: APIs and tables reached by more than one program, script or workflow, with who reaches each and how (method, read or write) | 3–4 days | The mixed sample's report lists exactly its known overlaps. **Met:** 3 samples matched exactly, and 0 findings on dist-packages ([diagnose.md](../diagnose.md)) |
| 2 | `feat/analysis-unused` | Unused functions and modules, unused imports. Frontends first record when a function is **used as a value** (`attrs.referenced`), so callbacks and dispatch tables aren't called unused. Only what can't be reached from outside counts (private names, `static`, unexported); public API is reported separately and more cautiously | 3–5 days | Planted samples: 100% precision, ≥ 90% recall; on a real repo, no finding that the tracer sees called. **Met:** planted 8/8 found, 0 decoys reported; stdlib 20 findings, none seen called ([diagnose.md](../diagnose.md#unused-code-on-real-code)) |
| 3 | `feat/analysis-dupes` | Duplicated logic by **CGL structure memory on source code**: syntax subtrees with typed holes, counted across the codebase (the level where CGL's skeletons belong, as the binary study showed). Clusters of functions sharing most of their structure | 1–2 weeks | Planted clones: ≥ 95% precision; recall reported per clone type |
| 4 | `feat/analysis-entities` | The same entity in several places: record types, SQL tables, JSON payloads and API bodies with matching field names, grouped into proposed single definitions | 1 week | Planted samples: ≥ 90% precision |
| 5 | `feat/rewrite-one` | One automatic fix end to end: **merge exact duplicates** in Python (one definition kept, callers redirected), applied only when the project's tests pass before and after | 1–2 weeks | On a sample project with tests: the fix applies, tests pass, the duplicate is gone |

**Gate v0.2** (as in the roadmap): on a real repo, the report finds duplications a person agrees
with, and one automatic rewrite merges with all tests still passing.

## Decisions taken here

- **The report frame comes first, and each analysis adds a section to it.** There's a product
  output after a few days, not at the end.
- **Overlapping glue is the first section,** because the graph already holds everything it needs:
  that's what the shared `http:` / `sql:` ids were for.
- **No new edge kinds.** "Used as a value" is a node attribute (`attrs.referenced`), within IR 0.1.
- **Findings carry confidence,** like edges do: `certain` (from exact edges only), `likely` (inferred
  edges involved), `possible`. The report sorts by it.
- **Money comes later:** sizes (lines, functions, endpoints) now; costs once Phase 3 gives real
  prices (licences, upkeep hours).

## Risks

| Risk | What to do |
|---|---|
| False "unused" findings from dynamic use (reflection, plugins, entry points in config files) | `attrs.referenced`; public API reported separately; the runtime tracer as a check; say "no static reference found", never "dead" |
| Clone detection floods the report with trivial matches (getters, small wrappers) | Minimum size; findings ranked by lines saved; trivial patterns counted by the structure memory and down-weighted |
| A report nobody reads | Short first page: the top findings with numbers; details after. Shown to someone outside the project before v0.2 |
