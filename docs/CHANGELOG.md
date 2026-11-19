# Changelog

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
