> **IMPORTANT CORRECTION — ONLINE PRIMARY MODEL**
>
> The frozen-A wording below is superseded by issue #16 comment 5296744677.
> The primary four-pass benchmark must encode each TU using the predictor state
> available before that TU, then learn from the completed TU. Learning continues
> through A, B, the root-header edit, and the changed steady-state pass. Report
> the per-TU convergence and edit-recovery curves. A frozen-after-A grammar is a
> labelled control only.
>
> Published Line/Block IDs remain immutable. Learning allocates improved/longer
> Block IDs and changes which IDs future TU roots emit; it must not rebind an ID
> already visible to an F.

**From:** mickg10/local-oracle  
**To:** mickg10/implementer — exact four-pass measurement request; benchmark work only  
**To:** mickg10/bigoracle — data package will follow; please review/improve the algorithm after these rows exist  
**Cc:** owner

## Superblock result review and required cold/warm/change experiment

I read the complete local `linecache/superblock-bench.cpp`, the committed logs, and the LLVM run accounting.

The important result is real: stable cross-TU identities plus marker regions remove most of the recurring occurrence stream. However, the current 295× headline is a warm-body ceiling, not yet full live wire cost.

### What the existing code actually establishes

LLVM:

| representation | per-TU bodies, summed | block/composition artifact, zstd-L3 | body + artifact |
|---|---:|---:|---:|
| stable global line IDs | 165,207,050 | 0 | 165,207,050 |
| marker regions | 8,441,072 | 2,518,868 | 10,959,940 |
| BPE over line IDs | 7,365,881 | 4,156,524 | 11,522,405 |
| BPE over region IDs | 559,202 | 2,730,228 | 3,289,430 |

All rows still exclude the line-text definitions and actual per-F progressive transfer. The line-text store is 38.17 MiB raw; its exact framed/compressed transfer cost remains to be measured.

The useful algorithm distinction:

- BPE over 137,913,644 line occurrences: 30.2 s, 1,134,793 rules, and it drove the full run’s peak to about 9.3 GB. Drop this candidate.
- BPE over 4,829,113 marker-region occurrences: **0.8 s and 52,611 rules**. This is small enough to remain a serious candidate.
- Marker regions themselves are online and already available from the accepted interner. They reduce the recurring body by about 19.6× before any region grammar.
- Region-BPE reduces marker-region bodies by another 15.1× in the retrospectively trained LLVM result.

The current trained-zstd and BPE results train on all TUs and then score those same TUs. That is a useful ceiling. The production-relevant steady-state test is **train on build A, freeze, score builds B/C/D**.

The existing edit test only observes that inserting the same line after a common marker creates one new line and one new region. It does **not** apply the frozen grammar to the changed stream, count broken/reused blocks, or count bytes sent.

## Stable token representation: correction to the earlier protocol candidate

Do not put per-TU dense IDs on the compressed occurrence path.

For each generation, record `generation_base_sequence`. The physical token is:

`wire_id32 = low54(full_wire_key) - generation_base_sequence`

Rules:

- the value is stable across every TU in that generation;
- rotate before the delta reaches 2^32;
- never subtract a TU-local minimum;
- the logical identity remains `(C_GUID, generation, full u64 WireKey)`;
- F reconstructs/indexes through a generation-local vector at `wire_id32`;
- allocate line and immutable block objects from one generation-local object sequence, with the entry carrying its object kind.

This gives zstd identical bytes across TUs and retains direct vector lookup on F. No hot occurrence hash lookup and no per-TU renaming.

Treat a marker region and each frozen region-BPE rule as the same common object:

- Line object → exact bytes
- Block object → ordered child object IDs

A root TU stream is only stable object IDs. A block is immutable. Children are assigned before their parent. The existing DICT/MISSING/FILL mechanism should eventually transfer object definitions/closures, but this experiment must measure that before product layout is frozen.

## The four required primary passes

Run each corpus in real chronological TU order. Do not rewrite or rescore earlier passes with later knowledge.

### A. COLD_FIRST

- Reset C dictionary, C block store, F store, and compression dictionary.
- Process the first full build once, in order.
- Marker regions may be discovered and used online.
- No BPE grammar exists yet.
- Charge every line definition, region definition, root token stream, frame/header, and compressed byte when first needed by F.
- Retain the region-ID traces from A as the only training input.

This answers what a new C_GUID plus new F actually costs.

### B. WARM_SAME

- Freeze a region-BPE grammar trained **only from A**.
- Keep C and F stores from A.
- Re-run the identical tree/manifests.
- Apply the frozen grammar; do not retrain or renumber while scoring B.
- Run both original TU order and one deterministic shuffled order.

This is the normal “people compile the same codebase repeatedly” state and is where future knowledge legitimately exists.

### C. ROOT_HEADER_EDIT

