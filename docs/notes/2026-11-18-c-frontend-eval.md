# C frontend: first evaluation (2026-11-18)

## On the sample programs

basic, multi and http_glue (37 edges in total, 3 of them hard function-pointer edges): **100%
precision, 100% recall**, hard edges included.

## On real code, against the compiler

GCC's `-fcallgraph-info` writes down every direct call the compiler emits, after macro expansion
and for one build configuration. That makes it real ground truth, not a hand sample.
`tests/tools/c_callgraph_score.py` compares Optum's graph with it, counting only callers in the
compiled files.

| Project | Size | Compiler's calls | Precision (exact edges outside `#if`) | Recall |
|---|---|---|---|---|
| **cJSON** v1.7.18 | 2 library files (99 files analysed, 22k lines) | 276 | **100%** | **100%** |
| **zlib** v1.3.1 | 15 library files (69 analysed, 41k lines) | 165 | **85%** | **99.4%** |

Speed: zlib (41k lines) takes 0.08 s.

### What changed during the evaluation (zlib)

| Step | Precision | Recall | Why |
|---|---|---|---|
| First run, scoring only the compiled files | 75% | 94.5% | |
| + calls inside `#if` blocks tagged `attrs.condition` | 81% (outside `#if`) | | They're calls for other build configurations; the compiler saw one |
| + calls inside function-like macros (`FLUSH_BLOCK` → `flush_pending`, `deflateInit` → `deflateInit_`) | | 99.4% | Macros hid calls the compiler sees |
| + a definition elsewhere is `exact` only with a visible prototype | 85% | 97.6% | Two wrong links to other programs in the repo (`examples/gun.c`, a C++ header in `contrib/`) became `inferred` |
| + zlib's prototype styles (`f OF((...))`, `void EXPORT f(...)`) recognised | 85% | 99.4% | Recall back, precision kept |

### What's left

- **Extra (precision):** every remaining one depends on the build configuration:
  - `zmemcpy`/`zmemzero` are functions in one configuration and `#define`s for `memcpy` in another;
  - `send_bits` and `_tr_tally` are debug-build functions and macros otherwise.

  They're real "may call" edges, but the `#if` that decides them is in a header, not at the call.
- **Missed (recall):** one call, `read_buf → crc32`, inside a region the parser recovered from
  badly.
- **Parse errors:** 58 of zlib's 69 files have syntax errors to tree-sitter: macros like `ZEXPORT`,
  `FAR` and `OF()` that only parse after preprocessing. The frontend works through tree-sitter's
  error recovery, as the numbers show, but this is the main risk on old, macro-heavy C.

## Not covered

C++ (a `.h` may be C++); object-like macros that rename functions; macros that define functions;
the build configuration itself (a `compile_commands.json` reader would give the real `-D` flags
and include paths).
