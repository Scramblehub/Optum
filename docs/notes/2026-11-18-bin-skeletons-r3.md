# exp/bin-skeletons, round 3: call-graph propagation (2026-11-18)

Follows [rounds 1–2](2026-11-18-bin-skeletons.md), where judging each function alone found too
little across optimisation levels. Round 3 uses the relations between functions instead.

## Pre-registered before any run

- **Fresh, sealed test data:** two libraries new to the corpus, lz4 v1.11.1 (`liblz4.so`,
  `lz4_frame`, `lz4_file`) and parson (`libparson.so`, `parson_tests`), marked `sealed` in
  `tests/corpus/projects.json`. Tuning used zlib and cJSON only.
- **Method:**
  1. *Seeds:* a unique exact-hash match, or a combined skeleton+anchor match at round 2's
     no-mistake setting (cosine ≥ 0.85, margin 0.05).
  2. *Propagation:* a named function's unnamed callees and callers are matched only against its
     counterpart's callees and callers in memory, with a weaker similarity threshold τ, a margin δ,
     and optionally a structural rule (one open neighbour and one open candidate: match them).
     Repeat until nothing changes.
- **Selection rule on tuning:** the highest recall with precision ≥ 98% and false names ≤ 1%,
  both on functions not in memory and in a negative control.
- **The bar on the sealed test:** recall ≥ 60% at precision ≥ 98%, false names ≤ 1%. A miss is a
  negative result, and identification across optimisation levels stops there.

Script: [tests/tools/skeletons/round3.py](../../tests/tools/skeletons/round3.py) (`--phase tune`,
then `--phase test --config …` once). Each phase takes about 30 s.

## Results

Tuning (zlib + cJSON, 1,023 functions in memory):

| | Precision | Recall | False names |
|---|---|---|---|
| Seeds only | 100% | 21.5% | 0% |
| **Chosen: τ 0.2, δ 0.1, structural on** | **99.0%** | **46.9%** | 0% |

**Sealed test, run once** (lz4 + parson, 1,473 functions in memory):

| | Precision | Recall | False names (not in memory) | Negative control |
|---|---|---|---|---|
| Seeds only | 99.3% | 18.9% | 0.3% | 0.6% |
| **Seeds + propagation** | **96.2%** | **41.1%** | **3.5%** | 0.8% |
| lz4 alone | 96.8% | 51.0% | 4.8% | |
| parson alone | 94.3% | 25.6% | 2.4% | |
| The bar | ≥ 98% | ≥ 60% | ≤ 1% | ≤ 1% |

**The bar is not met** on any of its three counts.

## What it means

1. **Propagation is the real gain of this study.** It more than doubled recall on unseen code
   (18.9% → 41.1%), unlike round 2's improvement, which didn't generalise. The relations between
   functions carry information that single functions don't.
2. **It isn't enough to trust.** On unseen code it starts naming functions that aren't in memory
   (3.5%). Precision falls below 98%, and parson, with its many small JSON accessors calling each
   other, stays at 26%.
3. **As agreed beforehand, identification across optimisation levels stops here.** What works is
   identification at the same build flags: about 95% named with no wrong names (rounds 1–2), which
   is what "which known library versions are inside this binary" needs. That's the thing to ship.
4. **If it's ever reopened:** propagation over lifted-dataflow skeletons, and a confidence that
   decays with distance from the seeds, so a propagated name never counts as much as a seed. Use
   new sealed data again; lz4 and parson have now been seen.

## Also from this round

The larger corpus found a frontend detail: lz4's `lz4hc.c` `#include`s `lz4.c`, so its statics
are compiled twice. They're one source function, so one node, now with the other copies' addresses
in `attrs.copies`. The corpus grew to 75 binaries. With symbols, every function and every call is
still found; on `bare`, functions are at 98.6% / 99.1% and calls at 99.6% / 97.2%.