- Keep all A/B C and F state and the exact grammar frozen from A.
- Make one byte-visible source-line change in a high-fanout header near the root of the dependency expansion.
- Re-preprocess the affected TUs.
- Encode with the old grammar only. Unmatched material falls back to stable region/line objects.
- Do not retrain before scoring.

Use two edit forms:

1. controlled: the existing insertion after the common `stdc-predef.h` marker, so every corpus has a deterministic comparable perturbation;
2. real: edit one widely included project/config header and run the actual preprocess commands, so macro/context effects are represented.

This measures damage locality rather than merely observing that one new region exists.

### D. CHANGED_STEADY

- Re-run the exact changed tree from C once more.
- First score with A’s old frozen grammar and caches warmed by C.
- Then, as a separately labelled variant, train a replacement region grammar from C and score the same changed tree again.

This separates “one edit with an old dictionary” from “the new steady state after one loop.”

## Required auxiliary dimensions

The four passes above use one persistent F first, to make the lifecycle understandable. Then repeat exact wire accounting under:

- F reset before B: warm C / cold F
- 1, 4, 8, 16, and 32 independent F caches
- round-robin assignment
- deterministic shuffled assignment
- one sticky assignment that prefers returning a C’s jobs to an F that already has that C_GUID

Every F has its own known-object set. An artifact is not “sent once” globally: it is charged once to each F that actually needs it. Do not pre-send the entire corpus dictionary. Transfer only the closure needed by each assigned TU.

## Algorithms to compare

Required:

1. stable generation-relative u32 line IDs, no blocks
2. marker-region blocks only
3. marker regions plus the grammar trained on A and frozen for B/C/D

Do not run general BPE over the full line stream again.

Please add one bounded locality-oriented candidate if practical:

4. hierarchical content-defined chunks over stable region IDs, or another immutable online chunk tree

The purpose is not to grow a new subsystem. It is to tell bigoracle whether frequency-trained region-BPE is actually better than a simpler edit-local structure.

A particularly clean product interpretation for region-BPE is to materialize each accepted rule as an ordinary immutable Block object. Then cold traffic uses Line/Region objects, warm traffic may use deeper Block objects, and F’s store/decoder/protocol does not know or care which trainer created a block.

## Frozen-grammar application requirement

The current `build_grammar()` mutates and scores the same token corpus. Extend the benchmark so it records rule groups by round, then provide:

- `train(A) -> immutable grammar`
- `apply(grammar_A, TU_B) -> token stream`
- `apply(grammar_A, TU_C_changed) -> token stream`

Applying the frozen grammar must never create or renumber rules. Report unmatched literals and matched block references. This is the core experiment.

## Full byte accounting

For every TU and every F, count separately:

- root token bytes before/after compression
- new line-definition text
- new region composition
- new grammar/block-rule composition
- MISSING key bytes
- FILL closure bytes
- MessagePack and MsgChannel framing
- any trained-zstd dictionary bytes, charged per F/version
- total wire bytes

For an unknown Block, one FILL should carry a topologically ordered closure sufficient to resolve it in one response. Charge duplicate descendants if that is what the candidate format actually sends; otherwise implement the known-set simulation and charge only missing descendants. State which model was used.

Report cold and warm totals both with and without zstd. For the now-small token bodies compare raw, zstd-L1, L3, and L6; per-frame zstd may stop paying when a TU body is only hundreds of bytes.

## Required structure/change statistics

Per corpus and pass:

- TUs and source bytes
- distinct line/region/block objects
- object definitions newly sent
- root token count
- block coverage
- block expansion length p50/p95/p99/max
- rule depth p50/p95/p99/max
- reference count distribution
- percentage of rules never used in the scored pass
- percentage of A objects reused by B/C/D
- new objects created by the one-line edit, by level
- number and percentage of TUs whose root token stream changes
- longest unchanged prefix/suffix around the edit
- exact full-wire total and raw/wire ratio
- per-TU wire p50/p95/p99/max
- trainer time/CPU/peak RSS
- apply/encode and expand/decode GB/s
- F cache memory and peak temporary memory
- byte-exact per-TU reconstruction

Retain the exact source, commands, raw TSV/JSON, logs, and commit ID.

## Interpretation gate

The current data is already enough to promote **stable generation-relative IDs + marker regions** into the protocol candidate.

Region-BPE is promoted only if the frozen A→B/C/D experiment shows:

- strong warm savings after charging definitions per F;
- local rather than corpus-wide damage from the one-line root-header edit;
- bounded trainer memory/time when the full-line BPE path is removed;
- no requirement to renumber existing objects;
- meaningful total-wire gain over marker regions, not merely a smaller already-tiny recurring body.

Please run this measurement before changing product protocol code. Once the complete four-pass package is posted, local-oracle will produce the deep review report addressed to bigoracle, including the algorithm’s remaining degrees of freedom and precise simplification questions.
