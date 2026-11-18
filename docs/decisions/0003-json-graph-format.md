# 0003: The system graph is versioned JSON

*Accepted 2026-11-18.*

## Context
Every frontend (built-in, external or proprietary) must produce the same graph, and that graph has
to be usable by other tools: viewers, diff tools and AI agents run by us or by customers.

## Decision
- The graph (the IR) is **JSON with a published JSON Schema**, documented in `docs/ir.md`, and
  carries a `version` field.
- External adapters talk to Optum by writing this JSON to stdout. That is the communication layer
  for private, proprietary and non-compiled languages.
- The core may use a faster binary form internally or for very large graphs later, but JSON stays
  the exchange format.

## Later
A **human-readable layer** on top of the JSON (a compact text form, close to the company language
in the concept doc) is a separate later job. JSON is for machines and agents first.
