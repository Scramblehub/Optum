# Optum

Optum breaks software down into one graph of what it does, simplifies it, and rebuilds a company's
tool sprawl as one system the company owns. See [Optum-concept.md](Optum-concept.md) for the full
concept and [docs/ROADMAP.md](docs/ROADMAP.md) for the build plan.

Status: **pre-alpha, Phase 0.** The CLI runs, but has no frontends yet.

## Build

Needs GCC 13+ (C++20) and CMake 3.25+.

```sh
git clone https://github.com/Scramblehub/Optum.git && cd Optum
git submodule update --init --depth 1
cmake --preset debug && cmake --build --preset debug -j
ctest --preset debug
./build/debug/Core/optum graph .
```

Presets: `debug`, `release`, and `asan` (AddressSanitizer + UBSan; CI runs all three).

## Layout

| Folder | Holds |
|---|---|
| `Core/` | Optum's own source: the graph model (IR), frontends, analyses, viewer |
| `External_Libraries/` | Pinned third-party submodules; see its [README](External_Libraries/README.md) |
| `tests/` | Tests plus `tests/fixtures/`: small sample programs with known, hand-checked graphs |
| `docs/` | Roadmap, design notes, decision records |

## Working on it

See [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) for the branch and commit rules.
