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
