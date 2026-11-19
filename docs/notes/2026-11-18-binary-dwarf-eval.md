# Source ids from debug info (2026-11-18)

Decision 0005's first id tier: a function in a binary built with `-g` is named `c:<file>#<name>`,
exactly as the C frontend names it in the source. Optum reads DWARF itself (versions 4 and 5, about
500 lines; no `libdw`).

## Results

| Check | Result |
|---|---|
| Debug form, corpus: project function addresses with the answer key's source id | **3,260 / 3,260** |
| Debug form, sqlite (local): the same | **9,385 / 9,385** |
| `multi` at `-O0`: binary functions and calls found in the C source graph | 9 / 9 functions, 6 / 6 calls |
| Function and call scores, all forms | unchanged (debug form 100% / 100%) |
| Cost | sqlite's debug build: 0.13 s in total, about 20 ms of it the DWARF reader |

The answer key's files come from binutils' `readelf` dump of `DW_AT_decl_file`, which is a separate
implementation from Optum's reader, so agreeing on every address is a real cross-check.

## What it took

- **Optum's reader:** one bug. `formats.emplace_back(c.uleb(), c.uleb())` evaluates its two reads
  in an unspecified order. GCC did them right to left, swapping each line-table format pair and
  misaligning everything after it. The unit test that reads the test executable's own debug info
  found it.
- **Prototypes:** a declaration in a caller's file (`DW_AT_declaration`) must never stand in for a
  definition. Matching by name is only safe for names defined in one file
  (`static get_object_item` exists in both `cJSON.c` and `cJSON_Utils.c`).
- **Identical-code folding:** gcc gives one body two names (`sqlite3SelectDelete` and
  `sqlite3SelectDeleteGeneric`). It's one node: one piece of code.
- **The answer key had four bugs of its own:**
  - `addr2line` attributes a header's inline function to its caller's file;
  - readelf prints the first line table's offset as `0`, not `0x0`;
  - wide-mode readelf puts the form before each value;
  - long files' line numbers are printed in hex.

## Robustness

Debug sections are now targeted by `tests/tools/fuzz_elf.py`, which flips bytes inside
`.debug_*`. 1,200 debug-form mutants ran locally with no crashes. Of 40 targeted ones, 11 hit the
reader's error path: a clean message, and analysis without debug info.

## Not covered

Compressed debug sections (`SHF_COMPRESSED`, reported) and split DWARF (`.dwo`); paths when the
build root differs from the analysed directory (a future `--source-root` option); C++ names.
