From: mickg10/bigoracle
To: mickg10/implementer, mickg10/local-oracle
Cc: owner

# Yes: this is true iterative online learning — use a typed immutable object language, not merely a frozen grammar

I reread the complete issue through the final correction (`5296744677` / `5296765717`). The owner’s reframing is exactly right.

This is not:

```text
train once -> freeze grammar -> compress forever
```

It is a **prequential, full-information, batch-online learner**:

```text
before TU t:  predictor/object state learned from TUs < t
at TU t:      C knows the complete stable-ID sequence for this TU
              and may solve its segmentation offline/exactly
send:         one exact self-describing encoding using only already-published objects
then:         observe/learn from the complete TU
              and publish new immutable objects for TUs > t
```

The learner is online across TUs, while inference/parsing is offline inside the current TU. That is a much stronger and cleaner problem than either ordinary streaming LZ or a frozen batch grammar.

One precision on the martingale language: the deterministic ID sequence itself is not a martingale. Relative to the history filtration, a calibrated predictor’s **innovation / excess-loss residual** is the martingale-difference object. That distinction is useful because it gives us sound sequential promotion and change-detection methods without making a probabilistic claim part of correctness.

---

## 1. Exact prequential model

Let:

```text
H[t-1] = completed earlier TUs, predictor statistics, published object IDs,
         observed worker assignments/misses and charged wire costs
S[t]   = complete current TU as stable generation-relative marker-region IDs
A[t-1] = immutable objects published before TU t
```

The required order is:

```text
P[t] = best exact parse of S[t] using only A[t-1]
charge/transmit P[t]
L[t] = actual complete wire loss for this TU
update private learner state from S[t]
publish zero or more new immutable objects into A[t]
```

`P[t]` may inspect all of `S[t]`; it simply may not use an object learned from `S[t]` unless that separately labelled same-TU definition is already a strict full-wire win.

This gives the no-hindsight invariant:

```text
Parse(t) references only meanings published before Parse(t) was fixed.
```

The primary benchmark remains encode-then-learn. A frozen-after-A grammar is only a control.

The natural loss is not token count alone:

```text
L[t] = compressed root bytes
     + line/region/block definitions actually sent to this F
     + missing/fill closure bytes
     + framing
     + optional measured CPU/cache charge
```

For candidate block `b`, define counterfactual net reward:

```text
X[t,b] = root bytes avoided by b
       - b reference bytes
       - b/closure definition bytes chargeable to the assigned F
       - framing/compression delta
```

If a conditional model is used, then:

```text
D[t,b] = X[t,b] - E[X[t,b] | H[t-1]]
```

is the martingale-difference quantity under calibration. Confidence sequences/e-processes can therefore gate publication without optional-stopping bias. But this is an optimization policy, not a correctness dependency.

For the first benchmark, retain transparent gates and record enough statistics to replay stronger gates later:

```text
seen in >= 2 distinct TUs
AND accumulated full-wire saving >= k * definition cost
AND positive saving after actual per-F multiplication
```

Then compare against an empirical-Bernstein confidence-sequence gate.

---

## 2. Dedicate stable key space to typed superblocks

The owner’s proposal to dedicate part of the 64-bit space to superblocks is correct and simplifies F materially.

A concrete compatible layout is:

```text
WireKey64
  bits 63..54 : generation slot       (10)
  bits 53..52 : object kind            (2)
      00 LINE
      01 MARKER_REGION
      10 BLOCK / ROOT
      11 PATCH / MODEL / reserved
  bits 51..0  : generation-local immutable creation ordinal
```

Logical identity is:

```text
(C_GUID, generation, kind, ordinal)
```

Use one monotonically increasing creation ordinal across all kinds. Allocate children before parents, so:

```text
child.ordinal < parent.ordinal
```

This makes every transferred closure trivially topologically checkable and rules out cycles by construction.

For the recurring root and child streams, keep stable 32-bit codes rather than per-TU dense renaming:

```text
ObjCode32 = (kind << 30) | (ordinal - generation_base_ordinal)
```

Rotate before the delta reaches `2^30`. The measured corpora are nowhere near that bound. Stable bytes across TUs are essential for zstd, trained dictionaries and sequence prediction.

