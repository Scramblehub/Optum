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

Findings: **overlapping glue**, **unused code**, **code only tests use**, **duplicated logic**
and **the same data in several places** ([plan](plans/phase2.md)).

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

## Duplicated logic

Functions that are copies of each other, found with CGL's structure memory applied to source code
(the level where [the binary study](notes/2026-11-18-bin-skeletons-r3.md) said its skeletons belong). It needs the
source: `optum diagnose <dir>`, or `--graph` when the graph's `source` path still exists.

For every Python, JS and C function, Optum reads its body and computes:

| | What | Finds |
|---|---|---|
| Exact hash | the body's tokens, comments and the function's name aside (and a Python docstring) | **exact** copies |
| Shape hash | the same, with names as a typed hole `ID` and literals as `LIT` | copies with **renamed** names or values |
| Structure memory | the shape of every piece of the body (subtrees of 4 to 40 syntax nodes), counted across the codebase and weighted by rarity (`log(1 + functions / functions with it)`) | **near** copies with small edits |

Two functions are near copies when the weighted overlap of their pieces is at least **0.70** (the
usual line for near-miss clones). Common idioms (`for x in xs: ...append`) are in many functions,
so they weigh little; that's what keeps look-alikes apart (they score under 0.10 in the sample).

Precision rules:
- **Big enough:** 50+ tokens, 5+ lines, and 10+ distinct pieces. A run of `self.x = x` is one
  piece repeated: boilerplate, not logic.
- **Candidates** are pairs sharing 3+ rare pieces (in 2 to 20 functions); sizes within 60%.
- **Every member is similar to every other** (complete link), so loosely similar functions can't
  chain into one group. At most 50 members.
- A function is never a clone of a function nested in it, and a group whose members all sit inside
  members of another group is left out (it repeats that group). Test code is left out.

One finding per group. Confidence: `certain` for exact and renamed copies, `likely` for near
copies. Evidence: the clone type, the lowest similarity between two members, each member's
location and size, and `lines_saved`: the lines removed by keeping one.

## The same data in several places

One entity (a customer, an order) defined again in each place that holds it: a Python class, a
SQL table, a JS class, the payload sent to an API. Each copy can drift from the others; one
definition could replace them. Read from the fields the frontends record
([frontends.md](frontends.md)):

| Shape | Fields from |
|---|---|
| Class (Python, JS) | annotated class attributes (dataclasses), attributes set on `self` / `this`, JS class fields |
| Table | the columns code names for it: `CREATE TABLE`, `INSERT (...)`, `UPDATE ... SET`, a one-table `SELECT` |
| API payload | the body code sends: `requests.post(url, json={...})` (or a name bound to a dict), `axios.post(url, {...})`, `fetch(url, {body: JSON.stringify({...})})` |

Fields are compared by meaning, not spelling: `firstName`, `first_name` and `FirstName` are one
field. Each field weighs by how rare it is across all shapes, and generic fields (`id`, `name`,
`status`, `created_at`...) weigh little, so they can't make two shapes look alike.

**Identity, never coincidence.** A shared name (`Customer` here, `customers` there) or a few
fields in common can be coincidence: a marketing `Customer` segment isn't the CRM's customer, and a
hotel and a restaurant both have a city and a phone. So shapes match only when something
identifies the entity:

1. **A shared identity key**: the entity's own id, canonical across forms (`id` in the
   `customers` table is `customer_id`), a `<name>_id` / `_uuid` / `_key` field, a natural key
   (`email`, `sku`, `username`, `iban`, `external_id`...), or a column declared `PRIMARY KEY` or
   `UNIQUE`. Then 40%+ of the smaller shape's (weighted) fields shared, 2 of them specific.
2. **Else a temporary composite key**: no id in common, but 3+ specific fields every member has,
   covering 75%+ of the smaller shape: together they stand in for an id.

A field like `customer_email` on an order refers to a customer; it isn't the order's identity.
The finding says which basis matched (the shared key, or the fields of the composite).

**Regulated forms stay apart.** Tax, legal and medical records (a W-9, a contract, a consent, a
patient record) are required in their own form and often can't change once issued, so they are
never proposed for merging, however many fields they share. A shape is regulated when its name or
where it lives (module path, table, API path) has a word like `tax`, `vat`, `w9`, `1099`, `irs`,
`legal`, `contract`, `agreement`, `consent`, `gdpr`, `kyc`, `medical`, `patient`, `diagnosis`,
`prescription`, `clinical`, `hipaa`, or when it has a regulated identifier (`ssn`, `tin`, `ein`,
`tax_id`, `vat_number`, `mrn`, `npi`, `icd10`). The report's notes list what was left out.

