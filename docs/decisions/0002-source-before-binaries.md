# 0002: Source code first, binaries second

*Accepted 2026-11-18.*

## Context
Company glue is mostly scripts, workflow configs and SQL. Taking binaries apart (disassembly,
recovering functions and call edges) is a much harder research problem.

## Decision
The Phase 1 frontends and gate are built on source code. Binary breakdown is a time-boxed spike
(`exp/frontend-binary`): ELF parsing and capstone disassembly of small gcc-built C programs,
compared with the graph from the same C source.

## Consequences
The graph format is proven on inputs whose answer is known before it faces binaries, where the
answer is uncertain.
