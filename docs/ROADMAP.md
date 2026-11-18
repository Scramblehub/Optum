# Optum roadmap: first proof of concept

*Draft of 2026-11-18. Planned for about 16 weeks, with room to stretch to 24.*

## The order of work

1. **Break down:** turn programs into one graph of what they do, and prove the graph is right.
2. **Optimize:** find waste in the graph (dead code, duplicated logic, glue) and fix one kind
   automatically, with proof that behaviour didn't change.
3. **Unify:** apply the same machinery to a small fake company's SaaS stack and replace one flow.

Each phase ends with a gate: a result you can show someone. Don't start the next phase until the
gate passes. Missing it means the plan changes, not that you push harder.

## Decisions to make in week 1

These shape everything after them. My recommendation follows each one.

| Decision | Recommendation | Why |
|---|---|---|
| Implementation language for Optum | **Python 3.12** | Fastest for one person; the standard library parses Python; good bindings for tree-sitter, capstone and networkx |
| What "a program" means at first | **Source code first, binaries second** | Company glue is mostly scripts, workflow configs and SQL, not compiled binaries. Binaries are a much harder research problem; do them once the graph model has been proven on source |
| The shared graph format (the "IR") | **Typed JSON, versioned, documented in `docs/ir.md`** | Every frontend, internal or external, writes the same format. That format is the communication layer |
| How to define "graphed perfectly" | **Measured against ground truth:** hand-checked graphs for fixtures, plus runtime traces | "Perfect" can't be a gate unless it's a number |

## The system graph (the IR)

Everything plugs into one graph. Version 0 needs only:

- **Nodes:** `program`, `module`, `function`, `data_type` (a record or entity), `store` (a database
  table or file), `external` (an HTTP API or SaaS endpoint), `step` (a workflow step from a
  non-code tool)
- **Edges:** `calls`, `reads`, `writes`, `defines`, `sends_to` (data leaves for an external),
  `triggers` (one step starts another)
- **On every node and edge:** where it came from (file, line or address), which frontend produced
  it, and a confidence (`exact`, `inferred`, `observed`)

The last point matters later: when we optimize or replace something, we need to know which parts
of the graph are certain.

## Frontends and the communication layer

A frontend reads one kind of artifact and writes IR.

| Frontend | Reads | Built on |
|---|---|---|
| Python | `.py` source | the standard library's `ast` module |
| JavaScript / TypeScript | `.js`, `.ts` | tree-sitter grammars |
| SQL | schemas and queries | `sqlglot` |
| Binary (ELF, x86-64) | compiled programs | `pyelftools` and `capstone`; Ghidra headless later if needed |
| Workflow configs | n8n / Node-RED JSON exports | hand-written: they are already graphs |
| **External protocol** | anything | any program that writes IR JSON to stdout |

The **external protocol** is the communication layer for private, proprietary or
non-compiled languages. Optum runs an adapter as a subprocess, sends it a request (`describe`,
then `extract <path>`), and reads IR back, the way LSP works for editors. Someone with a
proprietary language writes a small adapter in whatever language they like; Optum never needs
to see their compiler. Our own frontends should use the same protocol, so it gets real use from
day one.

## Phases and branches

Each line is a branch. Size is a rough guess for part-time solo work.

### Phase 0: Foundations (week 1)

| Branch | Job | Size |
|---|---|---|
| `main` (direct) | Repo, layout, this roadmap | done |
| `feat/skeleton` | `pyproject.toml`, package `Core/optum/`, CLI entry point `optum`, pytest, ruff, GitHub Actions CI | 1–2 days |
| `docs/decisions` | Write records 0001–0004 for the decisions above | half a day |

### Phase 1: Break down (weeks 2–7)

