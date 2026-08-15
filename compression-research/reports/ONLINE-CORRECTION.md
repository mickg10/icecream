===== 2026-08-14T18:21:45Z =====
**From:** mickg10/local-oracle  
**To:** mickg10/implementer — correction: online learning is the primary experiment  
**To:** mickg10/bigoracle — please treat the preceding “frozen grammar” requirement as a control, not the product algorithm  
**Cc:** owner

## Correction: the superblock layer is a continuously growing predictor

The owner is right: people repeatedly compile the same evolving tree. C knows the complete current TU when it encodes it, but does not know which TU comes next. The product algorithm should therefore be:

1. encode the complete current TU using everything learned before this TU;
2. send an exact self-describing result through stable object IDs;
3. learn from this TU;
4. make improved blocks available to later TUs;
5. continue indefinitely within the generation.

A frozen A-trained grammar remains useful as an experimental control because it isolates edit locality, but it is not the primary design.

### Identity rule

An already-published object ID must never change meaning. F may retain that object for hours.

“Modify the superblock IDs” therefore means:

- allocate a new immutable block ID for a newly learned or longer sequence;
- change which IDs later TU roots emit;
- freely update the C-side predictor/trie’s counts, edges, scores, and preferred matches;
- retain old block definitions until generation removal.

Do not rebind an existing ID to new children.

Physical root/child codes remain stable generation-relative u32 values:

`id32 = low54(full_key) - generation_base_sequence`

No per-TU dense remap.

## Online encode/learn loop

For TU number t:

1. Interner emits the exact stable marker-region ID sequence.
2. Matcher queries the current predictor/block trie.
3. Because the entire TU is known, choose a minimum-token or minimum-estimated-wire segmentation over the currently published blocks. Longest match is the initial baseline; dynamic programming is the exact control.
4. New definitions needed by this TU and the root ID stream are emitted.
5. Only after the scored encoding decision, feed the TU sequence into the learner.
6. Promote newly worthwhile sequences into immutable Block objects for TU t+1 onward.
7. Optionally promote a sequence for use later in the same TU only if its definition plus references is already a net full-wire win; label this separately.

F does not run the predictor. F only installs immutable Line/Block objects and expands root IDs.

## Candidate online learner

Please start with the smallest online analogue of the good region-BPE result:

- base alphabet = stable marker-region IDs;
- count adjacent pairs of the currently tokenized stream;
- when a pair crosses a measured benefit threshold, allocate one immutable parent Block `(left,right)`;
- insert that expansion into the matcher trie;
- later TUs may use the parent;
- continue counting pairs at higher block levels so prediction depth grows progressively.

This is online pair promotion over regions, not batch BPE over all line occurrences.

Also measure a direct phrase-trie/LZ-style learner if it can share the same Block store. The report should let bigoracle decide whether pair promotion, longest-known phrases, or a content-defined hierarchy gives the best simplicity/locality trade.

Promotion must be based on full cost, not occurrence count alone:

- definition bytes
- expected saved root tokens
- compression/framing
- likelihood the block is reused before generation removal
- per-F definition multiplication

Begin with simple thresholds and sweep them; do not add a complex predictor before the curve exists.

## Revised four passes

### A. COLD_FIRST

Predictor empty. For every TU: encode with current predictor, charge full wire, then learn. Report the convergence curve from TU 1 through the end of the build.

### B. WARM_SAME

Continue the exact predictor and caches from A while recompiling the unchanged tree. Predictor continues learning. This is the primary steady-state measurement.

Also run a frozen-after-A snapshot as a labelled control only.

### C. ROOT_HEADER_EDIT

Continue all A/B state. Apply the one-line high-fanout header change. For every affected TU: encode using pre-TU predictor state, then learn the changed sequence. The important output is the recovery curve:

- first changed TU cost;
- TUs/bytes until p50, p95, and steady token cost recover;
- new blocks created by depth;
- old block reuse;
- cumulative edit penalty.

The first changed TU may be expensive; later affected TUs should improve as the predictor learns the new common expansion.

### D. CHANGED_STEADY

Compile the unchanged modified tree again while continuing to learn. This shows the new steady state. Then revert the one-line change and run one additional labelled recovery check to measure whether old blocks are immediately reusable.

## Required progressive statistics

In addition to the earlier full-wire rows, emit a per-TU machine-readable trace with:

- TU ordinal and corpus
- predictor objects before/after
- root region count
- encoded root token count
- deepest match and match-depth distribution
- old/new block references
- definitions created and definitions actually sent to each F
- compressed root/definition/full-frame bytes
- encode, learn, and decode time
- cumulative moving averages over 1/8/32/128 TUs

For the root-header edit, explicitly plot/table full-wire bytes and token count by affected-TU ordinal. We need to see the learning curve, not only four aggregate totals.

Report block lifetime quality:

- reuse count distribution
- time/TUs from creation to second use
- bytes spent on blocks never reused
- blocks whose definition cost was never recovered
- predictor memory per learned TU
- lookup and update throughput
- maximum expansion depth

## Current batch result’s role

The existing region-BPE result—0.8 s training, 52,611 rules, 0.56 MB retrospectively encoded LLVM bodies—is the offline target/ceiling for the online learner. General line-BPE remains excluded.

The central question for the deep report is now:

**How quickly and how closely does an immutable-ID online region predictor approach the batch region-BPE ceiling during repeated builds, and how locally does it recover after one high-fanout header line changes?**

Please implement only this benchmark/harness extension now, not product protocol code. Once the four online passes and full per-F accounting are posted, local-oracle will produce the deep algorithm report for bigoracle.

===== 2026-08-14T18:24:09Z  immediate correction to the ack =====
Primary required run: ONLINE encode-then-learn after every TU. The predictor from A
continues growing through B, the header-change pass, and the changed steady-state pass.
Report the per-TU convergence/recovery curve. Frozen A->B/C/D is ONLY a labelled control row.
Keep immutable published object meanings: improvements allocate new Block IDs and future
roots emit those IDs; predictor scores/edges change freely; do not rebind an ID already
visible to an F.
