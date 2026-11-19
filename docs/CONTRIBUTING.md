# How work is done in this repo

Kept short on purpose. Change it when it stops fitting.

## Branches

- `main` always builds cleanly (no warnings) and passes its tests, including the `asan` preset. No direct commits once Phase 0 is done.
- One branch per job from the roadmap, named by kind:
  - `feat/<name>`: a new capability, e.g. `feat/frontend-python` (a C++ frontend that reads Python)
  - `fix/<name>`: a bug fix
  - `exp/<name>`: a research spike that may be thrown away. Merge only what's worth keeping, or
    just write down what you learned in `docs/notes/`
  - `perf/<name>`: making something faster without changing what it does
  - `docs/<name>`: documentation only
- Merge through a pull request, even working alone. The PR description is the record of why.
- Messy first, clean later: a branch may merge rough, as long as `main` still passes its tests and
  the roughness is noted in the PR.

## C++ conventions

- C++20, built with CMake presets (`debug`, `release`, `asan`). See the README for commands.
- Before pushing: `cmake --build --preset asan && ctest --preset asan`.
- Format with `clang-format` (the repo's `.clang-format`: Google style, 100 columns).
- Compiler warnings (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`) are treated as bugs: fix
  them, don't silence them.
- Public headers go in `Core/include/optum/`, implementation in `Core/src/`, the CLI in
  `Core/cli/`. Everything lives in `namespace optum`.
- New dependencies go in `External_Libraries/` as pinned shallow submodules, with a row in its
  README (version, license, purpose). No unpinned downloads at build time.
- Every new feature comes with tests in `tests/` (Catch2). Frontends also get fixtures with
  expected graphs in `tests/fixtures/`.
- Scripts and tooling outside the core (fixture tracers, the viewer) may use other languages;
  see decision 0001.

## Milestones

Each roadmap phase ends with a tag (`v0.1`, `v0.2`, ...) and a short entry in `docs/CHANGELOG.md`.

## Decisions

Decisions that would be expensive to reverse (IR format, implementation language, generated
stack) go in `docs/decisions/NNNN-title.md`: the context, the decision, and the alternatives turned down.