| Branch | Job | Size |
|---|---|---|
| `feat/ir-core` | IR data classes, JSON schema, load/save, merge two graphs, `docs/ir.md` | 3–4 days |
| `feat/fixtures` | 6–10 small programs in `tests/fixtures/`, each with a hand-written expected graph | 2–3 days, then ongoing |
| `feat/frontend-python` | Modules, functions, calls, imports, HTTP calls (`requests`, `httpx`) as `external` | 1 week |
| `feat/export` | Graphviz DOT, Mermaid and JSON export; `optum graph <path> --format svg` | 2 days |
| `feat/frontend-protocol` | The subprocess adapter protocol; port the Python frontend onto it | 3–4 days |
| `feat/frontend-js` | JS/TS via tree-sitter, through the protocol | 1 week |
| `feat/trace-verify` | Run a fixture, record which functions actually call which, compare with the static graph, report precision and recall | 4–5 days |
| `feat/viewer` | A local web page: pan, zoom, filter by module or edge type, click through to source | 1 week |
| `exp/frontend-binary` | ELF symbols and disassembly to a call graph for small C programs compiled with gcc; compare with the graph from the same C source | 1–2 weeks, a spike |

**Gate v0.1:** run `optum graph` on a real mixed-language repo (Python plus JS, about 10k lines)
and get a navigable graph. On the fixtures: call-edge precision ≥ 95% and recall ≥ 90%
against the hand-checked graphs, with the trace check agreeing. The binary spike has a written
result, even if the result is "not yet".

### Phase 2: Optimize (weeks 8–12)

Start by suggesting; rewrite automatically only once the suggestions are trusted.

| Branch | Job | Size |
|---|---|---|
| `feat/analysis-dead` | Unreachable functions, unused modules, unused external endpoints | 3 days |
| `feat/analysis-dupes` | Duplicated logic: normalize function bodies to skeletons and cluster them (the CGL structure-memory idea, in a simple first form) | 1–2 weeks |
| `feat/analysis-entities` | Find the same entity in several places (a `Customer` class, a `customers` table, a CRM payload) and propose one definition | 1 week |
| `feat/report` | `optum diagnose`: one report listing waste, priced where it can be (lines, endpoints, duplicated stores) | 3 days |
| `feat/rewrite-one` | One automatic fix end to end, e.g. merge duplicated functions, gated by the fixture tests passing before and after | 1–2 weeks |

**Gate v0.2:** on a real repo, the report finds duplications a person agrees with, and one
automatic rewrite merges with all tests still passing.

### Phase 3: Unify (weeks 13–16, can stretch to 24)

This is the "test company" from section 11 of the concept doc, kept small.

| Branch | Job | Size |
|---|---|---|
| `feat/testco` | Docker Compose with 3–4 open-source tools (e.g. Odoo, a help desk, n8n with glue workflows) and a data generator | 1 week |
| `feat/frontend-saas` | Adapters reading those tools' APIs, schemas and n8n workflows into IR | 1–2 weeks |
| `feat/unified-model` | Merge the per-tool graphs into one company graph: shared entities, one flow (e.g. order-to-cash) | 1 week |
| `feat/generate-one-flow` | Generate a small owned service that replaces the glue for that one flow | 1–2 weeks |
| `feat/parallel-run` | Run old and new side by side on generated data and reconcile the results record by record | 1 week |

**Gate v0.3:** one flow in the test company runs on the generated service, the parallel run
reconciles exactly, and the before/after simplicity numbers from section 3.4 of the concept doc
are reported.

## Alongside the code

- **Interviews (concept doc section 11).** One or two conversations a month with operations
  heads or CTOs during Phases 1–2. They cost little, and they tell you whether Phase 3 should
  target the right flow. The concept doc's business case rests on the replaceable share of SaaS
  spend, and only those conversations can measure it.
- **CGL.** Reuse from `~/Projects/AI` deliberately: copy or depend on a specific piece when a
  branch needs it (most likely in `feat/analysis-dupes`), and record what was taken in
  `External_Libraries/README.md`.

## Risks to the plan itself

| Risk | What to do |
|---|---|
| Binary decompilation swallows months | Keep it as a time-boxed `exp/` spike; source frontends carry the gates |
| The IR keeps changing and breaks every frontend | Version it; fixtures with expected graphs catch breakage at once |
| "Perfect graph" never feels done | The gate is the precision/recall number, not a feeling |
| The viewer turns into a front-end project | Use an off-the-shelf graph library (Cytoscape.js); no custom rendering |
