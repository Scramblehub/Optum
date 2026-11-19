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

## C

| Thing | Node | Id example |
|---|---|---|
| Source file `main.c` | `module` | `c:main.c` |
| Function (static or not) | `function` | `c:main.c#square` |

| Code | Edge |
|---|---|
| File defines function | `defines` |
| `f()` | `calls`; across files, resolved to the defining file |
| Call through a function pointer | `calls` to every function that may be stored in it (hard) |
| libc (`printf`, `malloc`) | none |

Binaries (`exp/frontend-binary`) map recovered functions to the same `c:` ids through debug
symbols where possible, so the binary graph can be scored against the source graph.

## n8n (external adapter)

[Core/adapters/n8n/optum_n8n.py](../Core/adapters/n8n/optum_n8n.py), run with
`--adapter "python3 Core/adapters/n8n/optum_n8n.py"`. Reads n8n workflow exports (one workflow, or
a list of them, per `.json` file); other JSON is skipped.

| Thing | Node | Id example |
|---|---|---|
| Workflow | `program`, `attrs.active` | `n8n:wf-order-crm` (the workflow's id, else the file name) |
| n8n node | `step`, `attrs.type`, `attrs.trigger` for triggers | `n8n:wf-order-crm/Create deal` |
| SaaS integration (Slack, HubSpot...) | `external`, `attrs.service` | `saas:slack` |

| Workflow content | Edge |
|---|---|
| Workflow contains step | `defines` |
| Connection A → B (any output) | `triggers` |
| HTTP Request step | `reads` (GET/HEAD/OPTIONS) or `sends_to` to `http:...`; `{{ expressions }}` in the URL become `{}` and make it `inferred` |
| Postgres / MySQL / MSSQL step with a table | `reads` (select) or `writes` (insert, update, upsert, delete) to `sql:<table>`. Raw `executeQuery` isn't analysed yet (reported on stderr) |
| Any other non-core node type | `reads` (get/getAll/search operations) or `sends_to` to `saas:<service>` |

Logic and plumbing nodes (If, Switch, Set, Code, Merge, Wait, triggers, sticky notes...) are steps
with no outside edges.

## Known gaps (0.1)

- No `extends` edge for inheritance: the graph can't yet say "Square is a Shape". Candidate for
  IR 0.2.
- Class attributes, globals and module constants (`HANDLERS`, `SHOP`) aren't nodes.
- n8n: Code nodes' JavaScript and raw SQL queries aren't analysed; sub-workflow calls
  (`executeWorkflow`) aren't linked to the called workflow yet.