Groups are complete-link, as for clones, and must span **2 or more forms**: a table, a payload,
or classes in different languages (classes of one language alone are code structure: subclasses,
vendored copies; duplicated logic covers those). Shapes need 3+ fields; test code is left out.

The finding proposes one definition (the fields half the members or more have) and shows each
field as each place spells it. Confidence: `likely`.

## How it's checked

Each sample has an answer key with planted findings, `expected_diagnosis.json`, which
[check_diagnosis.py](../tests/tools/check_diagnosis.py) compares with the report on (kind, units,
targets). Any missing or extra finding fails the test.

| Sample | Planted |
|---|---|
| [mixed/crm_glue](../tests/fixtures/programs/mixed/crm_glue) | 1 finding: Python, JS, C and an n8n workflow syncing shop orders into a CRM |
| [mixed/shared_table](../tests/fixtures/programs/mixed/shared_table) | 1 finding: a Python report and a JS signup both on `customers` |
| [mixed/one_package](../tests/fixtures/programs/mixed/one_package) | no finding: one package's modules and its tests share a table |
| [mixed/entities](../tests/fixtures/programs/mixed/entities) | a customer in 4 places (Python dataclass, SQL table, JS class, CRM payload in camelCase; matched by `email`) and an order in 3 (matched by a composite key); decoys: different data, only generic fields in common, a same-named marketing `Customer`, a hotel and a restaurant with a city and a phone, and regulated forms (a W-9 and its table, a medical record with the customer's fields). The earlier rules merged the medical record into the customer and grouped the W-9 with its table |
| [mixed/dupes](../tests/fixtures/programs/mixed/dupes) | 5 clone families: exact, renamed and edited copies in Python, a renamed copy in JS, near copies in C; decoys with the same idioms but other logic, tiny look-alike helpers, and a copy in test code |
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

### Duplicated logic on real code

- **Recall:** [plant_clones.py](../tests/tools/plant_clones.py) copies 40 random standard-library
  functions (above the size limits) three times each into a copy of the library: exact, with
  every local name and string renamed, and with a statement added. Over two seeds (240 copies):
  exact 95% and 95%, renamed 92% and 98%, edited 95% and 95%. The misses are mostly whole families
  under the distinct-pieces limit (`__eq__` chains of comparisons).
- **Precision:** on the standard library itself, 75 groups (1,658 lines to remove: 8 exact, 29
  renamed, 38 near). A random 15, read side by side, were all real copies: `chunk.py` and
  `wave.py`, the `utf_16`/`utf_32` codecs, heapq's max-heap variants, asyncio's `recv`/`recv_into`.
  Some are deliberate (a max-heap variant), but they are duplicated logic all the same.
  Getting there: boilerplate initialisers (`self.x = x` only) and functions paired with a function
  nested in them were false findings; the distinct-pieces and nesting rules removed them.
- **Speed:** 1.6 s for the standard library's saved graph (it re-reads the source).

### The same data on real code

Real code with classes, tables and API payloads side by side is what Phase 3's test company is
for. dist-packages has 14 tables with columns and no payloads, and gives no group (a first version
without the forms rule gave 577, nearly all classes against classes: vendored copies and
subclasses). It also showed a table bug, now fixed: SQLite's `INSERT OR REPLACE INTO bans` read as
a table named `OR`.

## JSON

`report_version` is `"0.1"`. Top level: `source`, `units` (with frontend and lines), `findings`,
`reached` and `packages`. A finding is `{kind, title, confidence, units, targets, evidence}`, and
`evidence.targets[].uses[]` gives each use's `unit` (component), `function`, `kind`
(reads/writes/sends_to) and `method` (empty when there isn't one). For `unused` and `test_only`
findings, `units` is the component, `targets` the definitions' ids, and `evidence.definitions[]`
gives each one's `id`, `kind`, `file`, `line` and `lines`, with `evidence.lines` the total. For
`duplicated_logic`, `targets` are the functions, and `evidence` has `clone_type` (exact, renamed,
similar), `similarity`, `members[]` (`id`, `file`, `line`, `lines`, `tokens`) and `lines_saved`.
For `same_entity`, `targets` are the class, table and API nodes, and `evidence` has `entity`,
`similarity`, `members[]` (`id`, `kind`, `name`, `fields` as written, `normalized`, `keys`,
`used_by`), `proposed_fields`, `basis` (`identity` or `composite`) and `basis_fields`. The report's
top-level `notes` say what analyses left out on purpose (regulated forms).
