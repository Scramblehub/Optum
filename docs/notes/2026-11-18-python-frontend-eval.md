# Python frontend: first evaluation (2026-11-18)

## On the sample programs

| Program | Edge precision | Edge recall |
|---|---|---|
| basic_calls, classes, imports, http_glue, sql_store | 100% | 100% |
| dynamic | 100% | 76% (misses its 5 hard edges, by design) |
| **All Python samples (105 edges)** | **100%** | **95%** |

These programs were written alongside the conventions, so they flatter the frontend. The real-code
numbers below matter more.

## On real code: the Python 3.12 standard library

- **Size and speed:** 571 files, 285k lines → 16.7k nodes, 37k edges in about 0.15 s
  (release build, 32 cores, peak 220 MB). The sanitizer build runs clean on the same input.
- **Recall:** standard-library workloads run under a tracer (`tests/tools/stdlib_recall.py`, now `Core/tracers/python_trace.py` with `optum verify`):
  **243 of 260 calls written in the source were in the graph (93.5%).**
  - Started at 67%. Most of that gap was the tracer counting things that aren't calls in the
    source (class bodies, operators, `@property`). Filtering those gave 92.7%; typing
    `self.<attr>` assignments raised it to 93.5%.
- **What's still missed:**
  - calls on function parameters of unknown type (`source.match()`): needs type hints or inference
  - decorator factories (`@lru_cache(...)`, `@wraps(...)`) and registry/table dispatch: hard edges
  - attributes holding a value returned by a function, not a constructor
  - 5 misses are a tracer artifact (constructors called inside another `__init__`)
- **Precision:** can't be measured by running code, since the graph lists calls that *may*
  happen. Hand samples of the guessed edges, 24 in total, found one problem: guessing by unique
  method name linked every `s.startswith()` to `collections.UserString`. Fixed by never guessing for
  names that builtin types use. After the fix, the sampled guesses all looked right.

## Guessed edges, by kind (stdlib)

| `attrs.via` | Edges | Meaning |
|---|---|---|
| `local_type` | ~890 | Receiver's class known from an assignment (`x = Cls()`, `self.x = Cls()`) |
| `override` | ~760 | `self.m()` may run a subclass's override |
| `unique_method_name` | ~905 | Receiver unknown; one project class has a method by that name |

Out of about 18k call edges, 82% are `exact`.

## Next steps this suggests

- Use type hints (`def f(source: Tokenizer)`): cheap, and common in modern business code.
- Track return types of project functions for attributes and locals (`x = make_client()`).
- `feat/trace-verify` should turn the prototype script into a proper tool, fixing the `__init__`
  artifact.

## Update (v0.1)

`optum verify` with the new tracer (`docs/verify.md`) counts every call made by a call instruction,
indirect ones included. The text filter of the prototype dropped those. On the same workloads that
gives **537 observed calls and 70.6% recall**: the old 93.5% was over 260 calls, without the hardest ones.

## Update (2026-11-19): following values

The misses on the standard-library workload were mostly calls the frontend could have followed
statically: through a parameter, a return value, a callback, a class body, or what Python runs
when a class statement executes. Each step below was measured on the same trace (530 observed
calls, the v0.1 tracer):

| Step | Recall |
|---|---|
| main (v0.1) | 71.5% |
| Class bodies are a scope of their own (their calls belong to the class, as at runtime; names resolve there first) | 76.2% |
| Protocol calls: `len(x)`, `list(x)`, `for v in x`... reach `__len__`, `__iter__`, `__next__` | 77.2% |
| Parameter defaults; `self.x()` holding a function or class | 77.5% |
| What call sites pass to parameters (functions, classes, instances), over up to 3 calls | 80.0% |
| Callbacks given to `map`/`sorted`/`re.sub`...; mixins calling what only their subclasses have; class attributes | 81.7% |
| Return values: closures, factories, decorator factories | 83.6% |
| Class creation (metaclass `__new__`/`__prepare__`, `__init_subclass__`, `__set_name__`) and metaclass `__call__` | **85.1%** |

The tracer also got a fix: a constructor call is recorded as a call to the class actually called
(`Sub()` with an inherited `__init__` was recorded as the base class), and `__new__` constructors
are mapped like `__init__`. That gives 533 observed calls; on that trace main has **73.4%** and
this version **86.9%**.

- **Precision:** every new edge is `inferred`, with `attrs.via` saying how it was found. In a hand
  check of 36 sampled new edges (6 each of `argument`, `return_value`, `callback`,
  `param_default`, `attr_value`, `subclass`) and 12 `class_creation`/`metaclass` edges, none was
  wrong. The sample programs still score 100% precision. A new sample, `flows`, covers each rule;
  `dynamic` now finds 2 of its 5 hard edges (decorator factory, callback through a parameter).
- **Speed:** unchanged (1.15 s for the standard library); the parameter flows add up to 3 more
  resolution passes, which are cheap next to parsing.
- **Still missed (70):** dispatch tables (`self._dispatch[type(x).__repr__]`), `getattr`,
  generators run by whoever iterates them, descriptors (`cached_property.__get__`), enum and typing
  internals (metaclass machinery several levels deep), and fallbacks for C modules
  (`try: from _x import f / except ImportError: def f`, where the Python fallback is the only
  definition the frontend sees).
