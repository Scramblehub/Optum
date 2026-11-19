# The binary reference corpus

Known C programs compiled at every optimisation level, kept in four forms, each with an answer key.
It's the ground truth for Optum's binary work ([plan](../../docs/plans/binary.md), stage 2).

```sh
python3 tests/tools/build_corpus.py --out build/corpus            # ~3 s on 32 cores
python3 tests/tools/build_corpus.py --out build/corpus --check    # answer keys consistent
python3 tests/tools/score_functions.py --corpus build/corpus --baseline eh_frame --form bare
python3 tests/tools/build_corpus.py --out build/corpus --with-sqlite   # adds sqlite: local only
```

`ctest` builds it into `build/<preset>/corpus` (test `corpus_build`) and checks it.

## What's in it

[projects.json](projects.json) lists the projects. Each is built at `-O0 -O1 -O2 -O3 -Os` with
gcc, `-g -fPIC`.

| Project | Source | Binaries |
|---|---|---|
| samples | `tests/fixtures/programs/c` | `basic`, `multi` |
| glue | `tests/fixtures/programs/c/http_glue` + [stubs](stubs) | `http_glue`, linked against stub `libcurl.so.4` and `libsqlite3.so.0` |
| zlib v1.3.1 | submodule `External_Libraries/corpus/zlib` | `libz.so`, `minigzip`, `example` |
| cJSON v1.7.18 | submodule `External_Libraries/corpus/cJSON` | `libcjson.so`, `cjson_test` |
| lz4 v1.11.1 (**sealed**) | submodule `External_Libraries/corpus/lz4` | `liblz4.so`, `lz4_frame`, `lz4_file` |
| parson `ec53fb6` (**sealed**) | submodule `External_Libraries/corpus/parson` | `libparson.so`, `parson_tests` |
| sqlite 3.54.1 (optional) | downloaded, SHA3-256 pinned | `libsqlite3.so` |

That's 75 binaries without sqlite. **Sealed** projects are held-out test data for identification
research: they've been used once, as a test, by [round 3](../../docs/notes/2026-11-18-bin-skeletons-r3.md).
Don't tune anything on them; add new sealed projects for new studies.

**Stubs** ([stubs/](stubs)): stand-in headers and shared libraries for libcurl and sqlite, so glue
code builds the same everywhere without their development files. The headers use the real ABI
values (`CURLOPT_URL` = 10002, …) and the libraries the real sonames, so the machine code and the
imports are what the real ones produce. Stub libraries are marked `stub` and left out of the
baselines. A binary whose entry names `expected` gets `source_graph` in its answer key: the
hand-checked source graph whose APIs and tables it must reach.

## The four forms

| Form | What's left | Like |
|---|---|---|
| `debug/` | everything, with DWARF debug info | a developer build |
| `symbols/` | the symbol table (`strip --strip-debug`) | most distributed Linux binaries |
| `stripped/` | only what the loader needs (`strip --strip-all`) | release builds |
| `bare/` | stripped, and without `.eh_frame` unwind tables | other compilers, `-fno-asynchronous-unwind-tables`, much Windows code |

## The answer key (`<project>/<opt>/truth/<binary>.json`)

- **`functions`:** every function in the binary, read from the debug form:
  - its address, size and whether it's `static`;
  - its source file and line, and its **source id** in the C frontend's form
    (`c:deflate.c#deflate`), so a binary's graph can be scored against, and merged with, the
    source graph. The file is the declaring file from the debug info (`DW_AT_decl_file`), read
    with binutils' `readelf`. That makes it independent of Optum's own DWARF reader. `addr2line`'s
    line-table answer is only a fallback: for a header's inline function it gives the caller's file;
  - pieces GCC splits off or clones when optimising (`gz_compress.cold`, `.part.0`, `.isra.0`,
    `.constprop.0`, `.localalias`) have `fragment` set and their parent function's source id;
  - size-0 function symbols in code sections are included (the C runtime's `frame_dummy` and
    friends are real code); C runtime code has `origin: "runtime"` and no source id: a function is
    in the project only if its source file exists in the project.
- **`calls`:** the direct calls the compiler emitted at that level (GCC `-fcallgraph-info`),
  between the binary's own functions, as source ids. Calls that were inlined are gone, as they are
  in the machine code.
- **`external_calls`, `indirect_call_sites`, `imports`:** library functions called, calls through
  pointers, and the shared libraries and functions imported.

## What the baselines show (2026-11-18)

| Function starts from | Form | Precision | Recall |
|---|---|---|---|
| `.symtab` | symbols | 100% | 100% (the answer keys agree with the binaries) |
| `.eh_frame` | stripped | 96.9% | **95.9%** |
| `.eh_frame` | bare | — | **0%** |

gcc's unwind tables alone recover nearly every function start of a stripped x86-64 binary: all but
the C runtime's size-0 functions (`frame_dummy`, `register_tm_clones`), and the few extra "starts"
are the PLT import stubs. So **finding functions in `stripped` binaries is not a real test**: the
`bare` form is, and so is everything past function starts (calls, imports, identities).

Optum's binary frontend on the same corpus: `tests/tools/score_binary.py`
([results](../../docs/notes/2026-11-18-binary-elf-eval.md)).
