# Plan: breaking down binaries

*Draft of 2026-11-18. Replaces the single `exp/frontend-binary` row in the roadmap.*

## 1. The goal

A compiled program ends up in the **same graph** as source code and workflows: its functions,
the calls between them, the shared libraries it uses, and the APIs and databases it reaches. Then
a compiled tool, a Python script and an n8n workflow that call the same CRM all meet on one
`http:` node, as the three-tool sample does today.

That breaks into three questions of increasing difficulty:

1. **Structure:** where are the functions, and what calls what? A solved problem when symbols are
   present; mostly solved without them.
2. **Reach:** which libraries, APIs and tables does it touch? Recoverable from imports and the
   constant strings passed to them.
3. **Identity:** in a stripped binary, *what is* `sub_401136`? Is it `strlen`, a SHA-256 round,
   or the same routine as one in another of the company's programs? **This is where CGL comes in.**

## 2. What CGL gives us

From `~/Projects/AI/CGL`: the evolver (`evolver/prog.cpp`), the trickle experiments
(`experiments/2026-10-26_trickle/README.md`) and the rule architecture
(`self_optimization_and_meta_rule_architecture.md`).

| CGL idea | What it is there | What it becomes here |
|---|---|---|
| **Structure memory** | Subtrees of 3–15 nodes from solved programs, with their leaves replaced by **typed holes** (the structure without its contents), counted by how many programs hold them | Skeletons of lifted machine code: dataflow subtrees with holes for registers, immediates, addresses and callees. An index from skeleton to the functions (with known names) that contain it |
| **Transfer** (round 5b) | Structures remembered from *other* tasks raised program search from 79 to 95 solved out of 190 (sign test p = 0.005). A run's own memory alone didn't help (round 5) | Skeletons from a reference corpus of *other* programs identify functions in a new binary. Corpus memory, not just the binary's own |
| **Read it off the data first** (round 3) | Memory plus similarity ("analogy") beat program search by 6–17 points and was 100–500× cheaper | Identification starts as lookup and vote over the skeleton index. No search or synthesis until lookup has plateaued |
| **Information ceiling** (round 3) | What the data can't decide (irregulars, stress), no method recovers; only memory of the specific case | A stripped binary has lost its names. Optum can name only what its memory has seen (library code, the company's other binaries); the company's own unique functions get *structure*, not names |
| **Fixed outside evaluator** | Every candidate is judged by an evaluator it can't change: correctness first, then cost | Differential execution: run the unknown function and the candidate on the same generated inputs. A match is a hypothesis until it passes |
| **Observational equivalence** (hole filling: Udupa et al. 2013, semantic backpropagation: Pawlak et al. 2015) | Expressions that agree on every test case are the same, for search purposes | The same test decides "is `sub_401136` really `strlen`?" |
| **Rule schema R = (P, G, T, I, V, J, F, A)** | Pattern, guard, transformation, invariants, validation, objective, failure policy, amendment, with correctness constraints ahead of the objective | Each identification ("skeleton S under guard G is `strlen`") is a rule: P = skeleton, G = architecture and ABI, T = label the node, I = call edges unchanged, V = differential test, J = fewer unknown functions, F = leave as unknown and keep the counterexample, A = narrow the guard |

The confidence levels already in the IR fit exactly:

| Confidence | For a binary function |
|---|---|
| `exact` | Named by symbols or debug info |
| `observed` | Identified, and the identification passed differential execution |
| `inferred` | Identified by skeleton vote only |

**One structure-memory engine, two uses.** Phase 2's `feat/analysis-dupes` (finding duplicated
logic in source) is the same machinery on source syntax trees. The skeleton miner should be
written once, over any typed tree (`Core/src/structure/`), and fed by both.

## 3. Scope

**In:** x86-64 Linux ELF executables and shared libraries, from gcc (clang later); with symbols,
with debug info, and stripped.

**Later, as separate tracks** (each is its own plan when it comes up):
- **Windows PE (`.exe`, `.dll`):** the format most business software ships in.
- **Managed bytecode: Java (`.jar`) and .NET (`.dll`).** Names survive compilation, so these are
  far easier than native code and probably *more* common in line-of-business software than
  stripped ELF. The interview should tell us which matters.
- Mach-O, ARM64; C++ virtual calls and RTTI; Go and Rust runtimes.

**Out:** packed or obfuscated binaries, and anything needing a licence to reverse.

## 4. Node ids

Binaries should merge with source graphs wherever the facts allow, in the same way `http:` ids let
tools meet. There are three tiers, best first:

| What we know | Id | Example |
|---|---|---|
| Debug info maps the function to its source | the **source id** | `c:src/util.c#checksum` (merges with the C source graph) |
| Only a symbol name | `bin:<binary>!<symbol>` | `bin:sync_helper!checksum` |
| Nothing (stripped) | `bin:<binary>!0x<address>`, plus `attrs.identified_as` once matched | `bin:sync_helper!0x401136` |
| A shared library, and functions imported from it | `lib:<soname>`, `lib:<soname>!<symbol>` | `lib:libcurl.so.4!curl_easy_perform` |

Symbols are preferred over addresses because addresses change with every build.
`attrs.identified_as` stays separate from the id: an identification can be revised, but an id
shouldn't change. No new node or edge kinds are needed. This goes in decision 0005.

## 5. Ground truth

Every stage is measured the way the frontends are: against an answer known in advance.

- **A reference corpus we build ourselves** ([tests/corpus](../../tests/corpus/README.md)): our C
  samples, zlib and cJSON (sqlite optionally, locally), each compiled at `-O0 -O1 -O2 -O3 -Os`
  with `-g`, then kept in four forms: debug info, symbols only, stripped, and **bare** (stripped
  and without `.eh_frame`). Only the recipe lives in git; the binaries are built in seconds.
  Finding from stage 2: gcc's `.eh_frame` gives every function start even when stripped, so the
  `bare` form is the real test of function discovery.