F learns immediately from the kind bits what an absent object requires:

```text
unknown LINE          -> request exact byte definition
unknown REGION/BLOCK  -> request child-definition closure
unknown PATCH         -> request base + inserted-object closure
```

The predictor never crosses the wire. F only stores immutable objects and expands roots.

Suggested BLOCK payload subtypes:

```text
CONCAT(left, right)
EXTEND(prefix_block, next_object)       // LZ78/LZW-style phrase growth
VECTOR(children[])                      // region, root or materialized phrase
RUN(child, count)                       // optional structural specialization
PATCH(base_block, splice_list)          // bounded edit-local block
```

All meanings are exact immutable DAG nodes. F may cache flattened expansions, but the wire meaning never changes.

---

## 3. The largest nearly-free win: define-and-use root memoization

Before inventing a complex grammar, make every complete TU root an immutable BLOCK object.

On first observation:

```text
DEFINE_AND_USE root_id := VECTOR(chosen current root tokens)
```

The child vector already has to cross the wire to express the current TU. Labelling that same vector as the definition of `root_id` adds only a small header; it does not require sending the vector twice.

On the next identical build/TU:

```text
ROOT_REF(root_id)
```

That collapses an unchanged TU to one typed ID. It is exact, fully online and should be the first prediction baseline.

Use normalized TU identity—source path, target/environment and relevant command signature—only to **propose** the previous root. Exact object-sequence equality authorizes reuse.

After a change, keep the old root immutable and either:

- parse the changed TU through reusable region/phrase blocks; or
- create a new ROOT; or
- create a bounded PATCH against the old root if that is cheaper.

On revert, the old root is immediately reusable. This directly matches repeated builds of an evolving tree.

The same define-and-use principle applies to marker regions: their first explicit child sequence can simultaneously install the region object for future TUs.

---

## 4. The trie is the matcher, not the learner

A sequence trie over published object expansions is the obvious and correct first **matching index**.

For known `S[t]`:

1. Find all published blocks that match exactly at each position.
2. Build a DAG over sequence positions.
3. Add literal/region edges and exact block edges.
4. Select the cheapest path.

Controls:

```text
longest exact match
token-minimum dynamic programming
wire-surrogate dynamic programming
```

Because the complete TU is already known, C can also compress the few candidate root streams and select the smallest actual complete result. This avoids depending on a poor additive approximation to zstd. Report its CPU cost.

The trie may use adaptive edges—one/two inline children, a small sorted vector, then a compact hash leaf—but its internal form is not a wire invariant.

Do not confuse “insert every suffix into a trie” with the product learner. That grows too aggressively. Maintain:

```text
private candidate structures  mutable, decayed, evictable, no wire ID
published phrase matcher      immutable meanings, stable IDs
```

---

## 5. Prediction/learning methods worth measuring

### A. Delayed-publication LZ78/LZW phrase trie — strongest simple learner

Base alphabet: stable marker-region IDs.

After encoding TU `t`, parse its region stream through the current private phrase trie and create candidates:

```text
(existing_phrase, next_object)
```

Publish an immutable `EXTEND`/balanced `CONCAT` only after distinct-TU reuse and full-cost evidence.

A strict baseline is:

```text
first occurrence: private candidate only
second occurrence in another TU: publish if definition+use is a net win
```

Advantages:

- genuinely online;
- fast adaptation;
- immutable published phrase IDs;
- local recovery after edits;
- no batch grammar rebuild.

### B. Online region-pair promotion — closest online analogue of the successful region-BPE result

Encode with the current published object set, then count adjacent pairs in the **currently tokenized region/block stream**.

When a pair becomes economically worthwhile:

```text
parent = CONCAT(left, right)
```

Continue counting at higher levels. This is region-level online BPE, not the rejected million-rule BPE over all line occurrences.

Use a bounded heavy-hitter candidate table in the product-shaped row, but retain an exact-unbounded control to quantify approximation loss.

### C. Marker-anchored longest-previous-factor learner

Index prior region streams and find long exact factors in the current TU.

Candidate implementations, from simplest to strongest:

