# Optum: concept and plan

*Working name (alternatives: Optumater; naming is parked). Draft of 2026-11-18.*

## 1. Summary

Optum breaks down a company's internal software, simplifies it, and rebuilds it as **one bespoke system the company privately owns**. The result is cheaper to run, faster to change and easier to understand.

It starts where the waste is most visible: the pile of SaaS products, integrations and sync jobs a typical company has collected. Optum maps that sprawl into a single model of the business. It keeps the commodity tools worth renting and replaces the rest, plus the glue between them, with a unified system generated from the model. Because the system is compiled from that model, every step reports what it does. The company gets more insight into its own operations than it had before.

Optum is a software version of what EY or Deloitte sell as transformation projects. The difference is that the discovery, the rebuild and the upkeep are mostly done by software, and the company ends up owning the result.

**Entry product: SaaS unification with private ownership.** It's the easiest part to sell and to prove, and it pays for everything after it.

## 2. The problem

- SaaS spend averages about **$4,830 per employee per year**, up 22% in a year (Zylo, 2025). Vertice reports about $9,200.
- Companies run somewhere between **106** (BetterCloud survey) and **about 275** (Zylo license data) SaaS apps. The counts differ by whether they include tools employees buy themselves.
- Every tool keeps its own copy of customers, orders and products. Integrations, sync jobs, scripts and CSV exports hold it together. Nobody owns that glue, and it breaks when a vendor changes an API or the person who built it leaves.
- The company's actual business rules are spread across ten vendors' settings pages. No one can answer simple questions like "what does one order cost us end to end?"

## 3. The product

### 3.1 The company model, in five layers

1. **Entities.** Customer, order, product, invoice, supplier: each defined once, not once per tool.
2. **Capabilities.** The business broken into parts (quoting, order promising, fulfilment, billing, procurement, support). Each part is classed:
   - **Core:** the line-of-business functions that earn the money or set the company apart. Optum optimizes these and builds them as the company's own code.
   - **Supporting:** necessary but not differentiating. Optum reshapes these to serve the core.
   - **Commodity:** payroll, email, tax, the accounting ledger. These stay rented from vendors, behind adapters that can be swapped.
3. **Flows.** The value streams that run across capabilities: order-to-cash, procure-to-pay, hire-to-retire. Glue is replaced here: a handoff becomes a typed step in a flow instead of a sync job.
4. **Implementation.** Generated code for the core and supporting parts, adapters for commodity tools, one runtime and as few data stores as possible.
5. **Observation, built in.** Every step emits events because the system is compiled from the model. Cycle times, cost per order, margin per customer, bottlenecks and error rates come for free, not as a later dashboard project.

### 3.2 The exterior serves the core

Each core function states what it needs from the others as a **contract**, for example:
- order promising needs stock counts no older than 15 minutes
- an invoice goes out only after the shipment is confirmed

Supporting functions are optimized against those contracts rather than their own local measures. For example, procurement is tuned to keep the best-selling items in stock rather than to get the lowest unit price. The compiler checks the contracts, so a change that would break the core is caught before it ships.

*Vocabulary:* the core/context split is Geoffrey Moore's idea, and enterprise architects call layer 2 a "capability map". What's new is generating them from how the company actually runs, then rebuilding from them.

### 3.3 Who decides what is core

Optum proposes and leadership decides. Evidence Optum brings:
- which flows end in revenue, and how much
- where the company departs from its industry's usual pattern
- cost and headcount per capability
- where errors and delays reach customers

### 3.4 Simplicity, measured

Every engagement reports these before and after:
- the number of systems and integrations
- copies of the same data
- the size of the codebase
- the time to make a typical change
- license spend

## 4. Entry product: SaaS unification with private ownership

### 4.1 What happens

1. Optum reads the company's SaaS stack: APIs, configurations, the integration platform, data flows, invoices and usage.
2. It decides, tool by tool, whether to **keep** it (commodity, behind an adapter), **replace** it (absorb it into the unified system), or **retire** it (overlapping or unused).
3. It generates one system: one data model, one runtime, one interface for the replaced tools, adapters for the kept ones.
4. Replaced tools switch over one at a time, timed to their contract renewal dates, so no license is paid twice for long.

### 4.2 Private ownership

The company owns:
- **its model:** the description of its business that the system is generated from
- **its code:** generated into a standard, readable stack, not a proprietary format
- **its data:** in its own database, in its own cloud account or data centre
- **the right to leave:** it can keep running and changing the system without Optum

This is the pitch against SaaS: stop renting ten products, own one that fits. It only works if ownership is real. The generated system has to run on standard infrastructure, without needing Optum to stay alive.

Ownership also means responsibility: someone has to operate, secure and update the system. That's what the maintenance tiers are for (section 6).

