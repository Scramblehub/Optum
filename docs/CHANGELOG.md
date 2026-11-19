# Changelog

## v0.2: Optimize (2026-11-19)

Phase 2 of the [roadmap](ROADMAP.md): from the graph to a diagnosis of waste, and a first fix
that acts on it safely.

### What's in it

- **`optum diagnose <dir>`** ([diagnose.md](diagnose.md)): a report in Markdown for people and
  JSON for agents. Every finding has its evidence and a confidence; notes say what was left out on
  purpose. Findings:
  - **overlapping glue:** several tools reaching the same APIs and tables;
  - **unused code** and **code only tests use** (rules after Magellan's: local definitions whose
    name is used nowhere else, framework hooks and dunders left alone);
  - **duplicated logic:** exact, renamed and near copies, by CGL's structure memory on source code
    (rare pieces weigh more, so common idioms don't make functions look alike);
  - **the same data in several places:** a class, a table and an API payload describing one
    entity, matched by a shared id or a temporary composite key, never by name; tax, legal and
    medical forms are never proposed for merging.
  - Vendored, build-output, example and generated code is left out of findings.
- **The first automatic fix** ([fixes.md](fixes.md)): merges exact duplicate Python functions and
  methods (an import, an alias, or deletion where inherited), only when it's safe: the same code,
  globals that mean the same, no import cycle (counting the merges' own imports), not regulated
  code. Dry run by default; with `--test`, the project's tests must pass before and after, or every
  file is restored.
- **Frontends record more:** fields of classes, SQL columns and declared keys, API payload fields;
  name uses (`attrs.referenced`, `attrs.local`) for unused code.

### Gate results

| Gate | Result |
|---|---|
| The report finds duplications a person agrees with (real repo) | sympy and sphinx: a random 12 groups, all real copies; standard library: 15 of 15 ([note](notes/2026-11-19-real-repos.md)) |
| One automatic rewrite merges with all tests passing (real repo) | sympy: 6 merges, 1,906 tests passing before and after; networkx: 2 merges, 1,413 tests |
| Planted samples, each analysis | overlapping glue 3/3 samples exact; unused 8/8 planted, 0 of 20 decoys; duplicated logic 6/6 families, 0 decoys; same data 2/2 entities, 0 decoys (including a same-named customer, coincidental fields and regulated forms); the fix 6 merged, 6 traps refused |

**Real-code numbers:**

| Check | Result |
|---|---|
| Unused code, standard library, against its runtime trace | 20 findings, none seen called |
| Unused code, capstone, against GCC `-Wunused-function` | GCC finds none; Optum's 2 are inside `#if 0` |
| Duplicated logic, planted copies of stdlib functions (recall) | exact 95%, renamed 92–98%, edited 95% |
| Whole runs under AddressSanitizer: express, lodash, redis, curl, django | no errors |

**The graph got better along the way:**

| Code | v0.1 | v0.2 |
|---|---|---|
| Python standard library, runtime calls in the graph | 73.4% (same trace) | **86.9%** |
| npm, runtime calls in the graph | 78.8% | **98.1%** |
| zlib against GCC's call graph, precision / recall | 85% / 99.4% | **98% / 100%** |

### Changed at v0.2

- **SQL:** quoted table names with spaces, escaped quotes in string literals, SQLite's
  `INSERT OR REPLACE` (it read as a table named `OR`).
- **Tracers:** a constructor call is recorded as the class actually called; JS implicit
  constructors no longer show up as the module calling the base class.
- **C:** `#if` blocks are evaluated for one build configuration (an optional
  `compile_commands.json` gives the real `-D` flags).
- **CI:** tests run in parallel, and a newer push cancels an older run.

### Known limits

The same-entity finding isn't measured on real business code yet (Phase 3). The fix doesn't
share copied methods through a base class yet, so copies across modules make one module import
another. See "Later improvements" in the [roadmap](ROADMAP.md).

## v0.1: Break down (2026-11-18)

Phase 1 of the [roadmap](ROADMAP.md): Optum turns code, workflows and compiled programs into one
graph and measures how right it is.

### What's in it

- **Graph format (IR 0.1):** nodes and edges with confidence (`exact`, `observed`, `inferred`),
  versioned JSON with a JSON Schema ([ir.md](ir.md)); `optum check`, `optum merge`.
- **Frontends:** Python, JavaScript/TypeScript and C (tree-sitter), plus x86-64 ELF binaries
  (capstone, with our own ELF and DWARF readers). Rules: [frontends.md](frontends.md).
  - APIs (`requests`, `fetch`/`axios`, libcurl) and SQL tables become shared `http:` / `sql:`
    nodes, so different tools doing one job meet on the same nodes.
  - Binaries with debug info get source ids, and merge with their source graph node for node.
- **Adapters** in any language through a JSON protocol ([protocol.md](protocol.md)); the first one
  reads n8n workflows.
- **Diagrams and views:** SVG, DOT, Mermaid; `--level module`, `--focus`, `--edges`,
  `--no-inferred` ([diagrams.md](diagrams.md)).
- **Measuring accuracy:**
  - `optum score` against hand-checked samples;
  - `optum verify` against runtime traces ([verify.md](verify.md));
  - a binary corpus of 75 binaries with answer keys ([tests/corpus](../tests/corpus/README.md)).

### Gate results

| Gate (roadmap) | Result |
|---|---|
| A real mixed-language repo becomes a navigable graph | node-gyp: 57 Python + 17 JS files (40k lines) → 1,569 nodes, 3,964 edges in about 70 ms; module and focus views render |
| Speed: 1M lines in under a minute | 1.97M lines of Python (`dist-packages`) in **0.88 s**. The whole directory, with 195 compiled modules (5.6M instructions), in 11 s |
| Samples: call precision ≥ 95%, recall ≥ 90% | Python, JS, C samples: 100% precision. Recall: 100%, apart from the edges marked hard by design |
| Runtime check agrees | `optum verify` on the runnable Python samples: 100%, apart from `dynamic`'s hard edges |
| Binary result written down | [Binary plan](plans/binary.md): stages 1–4b done; stage 5 research written up (negative result across optimisation levels) |

**Real-code numbers:**

| Code | Result |
|---|---|
| Python standard library | 70.6% of 537 observed calls, indirect ones included |
| npm | 79% of profiled calls |
| C | cJSON 100% / 100% and zlib 85% / 99.4% against GCC's own call graph |
| Binaries | 100% / 100% with symbols; about 99% / 97% fully stripped |

### Changed at v0.1

- **The local web viewer moved to the backlog:** diagram views cover it for now.
- **Python ids:** a package attribute that shadows a submodule (pip's `main()` next to `main.py`)
  gets `@def`. Before, the collision aborted the whole graph. Found by the 2M-line gate run.

### Known limits

See "Known gaps" in [frontends.md](frontends.md) and "Later improvements" in the
[roadmap](ROADMAP.md).
