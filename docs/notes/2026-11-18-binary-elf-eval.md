# Binary frontend (ELF, x86-64): first evaluation (2026-11-18)

Measured on the corpus (`tests/corpus`: 35 binaries, five optimisation levels, four forms) with
`tests/tools/score_binary.py`:
- **Function starts** are compared with the answer key's addresses.
- **Calls** between the binary's own functions are mapped to source ids through those addresses
  and compared with the calls GCC emitted at that level.

## Results

| Form | Functions: precision / recall | Calls: precision / recall |
|---|---|---|
| debug, symbols, stripped | **100% / 100%** | **100% / 100%** |
| **bare** (no symbols, no unwind tables) | **96.9% / 98.2%** | **98.9% / 94.4%** |

| bare, by level | Functions | Calls |
|---|---|---|
| -O0 | 100% / 100% | 100% / 100% |
| -O1 | 96.6% / 100% | 100% / 99.8% |
| -O2 | 92.4% / 96.9% | 98.1% / 90.1% |
| -O3 | 96.7% / 96.7% | 98.3% / 89.3% |
| -Os | 98.5% / 97.1% | 97.4% / 90.0% |

**sqlite** (local only; 207k instructions, 0.11 s, 31 MB): with symbols 100% / 100% functions and
99.6% / 99.6% calls; bare 95.3% / 99.8% and 99.5% / 98.8%.

## How it got there

| Step | Bare functions (P / R) | Calls with symbols (P / R) | What it fixed |
|---|---|---|---|
| First run | 65.7% / 98.2% | 100% / 75.7% | |
| Answer key: size-0 runtime functions counted | | | `frame_dummy` & co. are real code |
| Never split inside a symbol's or unwind entry's extent | | 98.1% / 78.5% | False splits even with symbols |
| `switch` jump tables recovered | 90.1% / 98.2% | | Case blocks reached only through a table |
| PLT calls to the library's own exports go to the local function | | 98.5% / **100%** | Shared libraries call their own exports through the PLT |
| Gap starts don't cascade; `endbr64` required in CET binaries | **96.9% / 98.2%** | | Unreachable `jmp; nop` blocks inside `-O0` functions |
| Answer key: runtime code identified by its source file existing | | **100% / 100%** | `crtstuff.c` had looked like project code |

## Robustness

Binaries in a repo are untrusted input. `tests/tools/fuzz_elf.py` mutates real ELF files (byte
flips, truncation, corrupted header and section fields) and runs Optum on each under ASan/UBSan.
- **900 mutants run locally:** no memory errors.
- **One bug found, and fixed:** invalid UTF-8 in a symbol name aborted the JSON writer. Output
  now replaces invalid bytes everywhere (`dump_json`), which also covers Latin-1 source files and
  odd file names.
- **ctest runs 150 mutants on every build.**

## Not yet

- Source ids from debug info (DWARF): `bin:` ids in every form today.
- Targets of indirect calls (function pointers, vtables).
- Strings passed to libcurl / sqlite (stage 4, lifting).
- C++ name demangling; PE, Mach-O and ARM64.
