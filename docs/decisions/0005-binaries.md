# 0005: How binaries join the graph

*Accepted 2026-11-18, and implemented: all three id tiers, the confidence levels and the licences
(stages 1–4 of [docs/plans/binary.md](../plans/binary.md)).*

## Context
Decision 0002 put binaries after source. With the Python and JS frontends working, binary
breakdown is the next frontend. Binaries have to merge with source graphs, carry honest
confidence, and avoid licences that would bind Optum's core.

## Decision
- **Scope first:** x86-64 Linux ELF from gcc. PE, Java and .NET bytecode, Mach-O and ARM64 follow
  as separate tracks once the interview shows which binaries businesses have.
- **Ids, best available first:**
  - the source id (`c:file#fn`) when debug info maps a function to its source, so binary and
    source graphs merge;
  - `bin:<binary>!<symbol>` with a symbol only;
  - `bin:<binary>!0x<address>` when stripped.

  Shared libraries are `lib:<soname>` and `lib:<soname>!<symbol>`. An identification of a stripped
  function goes in `attrs.identified_as`, never in the id.
- **Confidence:** `exact` for symbols and debug info, `observed` for an identification that passed
  differential execution, `inferred` for an identification by skeleton vote only.
- **No new node or edge kinds.** IR 0.1 holds binaries as they are.
- **Licences:** capstone (BSD) for disassembly; our own ptrace-based harness for differential
  execution, not an emulator. No GPL code in the core.
- **One structure-memory engine** (`Core/src/structure/`), over any typed tree, used by binary
  identification and by Phase 2's duplicate detection.

## Alternatives turned down
- **Addresses as ids everywhere:** they change with every build, so graphs from two builds
  wouldn't merge.
- **Ghidra headless as the frontend:** heavy (JVM), slow to start, and its output would still need
  mapping to our ids. It stays a possible reference for checking our results.
- **Unicorn for verification:** GPL, and an emulator is more than pure-function tests need.

## Consequences
Stripped binaries can only be *named* as far as the skeleton memory reaches. A company's own unique
functions get structure (calls, libraries, APIs) but not names. Reports have to say so.
