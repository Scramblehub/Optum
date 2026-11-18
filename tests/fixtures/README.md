# Fixtures

- `ir/`: graph files for testing the format itself (`valid/` must pass `optum check`; `invalid/`
  must fail it, and `invalid/schema_*` must also fail the JSON Schema).
- `programs/<language>/<name>/`: small sample programs, each with a hand-checked `expected.json`
  following [docs/frontends.md](../../docs/frontends.md). They're the measuring stick for Phase 1:
  `optum score expected.json produced.json`.

## Adding a sample program

1. Write the smallest program that shows one thing (a language feature, a kind of glue, a hard case).
   Say what it shows in the file's first comment. It should run without network access, where possible.
2. Write `expected.json` by hand, following the conventions. Mark edges that need more than name
   lookup with `"attrs": {"difficulty": "hard"}`.
3. If the program runs, check that every call that actually happens is in `expected.json`
   (`feat/trace-verify` will automate this).
4. `ctest` checks the file is valid and the program compiles.
