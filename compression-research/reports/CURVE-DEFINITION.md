**From:** mickg10/local-oracle  
**To:** mickg10/implementer — correction to the learning-curve definition  
**To:** mickg10/bigoracle — please assess this prequential metric in the algorithm review  
**Cc:** owner

Owner clarification: the headline learning curve is approximately the conventional one:

- **x-axis:** number of TUs observed so far;
- **y-axis:** predictor quality / compressibility achieved on the current or immediately following TUs.

Cumulative wire-bytes is useful accounting, but it is monotone and hides the actual learning slope. Keep it as a secondary plot, not the headline learning curve.

Use strict **prequential** measurement. For TU \(t\):

1. begin with predictor/Block state learned from TUs \(1..t-1\);
2. cover and encode TU \(t\) using only already-published Blocks;
3. record all TU-\(t\) curve points;
4. only then call observe(TU_t) and publish new immutable Blocks for TU \(t+1\) onward.

The complete TU may be known to the covering algorithm, but new knowledge extracted from that TU must not improve its own scored point. Thus the first TU is scored at x=0 prior TUs observed, the next at x=1, and so on. Continue x across repeated build loops, marking every loop boundary and the edit/revert boundaries.

Please publish two aligned learning curves:

### 1. Predictor/structure learning

Primary normalized score over a rolling window \(W\):

\[
Q_W(t) =
\frac{\sum_{i \in W} (B_i - O_i)}
     {\sum_{i \in W} (B_i - A_i)}
\]

where:

- \(B_i\) = stable marker-region baseline code length for TU \(i\), with no learned superblocks;
- \(O_i\) = online prequential code length using the Blocks available before TU \(i\);
- \(A_i\) = retrospective batch learner ceiling for the same TU.

Interpretation: 0 means no superblock benefit learned yet; 1 means the online learner has reached the retrospective ceiling. Do **not** clamp the value: negative values and values above 1 are informative. Define the exact included bytes consistently, and show a separate decomposition for root IDs and required Block definitions.

Also show the direct diagnostics:

- fraction of region occurrences covered by pre-existing Blocks;
- emitted root tokens per input region;
- mean/p50/p95 expanded regions per root token;
- reused versus newly published Blocks.

These diagnose *why* \(Q\) changes, but \(Q\) or code length remains the primary predictor score.

### 2. Effective end-to-end compressibility

Plot either of these equivalent forms, preferably both in adjacent panels:

\[
\text{bits/input-byte} = 8 \times
\frac{\sum_{i \in W}\text{full charged wire}_i}
     {\sum_{i \in W}\text{raw .ii bytes}_i}
\]

(lower is better), and

\[
\text{effective compression ratio} =
\frac{\sum_{i \in W}\text{raw .ii bytes}_i}
     {\sum_{i \in W}\text{full charged wire}_i}
\]

(higher is better).

“Full charged wire” includes roots, new Line definitions, Block closure definitions, fills, framing, and compression, charged to the F that receives them. This curve captures the combined learning of the persistent Line/Block stores. Keep the pure predictor curve above so cold Line-cache population is not mistaken for superblock learning.

Presentation:

- primary x-axis is **TUs previously observed**, not elapsed time and not cumulative bytes;
- show raw per-TU points faintly plus rolling weighted windows of 8/32/128 TUs;
- make the 32- or 128-TU curve the readable headline;
- annotate loop 1/2/4/8/plateau and all perturbation boundaries;
- for reordered runs, plot median and range/bands over fixed seeds at each TU-count position;
- retain raw bytes-observed as a secondary x-axis or companion plot because TU sizes vary, but do not replace the requested TU x-axis;
- report loop-level endpoint points and TUs-to-50%/90%/95%/99% of the final plateau.

So the core question visible in one glance is: **after the C-side learner has seen N TUs, how much of the reusable structure can it encode on the next TU, and what effective compression does that produce?**
