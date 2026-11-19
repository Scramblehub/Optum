# JavaScript / TypeScript frontend: first evaluation (2026-11-18)

## On the sample programs

| Program | Edge precision | Edge recall |
|---|---|---|
| basic, classes (TS), modules, http_glue, sql_store (TS) | 100% | 100% |
| dynamic | 100% | 81% (misses its 3 hard edges, by design) |
| **All JS samples (105 edges)** | **100%** | **97%** |

## On real code

### npm (JavaScript, CommonJS): 114 files, 14k lines

- **Speed:** 0.04 s.
- **Recall,** from V8 CPU profiles of 11 offline npm commands (`tests/tools/js_profile_recall.py`, now `Core/tracers/js_profile_trace.py` with `optum verify`):
  **83 of 105 observed calls (79%).**
  - It started at 48%. Two real bugs, neither covered by the samples, accounted for most of the
    gap:
    - `const X = require('./x')` followed by `new X()` or `class Y extends X`: the binding
      pointed at the module, not the class it exports.
    - `require('./x')(...)`, a call straight on the result of `require`.

    Both are fixed, and both now have cases in the `modules` sample.
  - Two other kinds of entry looked like misses but aren't calls written in the source: getters
    (now tagged `attrs.accessor`) and constructors (counted as a call to the class).
- **What's still missed (22):**
  - 11: npm's command dispatch. It loads `commands/<name>.js` by computed path and calls `exec()`
    on it. These are hard edges by design.
  - 9: a subclass with no constructor of its own runs an implicit one that calls the parent's.
    This is a profile artifact, not a call in the source. **Excluding these, recall is 86%.**
  - 2: a callback passed to `.sort()`, and a `require()` with a computed path. Both are hard.
- **Precision:** 92% of call edges are `exact`. A hand sample of 16 guessed edges found one wrong
  one: `this.#process.exit()` was linked to the project's `ExitHandler.exit`, but it's Node's
  `process`. The fix: guessing by method name now excludes the methods of Node's global objects
  and core modules (668 names, generated from Node itself).

### ajv (TypeScript): 106 files, 9.8k lines

- **Speed:** 0.02 s; the sanitizer build runs clean.
- **Imports:** 481 of 498 relative imports resolve to a project module (96.6%). All 17 that don't are
  `.json` data files, so every code import resolves.
- **Typed calls:** 162 calls are resolved through TypeScript annotations (`attrs.via:
  "type_annotation"`).
- **Guessed by method name: 293 (20%).** That's high, because ajv passes its code generator around
  as `({gen}: KeywordCxt)`: a destructured parameter whose field type isn't tracked yet. A hand
  sample of 14 found all of them correct (`CodeGen` is the only class with those methods). Typing
  destructured parameters from the annotated type is on the backlog.

## Not covered yet

- Object-literal methods (`const api = { get() {} }`) aren't nodes.
- `axios.create({ baseURL })` clients: their calls aren't linked to the base URL.
- `tsconfig.json` path aliases (`@/lib/x`): imports using them become third-party packages.
- No runtime recall check for TypeScript yet (it needs a compiled run).

## Update (2026-11-19): dispatch, callbacks, implicit constructors

Same 11 npm commands, profiled again (104 observed calls):

| Step | Recall |
|---|---|
| main (v0.1) | 78.8% (82 of 104) |
| Tracer: a class without a constructor runs an implicit one, a frame at the class line. It now counts as the class (`new Sub()`), and its implicit `super(...)` is left out. Before, it showed up as the *module* calling the base class: 9 false misses become what they are, misses of npm's dispatch | 78.8% |
| Return values (`static cmd(c) { return require(...) }`) and computed requires (`require(\`./commands/${command}.js\`)` → every matching file) | 96.2% |
| Callbacks given to `sort`, `map`, `filter`... | **98.1%** (102 of 104) |

- **Precision:** the new edges are all `inferred`. The 145 `computed_require` edges come from two
  lines in `Npm.#exec`: `new Command(this)` reaches the 67 command classes, and `command.exec(args)`
  reaches 78 `exec` methods. That is exactly what npm can run there. In a hand check of the 10
  `callback` edges, none was wrong (one is real recursion: `data.map(cleanup)` inside `cleanup`).
  The sample programs still score 100%, and a new sample, `dispatch`, covers the three rules.
- **Still missed (2):** `validate-engines.js` calling `entry.js` through a `require()` result passed
  around as a value, and one call from a nested object-literal method that the profile attributes
  to `LS.exec`.
