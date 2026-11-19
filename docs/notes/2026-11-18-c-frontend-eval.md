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

(Object-like renames and the build configuration's `-D` flags: see the update below.)

## Update (2026-11-19): `#if`s evaluated for one build configuration

The extra edges above were all calls the compiler doesn't see in its configuration. The frontend
now decides, per translation unit, whether each `#if` is compiled, and an edge that isn't
compiled is `inferred` with `attrs.inactive: true` and `attrs.condition`; nothing is dropped. The
configuration is a GCC or Clang build on Linux with no `-D` flags, or the flags from a
`compile_commands.json` if the project has one. Rules: [frontends.md](../frontends.md#the-build-configuration).

Same scorer, same GCC ground truth (`-O0`, no `-D` flags, x86-64).

| Project | Precision, all exact edges | Precision, exact edges outside `#if` | Recall |
|---|---|---|---|
| **zlib** before | 73.5% (223 edges) | 85.0% (173) | 99.4% |
| **zlib** after | **97.1%** (170) | **98.0%** (152) | **100%** |
| cJSON before / after | 100% / 100% | 100% / 100% | 100% / 100% |
| parson before / after | 100% / 100% | 100% / 100% | 100% / 100% |
| lz4 before / after | 71.9% / 74.3% | 76.7% / 83.2% | 80.3% / 80.3% |

### Steps (zlib)

| Step | All exact | Outside `#if` | Recall | Why |
|---|---|---|---|---|
| Before | 73.5% | 85.0% | 99.4% | |
| + `#if`s evaluated; every `#define` kept with its own `#if`; `#define zmemcpy memcpy` renames the call; a function defined only in an `#if` that's off is called `inferred` | 88.1% | 90.2% | 98.8% | `zmemcpy`/`zmemzero`, `_tr_tally` (through `_tr_tally_lit`), `check_match` gone. Compiler and platform names were all unknown, so `HAVE_MEMCPY` (it depends on `__MSDOS__` through `SMALL_MEDIUM`) wasn't decided yet |
| + compiler and platform names (`__STDC__`, `__GNUC__` on; `_WIN32`, `__MSDOS__`... off); names the code uses as values (`O_CLOEXEC`) unknown | 96.4% | 97.9% | 97.0% | `send_bits`, the rest of `zmemcpy`. Recall fell: real calls were in `#if`s that tree-sitter's error recovery stretched (`#ifdef __cplusplus / extern "C" {` covering the rest of `zlib.h`, so `deflateInit` looked defined only for C++) |
| + `#if` structure read from the directives, line by line, not from the syntax tree | 96.5% | 98.0% | 99.4% | Those calls back |
| + a `.c` file's `#define`s don't reach other files | 97.0% | 98.0% | 99.4% | `BUILDFIXED` (defined in `inflate.c`) is off in `infback.c` |
| + an `#if` around `else if (...) {...}` (parsed as a function named `if`): its calls kept | 97.1% | 98.0% | **100%** | The one missed call, `read_buf → crc32` |
| + one edge per caller and callee, from its most certain call site; include guards not shown as conditions | 97.1% | 98.0% (152 edges) | 100% | An unconditional call is no longer shown with another call site's condition |

Two things found on lz4 on the way, both fixed: a function defined in both branches of an
`#if / #else` (lz4's thread pool) is compiled if either is, and recall there is unchanged.

### What's left (zlib)

- **3 extra outside `#if`:** `gzseek64`, `gz_skip`, `gz_zero` → `gz_intmax`, through
  `GT_OFF(x)` = `sizeof(int) == sizeof(z_off64_t) && (x) > gz_intmax()`. The `sizeof` test is false
  on x86-64 and GCC drops the call even at `-O0`. The call is in the source in every
  configuration; finding it dead would take type sizes, which the frontend doesn't have.
- **2 extra inside `#if`:** `crc32_z` → `multmodp` / `x2nmodp` under `ARMCRC32`, which needs
  `__aarch64__`. The architecture is left unknown on purpose.

### `compile_commands.json`

With one, each file's `-D` / `-U` flags decide its `#if`s first. Checked by building zlib with
`-DZLIB_DEBUG` (GCC truth and a `compile_commands.json` with the same flag): 97.4% / 98.2% /
**100%** of 188 compiler calls. Without the file, the same graph's recall against that debug build
is 87.8%: the debug-only calls are `inferred`, as they should be for the default build.

### Speed

zlib 0.09 s (was 0.10 s), lz4 0.11 s (0.09 s), cJSON 0.08 s (0.07 s); release builds.

### Still not covered

C++; macros that define functions; include paths from `compile_commands.json` (only `-D` / `-U`
are read); type sizes (`sizeof` in conditions); the architecture. lz4's recall (80%) has a
different cause, not looked at here: most misses look like calls from `always_inline` functions,
which GCC attributes to their callers.
