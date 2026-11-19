# exp/bin-skeletons, round 1–2: naming functions in stripped binaries (2026-11-18)

Stage 5 of the [binary plan](../plans/binary.md): can a **structure memory** in the CGL sense
(skeletons with typed holes, mined from other binaries and weighted by how many functions hold them)
name the functions of a stripped binary? The gate was a written result. This is it.

**Short answer:**
- **With the same build flags:** yes, but so does a plain masked-byte hash (about 95% of
  functions, no wrong names).
- **Across optimisation levels:** structure memory doubled what the hash finds on zlib (the
  tuning set). It **did not on held-out cJSON**: about 18–19% either way, with lower precision.

Instruction-window skeletons aren't a robust identifier across optimisation levels.

## Setup

- **Data:** the corpus (`tests/corpus`), stripped form. Labels are the answer key's source
  function for each address (split-off pieces map to their parent). C-runtime code is left out:
  it's in every binary.
- **Tooling:** `optum disasm` (new) writes every function Optum finds, with instructions,
  operands, bytes with address fields masked, string literals referenced, and call targets.
  [tests/tools/skeletons/experiment.py](../../tests/tools/skeletons/experiment.py) runs everything
  below in about 8 s.
- **Methods:**

  | Method | What it compares |
  |---|---|
  | `exact` | hash of the function's bytes, address fields masked (FLIRT-style) |
  | `shape` | counts: instructions, calls, conditional jumps, returns |
  | `skeleton` | windows of 3–5 instructions with typed holes (register class, immediate, memory operand; tiny constants and imported functions' names kept), IDF-weighted, cosine vote with abstention |
  | `anchors` | contents rather than structure: string literals referenced, large constants, imported functions called |
  | `combined` | skeletons and anchors in one vector |

- **Held out by program and by level** (the plan's protocol): query `minigzip` at level L,
  with memory from `libz.so` + `example` at the other four levels ("cross both"). There's also a
  same-level setting, and a negative control (memory: cJSON only, so nothing should be named).
- **Pre-registration:**
  - round 1's threshold was fixed before running (0.85, margin 0.05);
  - round 2 tuned only on zlib, by a rule fixed beforehand (highest recall with precision ≥ 99%
    and negative-control false IDs ≤ 1%);
  - the chosen configuration was then run **once** on the held-out cJSON test.

## Results

Round 1, summed over the five levels (precision / recall; false IDs on functions not in memory):

| Setting | exact | shape | skeleton |
|---|---|---|---|
| same level | 100% / 95.5% | 100% / 57.5% | 100% / 95.3% |
| **cross both** (other programs, other levels) | 100% / 19.7% | 67.4% / 20.4% | 100% / 24.2% |
| cJSON cross level | 100% / 18.8% | 51.3% / 9.2% | 98.3% / 13.4% |
| negative control: false IDs | 0% | 17% | 0.2% |

Round 2, threshold sweep on zlib (tuning set):

| Method @ τ | zlib precision / recall | Negative false IDs |
|---|---|---|
| exact (reference) | 100% / 19.7% | 0% |
| skeleton @ 0.6 | 100% / 36.0% | 0.8% |
| **combined @ 0.6, chosen by the rule** | **99.5% / 37.0%** | 0.5% |
| anchors @ 0.6 | 89.8% / 32.3% | 3.5% |

Held-out test, cJSON, run once:

| | Precision | Recall (of 425) | Negative false IDs |
|---|---|---|---|
| combined @ 0.6 (chosen) | 93.9% | 18.1% | 2.9% |
| skeleton @ 0.6 | 95.3% | 19.3% | 0.7% |
| exact | **100%** | **18.8%** | **0%** |

## What it means

1. **Same compiler and flags: identification is solved by hashing.** That's the practical
   case of a statically linked library built with distribution defaults. "Which known library
   code is inside this binary" can be built today, at about 95% recall with no wrong names, from a
   memory of distribution builds.
2. **Across optimisation levels, short instruction windows carry too little.** Instruction
   choice, register allocation and block order all change, and what survives is mostly what the
   exact hash already catches (small functions that compile the same). The zlib gain didn't
   generalise. In CGL's terms: the structures were taken at the wrong level. CGL's skeletons are
   subtrees of *programs*, not windows of their machine code.
3. **The negative control matters most.** Shape-only matching names 17% of functions it has never
   seen; skeletons 0.2–0.8%. Any production identifier needs this control in its tests.
4. **CGL round 5b (memory from other tasks) couldn't be tested here.** zlib's library code is
   byte-identical in all three binaries, so other programs add nothing a binary's own other builds
   don't. Testing transfer needs programs that share structure without sharing code.
5. **CGL round 3's lesson held for cost:** memory plus lookup ran the whole study in 8 seconds.
   No search was needed to learn the result above.

## Next round (if pursued)

- **Change the representation, not the threshold:** skeletons from *lifted dataflow* (expression
  trees per basic block: the CGL-faithful structure, insensitive to register allocation), and
  call-graph propagation (names spreading to callers and callees, as BinDiff does).
- **New held-out data:** cJSON has now been seen. Add one or two more libraries to the corpus
  first (for example a JSON or compression library with a different author), and keep them sealed
  until the configuration is chosen.
- **Ship what works:** library fingerprinting at the same flags (exact hash plus skeletons, with
  the negative control as a gate).
