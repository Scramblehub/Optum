# `optum diagnose`

`optum diagnose <dir>` reads a codebase (or a saved graph) and reports waste: things a business
pays for twice. Markdown is for people and JSON for agents (`-f json`). It's the concept doc's
diagnostic. Each analysis in [Phase 2](plans/phase2.md) adds a section to the report.

```sh
optum diagnose tests/fixtures/programs/mixed/crm_glue \
    --adapter "python3 Core/adapters/n8n/optum_n8n.py"      # a directory, with an n8n adapter
optum graph big/repo -o big.json && optum diagnose --graph big.json -o report.md   # a saved graph
optum diagnose . -f json -o diagnosis.json                   # for an agent or a script
```

## What the report holds

- **Summary:** units by frontend, the outside systems reached, tables, packages and findings.
- **Findings:** for each one, the kind, confidence and evidence (a table showing who reaches what, and how).
- **What the code reaches:** every outside API, service and table, with the components that use it.
  This is the inventory, even when there are no findings.
- **Third-party packages and libraries** the code imports.

Findings so far: **overlapping glue**, **unused code** and **code only tests use**. Duplicated
logic and the same data defined in several places come next ([plan](plans/phase2.md)).

## Overlapping glue

Several tools doing the same job: syncing the same API into the same table, or calling the same
service. It's found by the shared node ids: `http:host/path`, `sql:table` and `saas:service` are
the same node whichever language or workflow reaches them ([ir.md](ir.md)).

**Units are components,** what a person would call one tool:

| Id | Component |
|---|---|
| `py:inventory.reader`, `py:inventory.writer` | `py:inventory` (the top-level package) |
| `js:src/a.js`, `js:src/b.js` | `js:src` (the top-level directory) |
| `py:sync`, `js:sync.js` (a root-level file) | itself |
| a program, an n8n workflow, a binary | itself |

So a package's own modules sharing a table are one tool, not a finding.

**Test code is left out of findings.** Test code reaches what the code under test reaches, and that
isn't waste. A function counts as test code if its id has a segment `test`, `tests`, `testing`,
`spec` or `__tests__`, or a segment starting `test_` or ending `_test`/`_tests`. Test code still
appears in the inventory.

**Rules:**

1. Components that share **at least 2** outside systems, where the shared ones are **at least half**
   of each component's outside systems, are grouped together (transitively). Each group of 2 or more
   is a finding: "N tools do the same job".
2. A single outside system reached by 2 or more components that rule 1 didn't already group
   together is a finding: "N tools reach X". A shared table is an example: two tools that
   both write customers may disagree about them.

**Confidence:** `certain` if every edge in the evidence is `exact`; `likely` if any is `inferred` or
`observed` (for example a URL with a hole, `deals/{}`). Findings are sorted by confidence first, then by
the number of components, then by the number of outside systems.

## Unused code

Definitions nothing in the project uses. The rules come from Magellan's `orphaned-definition`
and are tuned for precision: a report that cries wolf won't be read twice. A finding says "no
static reference found", never "dead": code that looks names up at runtime can't be seen.

A function or class is reported when all of these hold:

1. **It's local:** code outside the project can't reach it by name (`attrs.local`, set by the
   frontends). In Python: a `_name` (not a dunder) or a function nested in a function. In JS: a
   top-level definition its file doesn't export, a nested one, or a `#private` member. In C: a
   `static` function in a `.c` file (one in a header is for everyone who includes it). Public API
   isn't reported: callers outside the project can't be seen.
2. **Nothing else calls it:** no `calls` edge from another node, whatever its confidence.
3. **Its name appears nowhere else** (`attrs.referenced` isn't set). The frontends count every name
   in the project's code other than at a definition: values (callbacks, dispatch tables), attributes,
   calls they couldn't resolve, strings that are exactly the name (`getattr(x, "name")`,
   `__all__`), names in code inside strings (templates), and words in C macro bodies and in files C
   code includes without parsing (generated `.inc` tables). Names built at runtime count too:
   a string prefix (`"_handle_" + kind`, `"_cmd_%s"`, `f"_get_{x}"`, or a plain string that 3+
   definitions start with), and the literal after an f-string hole (`f"_{x}Presentation"`).
   Also counted as used: Python functions with a decorator (it receives them), methods of a class
   with a base outside the project (that base may call them by name), and names a project binary
   carries as a string (a C extension looking up a Python helper; `attrs.referenced_from: "binary"`).
