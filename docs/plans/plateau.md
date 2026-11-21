# The plateau loop

*How Optum's graph accuracy is improved on real code without overfitting to the code it's tuned
on. Started 2026-11-20; round 1 measured, round 2 in review.*

Run Optum on real repositories until its accuracy stops improving (a plateau). Find what it would
have needed to get past the plateau. Fix that in a way that generalises, and repeat with new
repositories. Each round does the following:

1. **Measure.** One analyst per language (Python, JavaScript, C) runs a frozen Optum build on its
   *tuning* repositories against ground truth: traced runs for Python and JavaScript, the
   compiler's call graph for C. Every miss and extra edge is classified by cause. Analysts report
   before anything changes.
2. **Separate measurement errors from Optum's.** A miss caused by the tracer or the scorer (a
   profiler skipping a frame, a scorer dropping header-defined callers) is fixed in the tool, not
   in Optum. It is reported apart so it doesn't inflate either side.
3. **Find the real failure and its layer:** parsing, a frontend's language semantics, the IR,
   graph analysis, or evaluation. Fix it at that layer.
4. **Prefer the general fix.** A fix is general when the pattern shows up in at least two
   repositories, or when it implements a rule of the language. Repository-specific patches are
   left out unless they're a real language rule seen once.
5. **A regression test per fix**, in the frontend's test suite: a small fixture showing the pattern.
6. **Re-measure the tuning repositories with `tests/plateau/compare.py`.** The comparison is per
   relationship type (each `via` label: exact, override, return_value, ...) and per cause
   category, never only the total. **Any observed call found before and missed after fails the
   round**, whatever the totals say. A call found both times by a different rule is listed as
   relabelled, usually a guess becoming certain. Added edges that the run never exercised are
   counted, because only a hand check can judge them: 25 per label that gained edges, with the
   source read at each call site.
7. **Evaluate on the held-out repositories** ([tests/plateau/repos.json](../../tests/plateau/repos.json)),
   which no one looks at while designing a fix. A fix that helps the tuning set and does nothing,
   or harm, on the held-out set was fitted to the tuning set. The sealed repositories are kept for
   release checks only.
8. **The batch.** The user reviews the three reports together, and the changes merge as one
   batch after steps 6 and 7 pass.

## When a held-out repository finds a problem

That's what it's for, and it changes its role. The fix is designed on general grounds (often by
restoring behaviour the change broke), the repository is used again to confirm the fix, and it is
recorded as `seen_held_out` in `repos.json`: no longer independent evidence for that fix. A fresh
held-out repository checks it as well.

## What "plateau" means

A repository has plateaued when its remaining misses are measurement artifacts, patterns specific
to that repository, or behaviour that only exists at run time (monkeypatching, code built with
`exec`, callbacks from C). Then it stops being useful for tuning, and a repository in a different
style replaces it: a plugin architecture, dependency injection, event-driven code, generated code,
or heavy use of configuration.

## Rounds

| Round | Fixes | Result on tuning repos | Held-out |
|---|---|---|---|
| 1 | none (measurement) | Python 73.1%, JS 76.8% recall; C 69–100% recall, 56–100% precision | not run |
| 2 | Python: subscripted bases, `cls()`, `__call__`, defaults with arguments, no guessing on stdlib modules; JS: symbol-keyed methods, static/instance ids, class-expression names, `process` builtins; C: in progress. Tracers: Python argv, Sentinel ids, call-site lines; JS skip-level frames set aside | Python 73.7% → 81.0% (same traces), 0 calls lost; JS yargs 78.9% → 88.4%, marked 62.3% → 67.4%, 0 lost | pending |

Round 2's held-out checks: Python better on sphinx (+25 calls) and sympy (+42), 0 lost there, a
regression on django (unique-name edges lost on `obj.query.m()` receivers; being fixed, then
checked on django again and on requests and rich, fresh); JavaScript neutral on express and lodash, a regression
on npm (static methods dropped from unique-name guessing; restored); C better on curl (precision
79 → 97), mixed on redis: conditions set only by the build (generated config headers, `-D`) were
judged off, and `<assert.h>` resolved to a vendored file. Fixed by reading `compile_commands.json`
and resolving angle-bracket includes through include directories only; checked again on curl and
redis, and on libgit2 and jq (fresh).

## Round 3: first items

- **Multi-path edges.** Every rule that finds the same edge is recorded (`via` stays the first;
  `also_via` lists the rest); targets are chosen as today, so precision doesn't move. The gate then
  lists, for a rule about to change, the edges whose *only* support is that rule: the ones at risk,
  known before a change ships (round 2's npm regression would have shown up here). Agreement
  between independent rules becomes evidence in its own right.
- **Overload sets.** A call resolves to the set of candidates for a name, narrowed by what is
  known (argument count, static or instance, receiver type, TypeScript overload signatures,
  Python `@overload` and `singledispatch`): one left is exact, several are inferred with their
  reasons, instead of a single guess or nothing.
- The analysts' carried-over items: Python stored callables and annotations as receiver types;
  JS `x || new C()` fields and functions held in variables; C `#undef`/`#define` in file order,
  computed includes, macros with calls in their bodies.
