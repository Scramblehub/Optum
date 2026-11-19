# Frontend conventions

What each frontend must produce. The expected graphs in `tests/fixtures/programs/` follow these
rules, and `optum score` measures a frontend against them. When a rule changes, the expected graphs
change in the same commit.

## All languages

- **What an edge means.** The expected graph lists every call that **may** happen (a static,
  "may call" graph), not only the calls one run happens to make. Runtime traces (`feat/trace-verify`)
  check the other direction: every call seen at runtime must be in the graph.
- **Who makes a call.** The innermost enclosing function. Code at the top of a module or file is
  attributed to the module node.
- **Not in the graph:** language builtins and the standard library (`print`, `len`, `json`,
  `printf`). They add noise and no insight into the business. Exceptions are I/O calls that reach
  something outside, which become `external` or `store` nodes (below).
- **Hard edges.** Edges that need more than reading names, such as dispatch tables, `getattr`,
  callbacks and function pointers, are marked in expected graphs with
  `"attrs": { "difficulty": "hard" }`. `optum score` reports recall on them separately, so
  progress on the hard cases stays visible.
- **What is scored (0.1):** node ids and kinds; edges by (`from`, `to`, `kind`). Not scored yet:
  names, locations, confidence, attrs.

## Python

Ids are `py:` followed by the dotted path from the analysed root.

| Thing | Node | Id example |
|---|---|---|
| File `shop/orders.py` | `module` | `py:shop.orders` |
| Package `shop/__init__.py` | `module` | `py:shop` |
| Function | `function` | `py:shop.orders.place_order` |
| Nested function | `function` | `py:plugins.handler.register` |
| Class | `data_type` | `py:shapes.Square` |
| Method | `function` | `py:shapes.Square.area` |
| Third-party package (not stdlib) | `module`, `attrs.third_party: true` | `py:requests` |

