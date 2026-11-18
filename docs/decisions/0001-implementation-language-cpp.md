# 0001: Optum's core is written in C++20

*Accepted 2026-11-18.*

## Context
Optum has to break down codebases of hundreds of thousands to millions of lines, and later binaries.
Parsing, graph building and analysis over that much input must be fast and memory-efficient.

## Decision
- The core (graph model, frontends, analyses, CLI) is **C++20**, built with **CMake** (3.28+) and
  GCC 13+. Clang support comes later, through CI.
- Third-party code lives in `External_Libraries/` as **pinned git submodules**, so every build uses
  exactly the same versions.
- Source languages are parsed with **tree-sitter** (a C library with grammars for Python, JS/TS,
  C/C++, SQL and more). One parsing engine covers every language.
- Tooling that isn't the core (fixture tracers, scripts, the web viewer) may use whatever fits
  best: Python for tracing Python fixtures, JS for the viewer.

## Alternatives turned down
- **Python:** fastest to write, but too slow and memory-hungry at the target scale.
- **Rust:** comparable speed and safer, but the user prefers C++, and tree-sitter, capstone and
  LLVM are C/C++ libraries anyway.

## Consequences
Slower early progress. Tests and sanitizers (ASan/UBSan) run in CI from the start to catch
memory errors early.
