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

Today there's one kind of finding, **overlapping glue**. Unused code, duplicated logic and the same
data defined in several places come next ([plan](plans/phase2.md)).

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

## How it's checked

Each sample has an answer key with planted findings, `expected_diagnosis.json`, which
[check_diagnosis.py](../tests/tools/check_diagnosis.py) compares with the report on (kind, units,
targets). Any missing or extra finding fails the test.

| Sample | Planted |
|---|---|
| [mixed/crm_glue](../tests/fixtures/programs/mixed/crm_glue) | 1 finding: Python, JS, C and an n8n workflow syncing shop orders into a CRM |
| [mixed/shared_table](../tests/fixtures/programs/mixed/shared_table) | 1 finding: a Python report and a JS signup both on `customers` |
| [mixed/one_package](../tests/fixtures/programs/mixed/one_package) | no finding: one package's modules and its tests share a table |

On real code (the installed Python packages, `/usr/lib/python3/dist-packages`), the first version
reported 2 findings, both false: a package's own modules, and a package's tests. The component and
test-code rules took that to 0.

## JSON

`report_version` is `"0.1"`. Top level: `source`, `units` (with frontend and lines), `findings`,
`reached` and `packages`. A finding is `{kind, title, confidence, units, targets, evidence}`, and
`evidence.targets[].uses[]` gives each use's `unit` (component), `function`, `kind`
(reads/writes/sends_to) and `method` (empty when there isn't one).
