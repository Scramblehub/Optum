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
- **For the unused-code analysis** ([diagnose.md](diagnose.md#unused-code)), every Python, JS and C
  function and class node may carry:
  - `attrs.local: true`: code outside the project can't reach it by name (Python `_name` or nested;
    JS not exported, nested or `#private`; C `static` in a `.c` file);
  - `attrs.referenced: true`: its name appears in the project's non-test code other than at a
    definition, including strings, names built from a prefix or suffix, C macro bodies and
    `#include`d non-C files; also decorated Python functions and methods of a class with a base
    outside the project. `attrs.referenced_from: "binary"` when a project binary carries the name;
  - `attrs.referenced_by_tests: true`: the same, but only in test code.

  These are counts of names, not resolved references: generous on purpose, so a definition without
  `referenced` is very likely unused.
- **For the same-entity analysis** ([diagnose.md](diagnose.md#the-same-data-in-several-places)):
  `attrs.fields` on Python and JS class nodes (annotated class attributes, attributes set on
  `self`/`this`, JS class fields; 2 or more), `attrs.columns` on SQL `reads`/`writes` edges (the
  columns the statement names for that table), and `attrs.fields` on HTTP `sends_to` edges (the
  keys of the JSON or form body).

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
| A definition with the same dotted name as a submodule (a package's `main()` next to its `main.py`) | as above, with `@def` appended | `py:pip._internal.main@def` |

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
| Decorator `@d(arg)` on a function | the decorated scope `calls` `d`, and also the function `d` returns (`attrs.via: "return_value"`) |
| A class body's own statements (`PATTERN = re.compile(...)`, decorators on methods) | `calls` from the class node: the class body is a scope of its own, as at runtime. Its names resolve in the class body first |

**Following values (all `confidence: inferred`, with `attrs.via`).** A value's class or function is
also known through:

| `attrs.via` | How | Example |
|---|---|---|
| `param_default` | a parameter never reassigned is taken to be its default | `def urlencode(q, quote_via=quote_plus): quote_via(k)` |
| `argument` | what call sites pass to a parameter: a function or class (every one passed, up to 8), or instances of a class (when all call sites agree). Followed through up to 3 calls | `apply(shout, x)` makes `fn(text)` in `apply` call `shout` |
| `return_value` | what a function returns: a nested function, a class, or an instance | `get = self._formatter(a); get(1)`; `x = make_client(); x.send()` |
| `attr_value` | `self.x()` where `self.x` holds a function or class (`self.x = f`, or a class attribute `x = Cls`) | `self._address_class(n)` |
| `subclass` | `self.m()` where `m` is in neither the class nor its bases, but in its subclasses (mixins, abstract bases); each one found, up to 4 | `socketserver.ForkingMixIn` calling `self.close_request()` |
| `callback` | a project function passed to `map`, `filter`, `sorted`, `min`, `max`, `reduce`, `re.sub`/`subn` or `list.sort` (`key=`, `repl=`) is called from the caller | `sorted(xs, key=by_name)` |
| `class_creation` | a class statement runs its metaclass's `__prepare__`/`__new__`/`__init__`, the nearest base's `__init_subclass__`, and `__set_name__` of class attributes that have one; edges from the scope running the statement | `class Color(Enum)` calls `EnumType.__new__` |
| `metaclass` | `Cls()` also calls `__call__` of a project metaclass | `Counted(3)` with `metaclass=Meta` |

**Protocol calls.** `len(x)`, `iter`, `next`, `str`, `repr`, `int`, `float`, `bool`, `hash`, `abs`,
`bytes`, `reversed`, `format`, and iteration (`for v in x`, `list(x)`, `tuple`, `set`, `frozenset`,
`sorted`, `sum`, `min`, `max`, `any`, `all`) call the matching dunder on `x`'s class (`__len__`;
`__iter__` and `__next__`) when `x` is an instance of a known project class. The edge is resolved
like `x.__len__()` and carries `attrs.protocol` (`"len"`, `"for"`...).
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
| A function returning a module, class, function or instance (`static load(n) { return require(...) }`, factories) | calls through the result are followed; `confidence: inferred`, `attrs.via: "return_value"` |
| `require(\`./commands/${name}.js\`)`: a relative path with holes | every project file the pattern matches (a hole matches a file name, not `/`; at most 200). Constructing or calling what it exports, and methods on those instances, give an edge to each; `inferred`, `attrs.via: "computed_require"` |
| A project function passed to `map`, `filter`, `forEach`, `reduce`, `sort`, `find`, `some`, `every`, `flatMap`, `replace`, `Array.from`... | `calls` it from the caller (they run it synchronously); `inferred`, `attrs.via: "callback"`. Not `.then`: the callback runs later, from elsewhere |

## C

Files: `.c` and `.h`. There's no preprocessor run, so macros aren't expanded in general. The
exceptions: string macros, function calls inside function-like macros, `#define a b` renaming a
function, and `#if`s, which are evaluated for one build configuration (below).

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
| `MACRO(...)` naming a function-like macro of the project | `calls` to the project functions its body calls, followed through nested macros; `attrs.via: "macro"`. Every `#define` of the macro counts (`#ifdef X / #define M ... / #else / #define M ...`), each with its own `#if` |
| `f()` where `#define f g` is in force | `calls` to `g`. The edge to a function `f`, if there is one, is `inferred`, `attrs.inactive`, `attrs.condition: "!defined(f)"` |
| A call through a function pointer (`fp(x)`, `ops->run(x)`, a parameter) | `calls` to every function whose address is taken somewhere (named outside a call) **with the same number of parameters**; at most 12, else none. `inferred`, `attrs.via: "function_pointer"` (hard) |
| A call inside `#if` / `#ifdef` / `#else` | as above, with `attrs.condition` (e.g. `"ZLIB_DEBUG"`, `"not (HAVE_MEMCPY)"`): it happens in some build configurations |
| A call that isn't compiled in the build configuration: inside an `#if` that's off, through a macro `#define`d in one, or to a function defined in one (and nowhere else) | `inferred`, `attrs.inactive: true`, `attrs.condition` naming the `#if`s that are off. When the function is compiled out, a function-like macro of the same name is followed instead (`#ifdef ZLIB_DEBUG / void send_bits(...) / #else / #define send_bits(...)`) |
| libc and other libraries' functions | none |
| `curl_easy_setopt(h, CURLOPT_URL, url)` | `reads` / `sends_to` an `http:` node. The method comes from the function's other options (`CURLOPT_POST`/`POSTFIELDS` → POST, `CUSTOMREQUEST "PATCH"`, `UPLOAD` → PUT, `NOBODY` → HEAD; default GET). URLs from literals, `#define` strings, string constants, `"lit" MACRO` concatenation and `snprintf`/`sprintf` into a local buffer (conversions become `{}`) |
| `sqlite3_exec` / `sqlite3_prepare*`, `PQexec` / `PQexecParams` / `PQprepare`, `mysql_query` with SQL text | `reads` / `writes` `sql:<table>` nodes |

When several call sites link the same two functions, the edge comes from the most certain one:
`exact` before `inferred`, compiled before not, outside any `#if` before inside.

An `#if` around `else if (...) { ... }` in a function parses as a function named `if`; its calls
are kept as the enclosing function's.

### The build configuration

`#if` structure is read line by line from the directives, not from the syntax tree (tree-sitter's
error recovery on macro-heavy code can stretch an `#ifdef __cplusplus` over the rest of a header).
Each `#if` is then evaluated per translation unit (a `.c` file and the headers it includes) with
three-valued logic: on, off or unknown. Unknown counts as on, so an edge is only demoted when the
`#if` is known to be off.

- `compile_commands.json` at the root or in `build/`, if there is one: the `-D` / `-U` flags each
  file is compiled with come first.
- A name the project `#define`s is defined when one of the `#define`s the translation unit can
  see is in a branch that's on. While that's decided the name counts as not yet defined, so
  include guards and `#ifndef X / #define X default` hold.
- A name nobody `#define`s and the code never uses as a value is a build option that's off
  (`ZLIB_DEBUG`, `NO_GZIP`): the default is a build with no `-D` flags. One the code uses
  (`O_CLOEXEC`) comes from a system header: unknown. One `#define`d only in a header this file
  doesn't see: unknown.
- The compiler and platform are GCC or Clang on Linux: `__STDC__`, `__STDC_VERSION__` (C17),
  `__GNUC__`, `__linux__`, `__unix__` are defined; other platforms' and compilers' names (`_WIN32`,
  `_MSC_VER`, `__APPLE__`, `__MSDOS__`...) aren't; other reserved names (`__x86_64__`,
  `__aarch64__`...) are unknown. Constants of `limits.h`, `stdint.h`, `stdio.h` and `stdlib.h`
  (`INT_MAX`, `SIZE_MAX`...) are defined, value unknown, where the header is included.
- Arithmetic, comparisons, `defined`, `&&`, `||`, `?:` are evaluated; a function-like macro in an
  `#if` is unknown. An `#undef` of a name makes it unknown.

Binaries (`docs/plans/binary.md`) map functions with debug info to these same `c:` ids, so a
binary's graph can be scored against, and merged with, its source graph.

## Binaries (ELF, x86-64)

Files are found by content (the ELF magic), not by name: executables and shared libraries. Object
files, other architectures and malformed files are skipped with a message. Plan:
[plans/binary.md](plans/binary.md); ids and confidence: [decision 0005](decisions/0005-binaries.md).

| Thing | Node | Id example |
|---|---|---|
| The binary | `program` (it has an interpreter, PIE included) or `module` (a shared library); `attrs.soname` | `bin:minigzip`, `bin:libz.so` |
| A function with debug info | `function`, its **source id**; `loc` is the source file and line (and the address), `attrs.address`, `attrs.binary` | `c:gzlib.c#gz_open` (`c:minigzip.c#gz_compress.cold` for a split-off piece) |
| A function with a symbol | `function`, `loc.address`, `attrs.size`, `attrs.found` | `bin:minigzip!gz_compress` (`!name@0x…` when two share a name) |
| A function without one | `function`, `confidence: inferred` if found only by the gap rule | `bin:minigzip!0x4a20` |
| A library from `DT_NEEDED` | `module`, `attrs.third_party` | `lib:libc.so.6` |
| An imported function | `function`, `attrs.imported` | `lib:libc.so.6!printf` |

**Where functions come from** (`attrs.found`), best evidence first: `symbol` (`.symtab`) or
`export` (`.dynsym`), `eh_frame` (unwind entries), `entry`, `init_array`, `address_taken`
(pointers stored in data, from relocations), `call_target` (direct calls), and last `gap`: after a
function's final instruction (`ret`, `jmp`, `hlt`, `ud2`, or a call to a no-return import such as
`exit` or `__stack_chk_fail`) and its padding, the next instruction starts a function, unless:
- it lies inside a known symbol's or unwind entry's extent;
- code of the same function jumps there (direct jumps, conditional jumps, and `switch` jump tables:
  `lea r, [rip+table]` before an indirect `jmp`, with 32-bit offsets from the table);
- the binary marks function entries with `endbr64` (`-fcf-protection`) and this code doesn't
  start with it, and isn't jumped to from elsewhere (split-off `.cold` pieces are).

| Code | Edge |
|---|---|
| Binary contains function; library contains imported function | `defines` |
| Binary needs library | `imports` |
| `call f` / `jmp f` to a function start (a tail call: `attrs.via: "tail_call"`) | `calls` |
| `call` / `jmp` through a PLT stub or a GOT slot | `calls` to `lib:<library>!<name>`. The library is the one its symbol version names (`GLIBC_2.2.5` → `libc.so.6`); else the `DT_NEEDED` library that exports it, found like the dynamic linker would (RUNPATH/RPATH with `$ORIGIN`, then the system library directories, on the machine running Optum); else the only `DT_NEEDED`; else `lib:?` |
| A PLT call to a function the binary exports itself (normal in shared libraries) | `calls` to that local function, `attrs.via: "plt"` |
| Calls through registers or memory without a known target | none yet |
| libcurl: `curl_easy_setopt(h, CURLOPT_URL, url)` … `curl_easy_perform(h)` | `reads` / `sends_to` an `http:` node, `attrs.method`. Each `perform` uses the URL and method options (`POST`/`POSTFIELDS`, `UPLOAD`/`PUT`, `NOBODY` → HEAD, `CUSTOMREQUEST "PATCH"`) set since the previous one, so requests stay apart when gcc inlines several functions into one |
| `sqlite3_exec` / `sqlite3_prepare*`, `PQexec` / `PQexecParams` / `PQprepare`, `mysql_query` with SQL text | `reads` / `writes` `sql:<table>` nodes |

**Lifting** (for those two rows): in each function that calls one of these libraries, values are
followed through registers and stack slots in address order:
- constants;
- addresses of string literals (`lea r, [rip+x]`);
- pointers stored in data (`static const char *URL = "..."`, through its relocation);
- local buffers filled by `snprintf` / `sprintf` / `strcpy`, including the `_FORTIFY_SOURCE`
  `__*_chk` versions. printf conversions become `{}`.

Arguments are read from the System V registers at each call. The ids are the same as the source
frontends', so a compiled program and a script calling one API meet on one node.

`optum disasm <binary>` (a research aid) prints every function Optum finds with its instructions,
operands, masked bytes, referenced string literals and call targets, as JSON.

**Source ids from debug info** (DWARF 4 and 5, our own reader):
- **Which entry:** a function's id is `c:<declaring file>#<name>`. The name is the symbol's when
  there is one, so split-off pieces keep `.cold` / `.part.0`. Clones find their name and file
  through `DW_AT_abstract_origin` / `DW_AT_specification`, and split-off ranges map to their
  function.
- **Code without an entry:** a symbol whose code has no debug entry is matched by name to a
  definition without an address, but only if that name is defined in one file. Prototypes
  (`DW_AT_declaration`) never count.
- **Paths:** relative paths are used as recorded (builds with `-ffile-prefix-map`). Absolute paths
  are made relative to the analysed directory when the source lies under it, and kept absolute
  otherwise.
- **One node per body:** copies of one source function (a header's `static inline`, or a `.c` file
  `#include`d into another) share one node, with the other copies' addresses in `attrs.copies`;
  two names folded onto one body get one node.
- **Malformed debug info:** the binary is analysed without it, with a message.

An unoptimised debug build therefore merges node for node with the C frontend's graph of its source,
when both are analysed from the directory the build used as its root.

## Known gaps (0.1)

- No `extends` edge for inheritance: the graph can't yet say "Square is a Shape". Candidate for
  IR 0.2.
- Class attributes, globals and module constants (`HANDLERS`, `SHOP`) aren't nodes.
- C: no preprocessor, so code that only parses after expansion (heavy `EXPORT`/`FAR`-style macros)
  is read through tree-sitter's error recovery; object-like macros that rename functions
  (`#define zmemcpy memcpy`) aren't followed; C++ isn't supported (a `.h` may hold C++).
- JS: object-literal methods aren't nodes; `axios.create({ baseURL })` clients and `tsconfig.json`
  path aliases aren't followed; destructured parameters don't get types from their annotation.
- Binaries: no indirect call targets; lifting ignores control flow and only follows the libraries
  listed; compressed debug sections and split DWARF (`.dwo`) aren't read; x86-64 ELF only; no C++
  demangling.
- n8n: Code nodes' JavaScript and raw SQL queries aren't analysed; sub-workflow calls
  (`executeWorkflow`) aren't linked to the called workflow yet.