### 4.3 Example economics (every input is an assumption to test)

A 300-employee company:

| Item | Assumption | Per year |
|---|---|---|
| SaaS spend | 300 × $4,830 | $1.45M |
| Share Optum can replace or retire | 40% | $580k saved |
| Glue upkeep today | 2 people at $150k | $300k |
| Glue upkeep after | half | $150k saved |
| Optum co-managed tier | assumed fee | −$200k |
| **Net saving** | | **about $530k** |
| One-time rebuild fee | assumed $250–400k | payback under 1 year |

The share that can be replaced is the most important unknown. The early diagnostics (section 11) exist to measure it.

## 5. The engagement

1. **Map:** connectors read every tool and flow; the output is the company's system graph.
2. **Diagnose:** overlapping tools, unused licenses, duplicated data, glue upkeep hours, fragile points (what breaks if one person leaves), all priced in dollars. Also the first business insights.
3. **Design:** a keep / replace / retire decision per tool, the company model, the contracts.
4. **Rebuild step by step:** the strangler-fig pattern. The new system takes over one piece at a time while the old one keeps running. No big-bang cutover.
5. **Verify:** old and new run in parallel and their data is reconciled before each piece switches over: the same invoices, the same customer records, the same totals.
6. **Run and maintain:** in the tier the company chooses.

## 6. Maintenance tiers and expansion

| Tier | Who maintains | Optum provides | Fee |
|---|---|---|---|
| Self-managed | The company's team | Tooling: model editor, compiler, monitoring, upkeep agents | Lowest |
| Co-managed | Both | Updates, adapter fixes when vendors change their APIs, a quota of changes | Middle |
| Fully managed | Optum | Operation, service-level guarantees, on-call | Highest |

The company owns its model and code in every tier and can change tiers.

**Expansion.** A new feature is a new capability added to the model. The compiler checks that it fits the entities, flows and contracts, then generates the code. A new venture is a new value stream that reuses existing capabilities: the same customers, billing and fulfilment for a new product line. Changes are priced per capability, or come out of the tier's quota.

## 7. Technology

### 7.1 The company language

The company model is written in a language derived for that company. It's a short, typed description of its entities, capabilities, flows, contracts and adapters. Code, integrations, tests and migrations are generated from it. A flow might read:

```
flow Refund(case: Order) deadline 48h {
    check    = step code   verify_receipt(case)
    approve  = step role   Finance  when case.amount > 500
    pay      = step code   issue_refund(case)   after approve
}
```

The compiler rejects:
- a case that can't reach an end state
- a skipped sign-off
- a deadline that measured step times can't meet
- sensitive data sent to a step that isn't cleared for it

### 7.2 The derivation engine

Getting from sprawl to model is the hard research problem, and where the advantage lies. The engine builds on the CGL work (in `~/Projects/AI`):

- **Structure memory** (skeletons with typed holes) finds the same mapping, transform or rule repeated across the glue and the tool configurations.
- **The rule schema R = (P, G, T, I, V, J, F, A)** expresses each simplification: a pattern, a guard, a transformation, the invariants, how it is validated, its objective, what happens on failure, and how it is amended.
- **A fixed outside evaluator** judges every change: correctness first (the paired gate, data reconciliation), then cost.
- **Transfer:** rules learned at one company make the next engagement faster. CGL's round 5b result (structures learned on other tasks: 95 vs 79 solved on new seeds, p = 0.005) is early evidence that this works.

### 7.3 Verification

Trust is the product. Every switch-over needs:
- a parallel run with data reconciliation
- replay of historical records
- generated tests from the model
- staged rollout with rollback

### 7.4 Security by construction

The selling point is security by construction, not obscurity. The company language marks sensitive data (personal data, payment data) as its own types, and the compiler won't let it be logged or sent off-site. It also enforces capability-based access, so a component only touches what it declares. Optum runs inside the customer's environment; no customer data leaves by default.

## 8. Why this is hard to copy

1. **Deriving the model from real sprawl**, rather than drawing it by hand in workshops.
2. **Verified rebuilds:** the parallel-run and reconciliation machinery.
3. **A pattern library that compounds** with every customer.
4. **Benchmarks across companies (later, opt-in, aggregated):** for example, "your order-to-cash takes longer than 80% of distributors your size."

## 9. Market

### 9.1 First customers

Companies with roughly **200–2,000 employees**:
- They have real sprawl.
- They're too small for a Big Four transformation program and too complex for spreadsheets.
- One person feels the pain and can sign: a COO, with the CTO as ally.

### 9.2 Sectors