4. **It isn't called by name by convention:** dunders, `constructor`, `main`, IPython's `_repr_*_`,
   and methods named like framework hooks (`visit_`, `do_`, `handle_`, `on_`, `test_`, `setUp`,
   `run`...).
5. **It isn't test code**, and isn't inside a definition already reported.

**Used only by tests:** the same, except that test code calls it or names it
(`attrs.referenced_by_tests`): the product never runs it.

One finding per component, listing the definitions with their sizes. Confidence: `likely`.

## How it's checked

Each sample has an answer key with planted findings, `expected_diagnosis.json`, which
[check_diagnosis.py](../tests/tools/check_diagnosis.py) compares with the report on (kind, units,
targets). Any missing or extra finding fails the test.

| Sample | Planted |
|---|---|
| [mixed/crm_glue](../tests/fixtures/programs/mixed/crm_glue) | 1 finding: Python, JS, C and an n8n workflow syncing shop orders into a CRM |
| [mixed/shared_table](../tests/fixtures/programs/mixed/shared_table) | 1 finding: a Python report and a JS signup both on `customers` |
| [mixed/one_package](../tests/fixtures/programs/mixed/one_package) | no finding: one package's modules and its tests share a table |
| [mixed/unused](../tests/fixtures/programs/mixed/unused) | 7 unused definitions in Python, JS and C, 1 used only by tests, and 20 decoys that must not be reported (dispatch table, decorator, `getattr`, `__all__`, prefix and suffix dispatch, generated code, outside base, callback, export, macro, `.inc` table, function-pointer table...) |

On real code (the installed Python packages, `/usr/lib/python3/dist-packages`), the first version
reported 2 findings, both false: a package's own modules, and a package's tests. The component and
test-code rules took that to 0.

### Unused code on real code

Each real-code check has an independent check: a runtime trace (anything the tracer sees called
is a false finding), the compiler, or a hand check of every finding or of a random sample.

| Code | Findings | Check | Result |
|---|---|---|---|
| Python standard library | 20 | runtime trace of the stdlib workload | none seen called |
| dist-packages (Python) | 76 | random sample of 25, hand-checked | 25 real; 10 known false overall (below) |
| capstone (C, 400k lines) | 2 | GCC `-Wunused-function`, every architecture | GCC finds none; Optum's 2 are inside `#if 0`, which GCC never compiles |
| npm (JS) | 0 | npm lints with ESLint's `no-unused-vars` | agrees |

Getting there, the false findings were the rules above: first 108 on the standard library (string
prefixes, test-only use, functions CPython's C core calls by name), 658 on capstone (generated
`.inc` tables), and on dist-packages overrides of outside bases, code in strings, and numpy's C
looking up helpers by name. **Known false findings left:** 8 in LibreOffice's `uno` (its C++ binary,
outside the analysed tree, calls them by name) and 2 in numpy (one kept for unpickling old files,
one named from Cython without a plain string).

## JSON

`report_version` is `"0.1"`. Top level: `source`, `units` (with frontend and lines), `findings`,
`reached` and `packages`. A finding is `{kind, title, confidence, units, targets, evidence}`, and
`evidence.targets[].uses[]` gives each use's `unit` (component), `function`, `kind`
(reads/writes/sends_to) and `method` (empty when there isn't one). For `unused` and `test_only`
findings, `units` is the component, `targets` the definitions' ids, and `evidence.definitions[]`
gives each one's `id`, `kind`, `file`, `line` and `lines`, with `evidence.lines` the total.
