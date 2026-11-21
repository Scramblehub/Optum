# The system graph format (IR) 0.1

Every Optum frontend writes this format, and so must any external adapter for a private or
proprietary language ([decision 0003](decisions/0003-json-graph-format.md)). It's plain JSON, so
any tool or AI agent can read it.

- Machine-checkable shape: [ir.schema.json](ir.schema.json) (JSON Schema 2020-12)
- Full check, including ids and edges: `optum check file.json`
- Example: [tests/fixtures/ir/valid/order_sync.json](../tests/fixtures/ir/valid/order_sync.json)

## Document

```json
{
  "ir_version": "0.1.0",
  "source": "what this graph describes (path, repo URL)",
  "nodes": [ ... ],
  "edges": [ ... ]
}
```

`ir_version`, `nodes` and `edges` are required. Unknown fields are rejected everywhere except
inside `attrs`, so typos in an adapter's output show up at once.

## Nodes

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | Unique, stable string. Convention: `<namespace>:<qualified name>` (below) |
| `kind` | yes | One of the node kinds below |
| `name` | no | Short display name |
| `loc` | no | Where it was found (below) |
| `frontend` | no | What produced it: `python`, `js`, `c`, `binary`, an adapter's name |
| `confidence` | no, default `exact` | `exact`, `observed` or `inferred` (below) |
| `attrs` | no | Object of frontend-specific extras: anything goes |

| Kind | Is |
|---|---|
| `program` | A runnable whole: an executable, a service, a script |
| `module` | A file, package, namespace or class-like container |
| `function` | Anything callable: function, method, procedure, binary routine |
| `data_type` | A record or entity: class, struct, table row type, API payload |
| `store` | Where data rests: database table, file, queue, bucket |
| `external` | Something outside the analysed code: an HTTP API, a SaaS endpoint |
| `step` | A step in a non-code tool: an n8n node, a Zapier action |

## Edges

| Field | Required | Meaning |
|---|---|---|
| `from`, `to` | yes | Node ids; both must exist in the same document |
| `kind` | yes | One of the edge kinds below |
| `loc`, `frontend`, `confidence`, `attrs` | no | As for nodes |

| Kind | From → to |
|---|---|
| `calls` | function → function it calls |
| `imports` | module → module it imports |
| `defines` | container → what it contains (module → function, program → module) |
| `reads` | code → store or external it reads from |
| `writes` | code → store it writes to |
| `sends_to` | code → external it sends data to (data leaves the system) |
| `triggers` | step → the step or program it starts |

There is at most one edge per (`from`, `to`, `kind`). Call counts and the like go in `attrs`.

## Locations

```json
{ "file": "order_sync.py", "line": 30, "end_line": 41 }
{ "file": "sync_helper", "address": "0x401136" }
```

`file` is required; lines start at 1 (0 or absent means unknown). `address` is for binaries and is
a hex **string**, because JSON numbers above 2^53 lose precision in most tools.

## Confidence

| Value | Means |
|---|---|
| `exact` | Read directly from source or symbols; no guessing |
| `observed` | Seen happening at runtime (traces) |
| `inferred` | Deduced: an HTTP URL built from a variable, a call through a pointer |

Later phases only rewrite parts of the graph they're sure of, so frontends should be honest here.

## Id conventions

Ids must be stable between runs, so graphs from different runs and frontends can be merged.

| Namespace | Example |
|---|---|
| `py:` | `py:billing.invoice.send` |
| `js:` | `js:src/api/orders.ts#createOrder` |
| `c:` | `c:src/util.c#checksum` |
| `bin:` | `bin:sync_helper!0x401136` (binary!address) |
| `http:` | `http:crm.example.com/api/deals` |
| `sql:` | `sql:public.orders` |
| `n8n:` | `n8n:<workflow id>/<node name>` |

## Merging

`optum merge a.json b.json` (and `Graph::merge` in C++) joins graphs by node id:

- A node id that appears twice must have the same `kind` both times, or it's an error.
- `name`, `loc` and `frontend`: the first non-empty value is kept.
- `confidence`: the stronger one is kept (`exact` > `observed` > `inferred`).
- `attrs`: combined key by key; for the same key, the later graph's value wins.
- Edges merge by (`from`, `to`, `kind`) with the same rules.

## Versioning

`major.minor.patch`. A build reads any version with the same `major.minor` as its own. While the
major version is 0, a minor bump means a breaking change.

## Known limits (0.1)

- **Speed and memory.** Loading a 1M-node, 3M-edge graph (337 MB) takes about 11 s and 3.9 GB;
  writing it back takes about 21 s (release build, 2026-11-18). The cause is building a full JSON
  tree in memory. A streaming reader and writer is planned (`perf/ir-json` in the roadmap).
- **No human-readable form yet.** Planned later, per decision 0003.
