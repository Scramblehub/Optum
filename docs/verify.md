# Checking a graph against what ran

`optum graph` reads code without running it. `optum verify` checks the result against calls
observed while the code actually ran: how many of them the graph has, and why it misses the rest.

```sh
# Python: run a program (or a test suite) under the tracer
python3 Core/tracers/python_trace.py --root app/ -o trace.json -- app/main.py --some-flag
python3 Core/tracers/python_trace.py --root app/ -o trace.json -m pytest tests/

# JavaScript: profile with V8, then convert the profiles using the graph
node --cpu-prof --cpu-prof-interval 5 --cpu-prof-dir prof/ app/index.js
optum graph app/ -q -o graph.json
python3 Core/tracers/js_profile_trace.py --graph graph.json --root app/ --profiles prof/ -o trace.json

optum graph app/ -q -o graph.json
optum verify graph.json trace.json            # or --json, and --min-recall 0.9 for CI
```

The report gives the observed calls, how many are in the graph (`exact` or `inferred` edges), and
lists the missed ones by reason:
- **no edge between the two nodes:** both functions are known, but the graph doesn't connect them
  (typically a call through a callback, a dispatch table or `getattr`);
- **the callee isn't a node,** or **the caller isn't a node:** something the frontend didn't
  recognise as a function.

## The trace format

Any tool can write one, so other languages can be checked too:

```json
{"trace_version": "0.1", "source": "app/", "calls": [{"from": "py:app.main", "to": "py:app.util.load", "count": 3}]}
```

Ids are the graph's node ids. Self-calls are ignored, and repeated pairs count once.

## What the tracers record

| | Python (`python_trace.py`) | JavaScript (`js_profile_trace.py`) |
|---|---|---|
| How | `sys.setprofile`, every call, in-process | V8 CPU profiles: sampled, so rare calls can be missed |
| Counted | every call made by a call instruction, direct or indirect (callbacks, dispatch tables, `getattr`) | parent → child frames of the project's named functions |
| Left out | class bodies running, operators, property access: not calls in the source | getters and setters; frames not at a function's start |
| Constructors | `Cls()` → the class actually called (read from the running `__init__`/`__new__`, so `Sub()` stays `Sub` when it inherits its constructor); `super().__init__()` → that `__init__` | a constructor frame → its class, unless the graph has an explicit `super(...)` call |
| Ids | as the Python frontend names them, `@def` included | from the graph, by file and line |

Known limit: in Python, `init = super().__init__; init(...)` is recorded as a call to the class.

## Results so far

| Code | Observed calls | Recall | Notes |
|---|---|---|---|
| Python samples | | 100% | except `dynamic`: 50%, the 5 hard edges by design |
| Python 3.12 standard library (`tests/tools/stdlib_workload.py`) | 533 | **86.9%** | v0.1 was 70.6% (73.4% on today's trace); see the [Python note](notes/2026-11-18-python-frontend-eval.md) |
| npm (11 offline commands) | 105 | 79.0% | most misses: commands loaded by computed name |