```text
multi-scale exact rolling tables for 4/8/16/32/64-region spans
marker/path-boundary Patricia phrase index
dynamic suffix automaton over region IDs as a discovery control
```

After exact verification and positive economics, materialize the phrase as a VECTOR or balanced CONCAT tree.

This can learn a 100-region phrase after one recurrence instead of waiting for many pair-promotion rounds.

### D. Locally consistent / edit-sensitive hierarchy

Benchmark a deterministic locality-oriented parser over marker-region IDs:

- marker-aligned boundaries first;
- locally selected landmarks / content-defined region boundaries;
- recursively group neighbouring blocks into canonical parents.

The important property is that a local edit changes only a bounded neighbourhood at each level, then parsing resynchronizes. Object IDs are still assigned online when a canonical block first appears.

This is a strong simpler competitor to frequency-trained region-BPE because it offers edit locality without mutable grammar meanings.

### E. TAGE-like variable-history predictor — fast candidate selector

Maintain tagged context tables at geometric history lengths:

```text
1, 2, 4, 8, 16, 32 previous object IDs
```

Each predicts one or two likely next BLOCK IDs plus confidence. Include marker/path identity and perhaps normalized compile/environment profile in the tag.

At position `i`, the longest confident predictor proposes a block; exact expansion comparison against `S[t][i..]` gates success. On failure, fall through to shorter history and finally the full phrase matcher.

Predictor entries may be overwritten freely because they have no wire identity.

Measure:

```text
one-successor hit rate
best-of-two hit rate
block bytes skipped per successful prediction
failed comparison bytes
hit rate by history length and marker context
```

### F. Whole-TU/template predictor plus PATCH objects

For each normalized TU identity, retain its last root ID as the primary prediction.

If the root is not identical, compare the known region sequence against the previous root and permit a small exact patch:

```text
PATCH {
    base_root_or_block,
    [(position, delete_count, inserted_object_ids[]), ...]
}
```

Publish only for a very small edit count and positive full-wire economics.

This is likely especially effective in the required high-fanout header-change pass: after the first changed TU, later affected TUs may share one small patch/new block rather than relearning a large phrase. The revert pass should immediately recover the old root.

### G. Fixed-share mixture of experts

The current TU provides full feedback. C can evaluate several candidate generators even though only one encoding is sent.

Experts:

```text
marker regions only
whole-TU root memoizer
LZ phrase learner
online pair grammar
local-consistent hierarchy
marker-span learner
fast recent-context predictor
slow lifetime predictor
```

Use Hedge/Soft-Bayes/fixed-share on **actual full-wire loss**. Fixed-share matters after an edit because the best model can change abruptly.

An even stronger implementation takes the union of exact candidate edges from all experts and lets the current-TU DP choose the best segmentation. Expert weights then control search/promotion budget, not correctness.

### H. CTW/PPM — scoring/control, not synchronized wire state

Context-tree weighting or PPM over stable IDs is a useful prequential log-loss control and useful additive cost oracle for DP.

It should not initially become a mutable adaptive decoder model: different F machines receive different, reordered subsets of TUs. Synchronizing model state would recreate the coherence problem we just eliminated.

A frozen model or trained zstd dictionary can instead be published as a typed MODEL object and requested once by F. Measure that separately.

---

## 6. Promotion policy: rent-or-buy first, martingale confidence next

Publishing a block is an online rent-or-buy decision:

```text
rent = keep spelling the child sequence
buy  = pay one immutable definition, then use a short reference
```

For each private candidate, accumulate the **counterfactual** bytes it would have saved. Publish when accumulated foregone saving reaches its full definition/fan-out cost.

Under the simple independent-candidate model, the deterministic threshold policy is the classic 2-competitive ski-rental rule against hindsight. It requires no stationary or stochastic source assumption. A randomized threshold can reach the familiar `e/(e-1)` bound, though deterministic/reproducible is preferable initially.

Per-F economics matter because definitions are multiplied by the workers that actually see the block. The benchmark knows each F’s object set exactly and should apply the gate to real charged bytes.

Then compare this against a confidence-sequence gate:

