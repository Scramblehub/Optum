# Optum roadmap: first proof of concept

*Draft of 2026-11-18. Planned for about 16 weeks, with room to stretch to 24.*

## The order of work

1. **Break down:** turn programs into one graph of what they do, and prove the graph is right.
2. **Optimize:** find waste in the graph (dead code, duplicated logic, glue) and fix one kind
   automatically, with proof that behaviour didn't change.
3. **Unify:** apply the same machinery to a small fake company's SaaS stack and replace one flow.

Each phase ends with a gate: a result you can show someone. Don't start the next phase until the
gate passes. Missing it means the plan changes, not that you push harder.

## Decisions (made 2026-11-18)

Recorded in `docs/decisions/`:

1. **C++20 core** with CMake; dependencies pinned as git submodules in `External_Libraries/` ([0001](decisions/0001-implementation-language-cpp.md))
2. **Source code first, binaries second** ([0002](decisions/0002-source-before-binaries.md))
3. **Versioned JSON graph format**, which doubles as the language for external adapters and agents; a human-readable layer comes later ([0003](decisions/0003-json-graph-format.md))
4. **Accuracy measured on fixtures; realism from the field:** the test company mimics a real interviewed business ([0004](decisions/0004-measuring-graph-accuracy.md))

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
| Python | `.py` source | tree-sitter + `tree-sitter-python` |
| JavaScript / TypeScript | `.js`, `.ts` | tree-sitter grammars |
| C / C++ | source | tree-sitter grammars; libclang later if types are needed |
| SQL | schemas and queries | tree-sitter SQL grammar |
| Binary (ELF, x86-64) | compiled programs | own ELF reader or LIEF, plus capstone; Ghidra headless later if needed |
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
| `docs/decisions` | Records 0001–0004 | done |
| `feat/skeleton` | CMake build, `Core/` library + `optum` CLI, submodules (nlohmann/json, CLI11, Catch2), tests, GitHub Actions CI with sanitizers | 2–3 days |

### Phase 1: Break down (weeks 2–7)

| Branch | Job | Size |
|---|---|---|
| `feat/ir-core` | IR types in C++, JSON Schema, load/save, merge two graphs, `docs/ir.md` | 1 week |
| `perf/ir-json` | Streaming JSON reader/writer (e.g. simdjson or a SAX parser) and lighter node storage; target: the 1M-node graph loads in under 2 s and 1 GB | 3–4 days |
| `feat/fixtures` | 7 sample programs (6 Python, 1 C) with hand-checked expected graphs, conventions in `docs/frontends.md`, and `optum score` (precision/recall per edge kind, hard edges separately) | done; add more as frontends grow |
| `feat/frontend-python` | tree-sitter integration; modules, functions, classes, calls, imports, HTTP (`requests`, `httpx`) and SQL (`sqlite3`-style `execute`) | done: 100% precision / 95% recall on samples, 93.5% recall on the stdlib ([notes](notes/2026-11-18-python-frontend-eval.md)) |
| `feat/export` | DOT, SVG (via Graphviz) and Mermaid; views: `--level module`, `--focus`, `--edges`, `--no-inferred` ([docs/diagrams.md](diagrams.md)) | done |
| `feat/frontend-protocol` | Adapter protocol ([docs/protocol.md](protocol.md)): any program, any language; built-in Python frontend behind the same interface; first adapter: n8n workflows (Python); shared id rules so tools meet on one node | done |
| `feat/frontend-js` | JS/TS via tree-sitter, through the protocol | 1 week |
| `feat/trace-verify` | Run a fixture, record which functions actually call which (a Python tracer for Python, `-finstrument-functions` for C), compare with the static graph, report precision and recall. Ignore class-body frames: they look like calls but aren't | 1 week |
| `feat/viewer` | A local web page: pan, zoom, filter by module or edge type, click through to source | 1 week |
| `feat/frontend-c` … `feat/bin-rules` | Binaries: 7 stages from the C source frontend and an ELF reader to CGL-style structure memory and verified identification. See [plans/binary.md](plans/binary.md) and [decision 0005](decisions/0005-binaries.md) | 9–13 weeks; stages 1–4 (C source, corpus, ELF reader, lifting) first, ~5 weeks |

**Gate v0.1:** run `optum graph` on a real mixed-language repo (Python plus JS, about 10k lines)
and get a navigable graph. Speed check: a 1M-line Python codebase graphed in under a minute on
this machine (32 cores). On the fixtures: call-edge precision ≥ 95% and recall ≥ 90%
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

This is the "test company" from section 11 of the concept doc, kept small, and modeled on a real business from the interviews (see [0004](decisions/0004-measuring-graph-accuracy.md)): the same kinds of tools and the same glue, swapped for open-source stand-ins.

| Branch | Job | Size |
|---|---|---|
| `feat/testco` | Docker Compose with open-source stand-ins for the interviewed company's tools (e.g. Odoo for ERP, a help desk, n8n for its glue) and a data generator | 1–2 weeks |
| `feat/frontend-saas` | Adapters reading those tools' APIs, schemas and n8n workflows into IR | 1–2 weeks |
| `feat/unified-model` | Merge the per-tool graphs into one company graph: shared entities, one flow (e.g. order-to-cash) | 1 week |
| `feat/generate-one-flow` | Generate a small owned service that replaces the glue for that one flow | 1–2 weeks |
| `feat/parallel-run` | Run old and new side by side on generated data and reconcile the results record by record | 1 week |

**Gate v0.3:** one flow in the test company runs on the generated service, the parallel run
reconciles exactly, and the before/after simplicity numbers from section 3.4 of the concept doc
are reported.

## Later improvements (backlog)

Picked up when they become the bottleneck, not before. Each is a small branch of its own.

- **Python frontend:** read type hints (`def f(source: Tokenizer)`); track return types of project
  functions (`x = make_client()`); decorator factories and dispatch tables (the "hard" edges);
  calls into third-party SDKs (`stripe.Charge.create()`) as `saas:` edges.
- **n8n adapter:** JavaScript in Code nodes; raw SQL in `executeQuery`; link `executeWorkflow` to
  the called workflow.
- **Diagrams:** dark mode; draw package containment as nesting.
- **Tracing:** turn `tests/tools/stdlib_recall.py` into `feat/trace-verify` (fix the `__init__`
  artifact).
- **Speed:** `perf/ir-json` (streaming JSON), once graphs get big enough to need it.

## Alongside the code

- **Interviews (concept doc section 11).** First one by mid-December 2026, using the checklist
  in [0004](decisions/0004-measuring-graph-accuracy.md); then one or two a month during Phases 1–2. They cost little, and they tell you whether Phase 3 should
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
| C++ slows early progress |  Keep branches small; lean on tree-sitter and libraries instead of writing parsers |
| "Perfect graph" never feels done | The gate is the precision/recall number, not a feeling |
| The viewer turns into a front-end project | Use an off-the-shelf graph library (Cytoscape.js); no custom rendering |
