# 0004: How graph accuracy is measured

*Accepted 2026-11-18.*

## Context
"Graphed perfectly" has to be a number, or Phase 1 never ends. There are two separate questions:
1. **Is the code graph correct?** Did we find every function and call, and nothing false?
2. **Is it the right thing to graph?** Does the POC mirror what real businesses actually run?

## Decision
- **Question 1 is measured on fixtures,** with no interviews needed: small programs in
  `tests/fixtures/` with hand-checked expected graphs, plus runtime traces of which calls actually
  happen. Gate: call-edge precision ≥ 95% and recall ≥ 90%.
- **Question 2 comes from the field:** real-world examples and at least one interview (target:
  within a month, by mid-December 2026) that records a real company's SaaS list and glue. Phase 3's
  test company is a smaller mimic of that company, not a generic stack.

## Interview checklist
- Every SaaS tool: what it's used for, seats, cost and renewal date
- Every integration: what tool A sends to tool B, how (native, Zapier/n8n/Make, script, CSV), how often
- Who maintains each integration, and what broke in the last year
- The one flow that hurts most (e.g. order-to-cash), walked step by step
- Where the same data (customers, orders, products) is kept more than once