| Code | Edge |
|---|---|
| Module defines function or class; class defines method; function defines nested function; package defines its submodules | `defines` |
| `import a.b`, `from a.b import x`, `from .b import x` (relative) | `imports` to the most specific module named (`a.b`); `from a import b` where `b` is a submodule → `a.b` |
| Import of a third-party package | `imports` to its `py:<package>` node |
| `f()` | `calls` to `f`, followed through imports and re-exports to where it's defined |
| `Cls()` | `calls` to the class node (`data_type`). Not to `__init__` |
| `self.m()`, `obj.m()` | `calls` to `m` on the object's class, found through inheritance; for `self` also every override in a subclass (`attrs.via: "override"`) |
| How an object's class is known | `self`; `x = Cls()` in the same scope; `self.x = Cls()` in any method of the class; `Cls().m()` directly. Edges found through `x = Cls()` or `self.x = Cls()` are `confidence: inferred`, `attrs.via: "local_type"` |
| `x.m()` with `x` unknown | `calls` to `m` only if exactly one project class defines `m`, and `m` isn't a dunder or a method name of a builtin type (`get`, `append`, `startswith`...). `confidence: inferred`, `attrs.via: "unique_method_name"` |
| `super().m()` | `calls` to the parent's `m` |
| Decorator `@d(arg)` on a function | the decorated scope `calls` `d`, and also the function `d` returns (hard) |
| `requests` or `httpx` `.get/head/options/post/put/patch/delete(url)` | an `external` node, id `http:<host><path>` (no scheme or query, `{}` for each variable part). URL from a literal, an f-string, a local or a module constant (also imported ones). GET/HEAD/OPTIONS → `reads`, others → `sends_to`. `attrs.method` set |
| `db.execute("<literal SQL>")` | a `store` node per table, id `sql:<table>` (default schemas dropped, see [protocol.md](protocol.md#ids-how-graphs-from-different-tools-join-up)). `SELECT` / `JOIN` sources → `reads`; `INSERT` / `UPDATE` / `DELETE` / `CREATE` targets → `writes` |
| Calls into third-party packages (`stripe.Charge.create()`) | none in 0.1, except the HTTP and SQL rules above |

### Implementation notes

- Module names come from paths relative to the analysed directory, or to the outermost package
  around it if the directory is itself a package (analysing `.../email` gives `email.message`). If an import doesn't match a
  module exactly, a unique module whose name ends with it is used (for `src/`-style layouts).
- Directories skipped: hidden ones, `__pycache__`, `node_modules`, `venv`, `site-packages`.
- Files with syntax errors are still analysed (tree-sitter recovers); `optum graph` reports how many.

## JavaScript and TypeScript

Files: `.js .jsx .mjs .cjs .ts .tsx .mts .cts`, except `.d.ts` and `.min.js`. Ids are `js:` + the
path from the analysed root, then `#` and the dotted name inside the file.

| Thing | Node | Id example |
|---|---|---|
| File | `module` | `js:src/orders.js` |
| Function (declaration, or `const f = () => ...`) | `function` | `js:src/orders.js#placeOrder` |
| Nested function | `function` | `js:app.js#report.fmt` |
| Class | `data_type` | `js:shapes.ts#Square` |
| Method, constructor, class field holding a function | `function` | `js:shapes.ts#Square.area`, `#Square.constructor` |
| Getter / setter | `function`, `attrs.accessor: "get"` / `"set"` | `js:lib/npm.js#Npm.flatOptions` |
| npm package | `module`, `attrs.third_party: true` | `js:axios`, `js:@scope/pkg` |

Anonymous functions (callbacks, object-literal methods) aren't nodes: their calls count for the
nearest named function, or the module.

| Code | Edge |
|---|---|
| `import ... from "./x"`, `require("./x")`, `export ... from "./x"`, `import("./x")` | `imports`. Relative specifiers resolve like Node and bundlers: the path, then `.ts .tsx .js .jsx .mjs .cjs`, then `/index.*`; a `.js` specifier may name a `.ts` file. Node builtins (`fs`, `node:path`) give no edge; other bare specifiers are npm packages, unless they name a project path |
| `f()` | `calls` `f`, through imports, `export`/`module.exports`, re-exports (`export {x} from`, `export * from`) and default imports |
| `new C()` | `calls` the class node. `super(...)` calls the parent's `constructor` |
| `this.m()` | `calls` `m` found through `extends`, plus every override in a subclass (`attrs.via: "override"`). `this` is the class of the nearest method, through arrow functions only |
| `x.m()` with `x`'s class known | From `new C()` (exact, or `local_type` via a variable), `this.x = new C()`, a class field, a TypeScript annotation (`p: C`, `private repo: C` parameter properties; `attrs.via: "type_annotation"`, plus overrides) |
| `x.m()` with `x` unknown | `calls` `m` only if exactly one project class defines it and the name isn't a builtin or Node core method (`push`, `then`, `exit`, `readFile`...); `attrs.via: "unique_method_name"` |
| `fetch(url, { method })`, `axios.get/post/...(url)`, `node-fetch` | `reads` (GET/HEAD/OPTIONS) or `sends_to` an `http:` node. URLs from literals, template strings and constants, also imported ones |
| `.query / .execute / .raw / .prepare / .run / .all / .exec` with SQL text | `reads` / `writes` `sql:<table>` nodes, as for Python |
| Getter and setter access (`npm.flatOptions`) | none: not a call in the source |

## C

Files: `.c` and `.h`. There's no preprocessor run, so macros aren't expanded in general. Two
narrow exceptions follow (string macros, and function calls inside function-like macros).

| Thing | Node | Id example |
|---|---|---|
| Source or header file | `module` | `c:src/util.c`, `c:src/util.h` |
| Function definition (`static` gets `attrs.static: true`) | `function` | `c:src/util.c#checksum` |
| Non-system header that isn't in the project | `module`, `attrs.third_party: true` | `c:<curl/curl.h>` |

Prototypes aren't nodes, but they decide confidence (below). A `static inline` function defined in
a header is a node of the header.

| Code | Edge |
|---|---|
| File defines function | `defines` |
| `#include "x.h"` / `<x.h>` | `imports`: next to the including file, from the root, or a unique project file ending in it. C and POSIX system headers give no edge; others become `third_party` modules |
| `f()` | `calls`, resolved like the linker: the same file, then headers it includes (also through other headers), then a unique non-`static` definition. That last case is `exact` only if the caller sees a prototype for `f`; otherwise `inferred`, `attrs.via: "name_only"`, since it may belong to another program in the repo |
| `MACRO(...)` naming a function-like macro of the project | `calls` to the project functions its body calls, followed through nested macros; `attrs.via: "macro"` |
| A call through a function pointer (`fp(x)`, `ops->run(x)`, a parameter) | `calls` to every function whose address is taken somewhere (named outside a call) **with the same number of parameters**; at most 12, else none. `inferred`, `attrs.via: "function_pointer"` (hard) |
| A call inside `#if` / `#ifdef` / `#else` | as above, with `attrs.condition` (e.g. `"ZLIB_DEBUG"`, `"not (HAVE_MEMCPY)"`): it happens in some build configurations |
| libc and other libraries' functions | none |
| `curl_easy_setopt(h, CURLOPT_URL, url)` | `reads` / `sends_to` an `http:` node. The method comes from the function's other options (`CURLOPT_POST`/`POSTFIELDS` → POST, `CUSTOMREQUEST "PATCH"`, `UPLOAD` → PUT, `NOBODY` → HEAD; default GET). URLs from literals, `#define` strings, string constants, `"lit" MACRO` concatenation and `snprintf`/`sprintf` into a local buffer (conversions become `{}`) |
| `sqlite3_exec` / `sqlite3_prepare*`, `PQexec` / `PQexecParams` / `PQprepare`, `mysql_query` with SQL text | `reads` / `writes` `sql:<table>` nodes |

Binaries (`docs/plans/binary.md`) map functions with debug info to these same `c:` ids, so a
binary's graph can be scored against, and merged with, its source graph.

## Known gaps (0.1)

- No `extends` edge for inheritance: the graph can't yet say "Square is a Shape". Candidate for
  IR 0.2.
- Class attributes, globals and module constants (`HANDLERS`, `SHOP`) aren't nodes.
- C: no preprocessor, so code that only parses after expansion (heavy `EXPORT`/`FAR`-style macros)
  is read through tree-sitter's error recovery; object-like macros that rename functions
  (`#define zmemcpy memcpy`) aren't followed; C++ isn't supported (a `.h` may hold C++).
- JS: object-literal methods aren't nodes; `axios.create({ baseURL })` clients and `tsconfig.json`
  path aliases aren't followed; destructured parameters don't get types from their annotation.
- n8n: Code nodes' JavaScript and raw SQL queries aren't analysed; sub-workflow calls
  (`executeWorkflow`) aren't linked to the called workflow yet.