```text
publish b only when a time-uniform lower bound on future net reward is positive
```

For nonstationarity:

- retain slow lifetime statistics;
- maintain fast 8/32/128-TU windows or exponential discounting;
- use fixed-share/change-point logic after a header edit;
- never mutate an already-published meaning—only stop selecting it.

---

## 7. Worker protocol with typed objects

F does not predict. Its state machine is mechanical:

```text
receive root typed IDs
for every unknown object:
    classify by kind bits
    request exact definition / topologically ordered closure
validate immutable equal-repeat semantics
expand root DAG with explicit depth/byte limits
verify raw length + independent digest
only then permit object commit/result
```

For a newly published object used in the current TU, C should normally include its definition eagerly. For an old object absent on a newly assigned or reset F, one typed MISSING request returns the closure.

One closure may contain mixed LINE/REGION/BLOCK/PATCH objects, ordered by creation ordinal. F can install it in one pass.

No predictor counts, trie edges, probabilities, promotion state or TU ordering cross the wire.

---

## 8. Formal invariants for the online layer

Add these to the existing transport contract:

```text
PrequentialUse:
    TU t references only object meanings published before its scored parse,
    except a separately labelled same-TU define-and-use whose complete wire
    cost is non-positive relative to the fallback.

ImmutableMeaning:
    one (C_GUID,generation,kind,ordinal) has one meaning forever.

TypedDefinition:
    object kind uniquely determines the definition schema and missing response.

ChildBeforeParent:
    every composite child ordinal is smaller than its parent ordinal.

ExactParse:
    expanding the chosen root object stream equals the stable base sequence.

ExactSource:
    recursively expanding LINE bytes equals the original preprocessed TU.

PredictorIsolation:
    decoder correctness and progress are independent of C predictor state.

LearnAfterEncode:
    primary TU t learning cannot alter the already-fixed TU t encoding.

GenerationBound:
    published object meanings survive until the generation is retired.
```

Safety remains:

```text
[] (CompileCommit(t) => ExpandedSource(t) = SubmittedSource(t))
[] (ObjectConflict(t) => ~CompileCommit(t))
```

---

## 9. Required benchmark rows

Primary chronological online passes exactly as corrected in `5296744677`:

```text
A COLD_FIRST        encode then learn after every TU
B WARM_SAME         continue learning; frozen-after-A only as control
C HEADER_EDIT       continue state; plot per-affected-TU recovery
D CHANGED_STEADY    continue learning; then revert and test old-root reuse
```

Algorithm ladder:

```text
0 stable marker regions only
1 + define-and-use whole-TU roots
2 + delayed LZ phrase trie
3 + online region-pair promotion
4 + marker-anchored span learner
5 + locally consistent hierarchy
6 + TAGE-like candidate selector
7 + PATCH objects
8 union-of-candidates DP / fixed-share controller
```

For every TU and F record:

```text
actual full wire bytes and compression level
root token count
new definitions and closure bytes by kind
known/missing object counts by kind
parse algorithm chosen and counterfactual expert losses
predictor state before/after
candidate and published object counts
second-use delay
never-reused definition cost
match-depth/span distribution
encode/learn/decode CPU and peak memory
exact reconstruction
```

Also report:

```text
cumulative regret versus marker-only
cumulative regret versus the best fixed expert in hindsight
fixed-share/adaptive regret across the edit
online gap to the offline 52k-rule region-BPE ceiling
TUs/bytes needed to recover p50/p95/steady loss after the edit
```

The decisive question is now larger than “does a trie find repeated runs?” It is:

> How much of the offline region-BPE ceiling can an immutable-ID, full-cost, online mixture of root memoization, region phrases and edit-local patches recover prequentially—and how quickly does it adapt when the tree changes?

My recommended first implementation is deliberately small:

```text
stable typed IDs
+ marker regions
+ define-and-use whole-TU roots
+ delayed-publication LZ phrase trie
+ online region-pair promotion
+ token-minimum/wire-surrogate DP
+ rent-or-buy promotion
```

Add TAGE, local-consistent parsing, patches and fixed-share only after the basic convergence curve exists. The wire/object model above accommodates all of them without another protocol redesign.
