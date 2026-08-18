# Policy-B corrected 44-cell size census

This report joins the corrected stable-Root P29 census to the complete current-decoder
GRZ2/zstd replay. It excludes the source sweep's timing fields. All 44 P29 probe wires
match their independent prefix gate, and all 44 complete GRZ2 rows are exact.

## Aggregate cold size

| policy | bytes | versus zstd-19-long | raw/wire | margin to 1.10× z19 | passes |
|---|---:|---:|---:|---:|:---:|
| always P29 | 71,169,904 | 1.103581× | 1047.34× | -230,936 B | NO |
| always GRZ2 | 71,395,383 | 1.107077× | 1044.03× | -456,415 B | NO |
| per-cell hindsight | 62,587,653 | 0.970502× | 1190.96× | 8,351,315 B | YES |
| fixed 500 MB raw threshold | 63,153,492 | 0.979276× | 1180.29× | 7,785,476 B | YES |
| leave-one-project-out raw threshold | 63,153,492 | 0.979276× | 1180.29× | 7,785,476 B | YES |

The 1.10× allowance is **70,938,968 B**. Always-P29 misses it by **230,936 B**.

A single causal feature—raw bytes observed through `min(112, total_TUs)`—is enough
on this matrix. Choosing P29 at 500 MB selects the eight Eigen/RocksDB cells and
leaves the two smaller-extent P29 winners on GRZ2. The leave-one-project-out fit
selects the same rows: **42/44** labels, thresholds
from **479.183 MB** to
**510.881 MB**, and only
**565,839 B** above hindsight.

## Interpretation boundary

This is a small, zero-model development policy, not the final generalization claim.
It must be replayed on fixed-16, native-25, and additional verified lineages. Its
feature is essentially free to maintain, but a live adapter must still account for
buffering and the TU112 decision. TU100/TU200 chronological transfer gates remain
separate and are not established by complete-program totals.

## Cell mistakes under project-held-out evaluation

| project | profile | winner | held-out choice | regret bytes |
|---|---|:---:|:---:|---:|
| opencv | fedora-clang-libcxx | p29 | grz | 465,216 |
| range-v3 | fedora-clang-libcxx | p29 | grz | 100,623 |

Machine-readable evidence: `policy-b-size-census.tsv`,
`policy-b-heldout-folds.tsv`, and `policy-b-size-summary.json`.