| Sector | Phase 1? | Why |
|---|---|---|
| Logistics, distribution, wholesale | Yes | Order, inventory, shipping and invoicing sprawl; little regulated data |
| Manufacturing (mid-sized) | Yes | ERP plus many side tools and lots of glue |
| Professional services | Yes | Many tools per employee; project-to-invoice glue |
| E-commerce and retail | Yes | Many integrations; card data stays with the payment processor |
| Software companies | Maybe | Lots of sprawl, but they often build their own fixes |
| Schools and universities | Later | FERPA for student records |
| Healthcare | No | HIPAA: patient data throughout; revisit later |
| Card-heavy finance and payments | No | PCI DSS; SOX for public companies |
| Government | No | FedRAMP-type authorization, long sales cycles |

Everywhere: GDPR (EU customers) and US state privacy laws such as CCPA apply to personal data from day one.

### 9.3 Against consultancies

| | Big consultancy | Optum |
|---|---|---|
| Discovery | Months of interviews and workshops | Weeks, read from the systems |
| Output | Recommendations, then a long implementation project | A running, verified system |
| Billing | Hours | Fixed diagnostic fee, rebuild fee, maintenance tier |
| Afterwards | Decays back into sprawl | Upkeep agents keep it simple |
| Ownership | Often a new vendor platform | The company owns model, code and data |

## 10. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Scope: "rebuild everything" sinks the project | Unification first, one value stream at a time, strangler-fig cutovers |
| Rebuilding commodity tools (payroll, tax) becomes a maintenance burden | Rule: own the glue and the business logic, rent the commodity |
| Users miss features of a replaced tool | Map actual feature usage before deciding; replace only what is used |
| Data migration errors | Parallel runs and reconciliation before every switch-over |
| Vendor contracts lock in spend | Time cutovers to renewal dates |
| Vendor terms restrict access | Official APIs and exports only |
| Ownership means the company now carries operations and security | Maintenance tiers; managed tier for companies without a team |
| Deciding what is core is political | Optum proposes with evidence, leadership decides |
| Delivery is consulting-heavy early on | Track engineers per customer; the engine and library bring it down |

## 11. Validation before building

1. **Interviews:** 10–20 operations heads and CTOs at mid-sized companies in the phase 1 sectors. Collect SaaS lists, integration inventories, who maintains the glue, and renewal dates.
2. **Diagnostics done mostly by hand:** 2–3 companies. Measure the share of SaaS spend that can really be replaced. The business case rests on it.
3. **A test company for the technology:** 8–10 open-source tools wired with realistic integrations and generated data, for example an ERP (ERPNext or Odoo), a chat server, a help desk, an automation tool and a wiki. The proof: Optum unifies it into one owned system, and the parallel run reconciles exactly.

## 12. Roadmap

| Stage | What | Gate to the next stage |
|---|---|---|
| 0 | Interviews and manual diagnostics | A measured replaceable share, and at least one company willing to pay |
| 1 | Map and Diagnose product | Paid diagnostics; findings acted on |
| 2 | SaaS unification for one sector and one value stream (e.g. order-to-cash at distributors) | A verified cutover; savings on record |
| 3 | The full company model: several value streams, contracts, tiers, expansion | Renewals and expansion requests |
| 4 | Benchmarks across customers; regulated sectors | Enough opted-in customers for honest comparisons |

## 13. Open decisions

1. **First sector and value stream:** distribution with order-to-cash, manufacturing, or professional services with project-to-invoice?
2. **Who signs:** COO (insight, operations) or CTO (simplification, cost)?
3. **Is the diagnostic a paid product on its own,** or only a step towards unification?
4. **Generated stack:** which standard language, database and hosting the owned system is generated into.
5. **Runtime:** fully generated code, or an open-source Optum runtime with generated configuration and code on top? Ownership is only real if the runtime is open or replaceable.
6. **Pricing:** rebuild fee vs share of savings; tier prices.
7. **Name.**

## 14. Parked ideas

- **Language derivation for performance:** deriving a faster language for one hot pipeline (the compute-savings framing). Rough model: about $8B a year of compute savings worldwide in the middle case. It could return as a feature for the core.
- **Recursive improvement:** the engine as its own target, only once the transfer curve shows compounding gains, with the evaluator write-protected.
- **Per-deployment software diversity** for security.

## 15. Sources

- Zylo, 2025 SaaS Management Index: https://zylo.com/news/2025-saas-management-index
- Vertice, SaaS spend per employee: https://www.vertice.one/blog/saas-spend-per-employee
- Breeze, SaaS tool sprawl statistics (BetterCloud figure): https://www.breeze.pm/articles/saas-tool-sprawl-statistics
- Gartner, public cloud spending 2025: https://www.gartner.com/en/newsroom/press-releases/2024-11-19-gartner-forecasts-worldwide-public-cloud-end-user-spending-to-total-723-billion-dollars-in-2025
- Halide, PLDI 2013: https://dspace.mit.edu/handle/1721.1/85943