- **The answer for each binary:** debug info and symbols say where every function starts and what
  it's called, and the C source frontend gives the call graph to score against.
- **A large real-world check:** this machine's `libc.so.6` with its debug symbols (`libc6-dbg`),
  scored with and without them.
- **Held out by program, not by function,** like the trickle test's held-out lemmas: identification
  is always tested on programs whose functions weren't in the skeleton memory.

## 6. The stages

Each line is a branch; `exp/` branches have a fixed time budget and end in a written result. Sizes
are for part-time solo work.

| # | Branch | Job | Size | Gate |
|---|---|---|---|---|
| 0 | `docs/binary-plan` | This plan; decision 0005 (ids, scope, licences) | done | — |
| 1 | `feat/frontend-c` | C source frontend on tree-sitter-c. The source side of every comparison, and useful by itself | done | Samples 100% / 100%; against GCC's own call graph: cJSON 100% / 100%, zlib 85% / 99% ([notes](../notes/2026-11-18-c-frontend-eval.md)) |
| 2 | `feat/bin-corpus` | Corpus build script, answer keys (functions with source ids, the compiler's call graph per level, imports), function-start scoring with baselines ([tests/corpus](../../tests/corpus/README.md)) | done | 35 binaries × 4 forms build in CI in seconds; answer keys checked against the symbol tables |
| 3 | `feat/bin-elf` | ELF reader (sections, symbols, versions, PLT/GOT, relocations, `.eh_frame`); capstone (submodule, BSD); function discovery from symbols, unwind entries, exports, pointers in data, call targets and gaps (jump tables, `endbr64`); direct calls and tail calls; imports as `lib:` nodes | done | Symbols / stripped: functions 100% / 100%, calls 100% / 100%. **Bare: functions 96.9% precision / 98.2% recall, calls 98.9% / 94.4%**; sqlite bare 95.3% / 99.8% and 99.5% / 98.8% ([notes](../notes/2026-11-18-binary-elf-eval.md)) |
| 4 | `feat/bin-lift` | Lift x86-64 basic blocks into a small typed expression IR (values, loads, stores, calls with their argument registers, constants). Constant strings reaching `curl_easy_setopt(CURLOPT_URL)`, `sqlite3_exec` or `PQexec` become `http:` and `sql:` nodes through the existing helpers | 2 weeks | A C version of the `http_glue` sample, built against libcurl, gives the same three `http:` nodes as the Python and JS versions |
| 5 | `exp/bin-skeletons` | **CGL structure memory:** mine skeletons from the lifted IR across the corpus; index them; identify stripped functions by vote. Compare against two baselines: exact byte hashes (FLIRT-style) and control-flow shape | 2–3 weeks, time-boxed | A written result: top-1 identification on held-out programs, by optimisation level, against the baselines. Plus the 5b question: does memory from *other* programs beat the binary's own? |
| 6 | `feat/bin-verify` | The fixed evaluator: run a function and its candidate on generated inputs in a sandboxed child (ptrace, SysV calling convention, pure functions only; any system call aborts the test). Passing raises `inferred` to `observed` | 1–2 weeks | Wrong identifications that pass verification: ≈ 0 on the corpus |
| 7 | `feat/bin-rules` | Identifications kept as R = (P, G, T, I, V, J, F, A) rules in a versioned library. A counterexample narrows a rule's guard (amendment as a logged meta-operation) | 1 week, or fold into Phase 2 | Rules survive a corpus rebuild with a different compiler version |

Stages 1–4 are engineering with known answers. Stage 5 is the research: the part only Optum would
have, and the part most likely to need more than one attempt. Stages 6–7 make its output
trustworthy enough for Phase 2 to act on.

**Rough total:** 9–13 weeks part-time. That's more than the 1–2 week spike the roadmap first
allowed, so the order matters. Stages 1–4 on their own deliver a useful result, binaries in the
graph with their imports and APIs, and Phase 2 can start after them. Stages 5–7 can then run
alongside.

## 7. Risks

| Risk | What to do |
|---|---|
| Identification research swallows the schedule | `exp/bin-skeletons` is time-boxed; stages 1–4 already deliver value without it |
| Optimised code (`-O2`, `-O3`) inlines and reorders until skeletons don't match | Report every result per optimisation level; mine the corpus at all levels; accept lower recall at `-O3` |
| Overclaiming: a confident wrong name is worse than no name | Nothing unverified is `exact`; verification (stage 6) before any rule is trusted; the information ceiling stated in reports |
| Licences: Unicorn (an emulator) and Ghidra-derived pieces are GPL or carry terms we don't want in the core | Capstone (BSD) for disassembly; our own ptrace harness, not an emulator; write down every dependency's licence in `External_Libraries/README.md` |
| The binaries businesses actually have are PE or .NET, not ELF | Ask in the interview; the IR, ids, skeleton engine and evaluator all carry over, and only the readers change |
| Running unknown code during verification | Pure functions only, in a child with no system calls allowed, resource limits, and no network |

## 8. Decisions needed

1. **Corpus projects:** zlib, sqlite and cJSON are the proposal. Anything closer to the
   interviewed company's stack is better.
2. **ELF first, or bytecode first?** ELF has the best ground truth on this machine; Java/.NET
   bytecode may matter more to businesses. The recommendation is ELF first, and revisiting after
   the interview.
3. **Debug-info reader:** our own minimal reader for `.debug_info` and `.debug_line`, or elfutils'
   `libdw` (LGPL, a system library). Decide inside `feat/bin-elf`. The ground truth for tests can come
   from `readelf` / `addr2line` either way.
