# P29 chronological-prefix correction

## Result

P29's Root token namespace is now independent of Regions introduced by later translation
units. Complete runs and suffix-blind runs produce identical curves, component ledgers,
raw-plane prefixes, and literal-group frames through two complete 112-TU groups on Catch2,
Eigen, and Godot. All runs reconstruct every `.ii` byte exactly.

The correction is deliberately narrow:

```text
legacy Region token r = r
legacy Block token  k = final_region_count + k

stable Region token r = 2*r
stable Block token  k = 2*k + 1
```

Block and Region ordinals are still allocated chronologically. The low bit merely gives the
two namespaces a stable wire distinction. Later Regions can no longer renumber an earlier
Block reference.

The ordinary structure streams are **not** restarted every 112 TUs. They already use
`ZSTD_e_flush` after each TU, so all emitted bytes are immediately decodable and are functions
only of the prefix already received. A complete run emits `ZSTD_e_end` once at the true end.
The new `--open-final-entropy` mode exists only for the suffix-blind gate: it omits that final
END tail so the gate compares the same physical cut as the continuing complete stream.

## Why the first diagnosis was too broad

The first selector harness compared a complete run's prefix with a separately terminated
short run. It found differences of roughly 1--2 KiB and called all of that "prefix drift."
Two different effects were mixed together:

1. **Real dependency:** Block tokens used `final_region_count + block_id`; the final Region
   count included later TUs.
2. **Artificial short-run tail:** the standalone run emitted entropy END bytes, while the
   same point inside the complete run was an ordinary flushed, still-open stream.

I first closed all structure streams every 112 TUs. That made the comparison equal, but it
paid for fourteen independent control frames on Eigen. The rejected result was:

| corpus | old complete P29 | restart-every-112 | delta |
|---|---:|---:|---:|
| Catch2/debian | 980,948 B | 981,526 B | +578 B |
| Eigen/debian | 2,375,000 B | 2,473,999 B | +98,999 B |

Those artifacts are retained under:

```text
/home/ttuser/issue16-p29-prefix-state/real-catch2-debian-v1
/home/ttuser/issue16-p29-prefix-state/real-eigen-debian-v1
```

The restart approach is rejected. It solves a comparison artifact by weakening compression.

## Final-source real-corpus evidence

Build:

```text
source SHA-256  89ed3b299332f90898b0bd86338052d67d686e93797d2e332080f7ed33c7c753
binary SHA-256  6cb0d9dbbf04f2e90459a49a715e55e06308cc05b382343a460c6dd62e5b526a
compiler flags   -O3 -march=znver3 -std=c++17
                 -Wall -Wextra -Wpedantic -Werror
                 -DICE_LINE_CAP_LOG2=23 -DWITH_BSC_GROUPS
outer workers    16
OMP threads      1 per outer worker
```

| corpus | TUs | checked prefix | complete old | complete stable | delta | prefix wire | C/F GB/s | identity |
|---|---:|---:|---:|---:|---:|---:|---:|:---:|
| Catch2/debian | 861 | 224 TU / 2 frames | 980,948 | 980,640 | -308 | 809,031 | 4.783 / 5.233 | PASS |
| Eigen/debian | 1,516 | 224 TU / 2 frames | 2,375,000 | 2,376,519 | +1,519 | 892,936 | 16.749 / 10.273 | PASS |
| Godot/native | 2,207 | 224 TU / 2 frames | 35,094,169 | 35,085,071 | -9,098 | 2,157,879 | 0.994 / 1.110 | PASS |

Godot exercises the compressed-blob and multi-object factor lanes:

```text
112 compressed blobs
124,407,478 inflated bytes
34,542 retained MO definitions
20 literal groups, all selected BSC
```

For every row the independent verifier checks:

- complete and suffix-blind per-TU curves are identical through TU224;
- every component row is identical and sums to complete physical wire;
- the first 224 rows of all four raw planes are identical;
- the first two packed literal frames are byte-identical;
- both executions report exact reconstruction for every TU.

Retained runs:

```text
/home/ttuser/issue16-p29-prefix-state/real-catch2-debian-v3
/home/ttuser/issue16-p29-prefix-state/real-eigen-debian-v3
/home/ttuser/issue16-p29-prefix-state/real-godot-native-v3
```

## Self-contained gate

`test_p29_literal_groups.py` now verifies:

1. flag-off output remains byte-identical to the frozen legacy binary;
2. stable-tag complete output is exact;
3. a second normally terminated complete-program run is byte-identical (the mode used when the
   program ends at or before the decision boundary);
4. a suffix-blind one-group run equals the first complete-run group;
5. a suffix-blind two-group run equals the first two complete-run groups;
6. a seven-TU input with three-TU groups exercises a partial final group;
7. the fixture actually defines an S1 Block, so the namespace correction is exercised;
8. inconsistent diagnostic mode is rejected;
9. existing malformed literal-plan rejection remains active.

Exact output on the final binary:

```text
P29 bounded literal groups exactness/accounting/rejection and prefix identity PASS
```

## Rate status

The Root representation is not the rate bottleneck, but the Godot C endpoint has little
margin. Three additional repetitions on a busy machine produced:

```text
C: 1.009, 0.999, 0.994 GB/s
F: 1.118, 1.124, 1.135 GB/s
```

The frozen legacy run produced C minima above 1 GB/s with the same 16-worker policy, but the
final stable-tag binary has not yet passed the required **slowest-of-three** rule in an isolated
repeat. Therefore prefix identity and size are closed; the Godot C-rate row remains open by
0.6%. The retained repetition root is:

```text
/home/ttuser/issue16-p29-prefix-state/godot-rate-reps-v1
```

## Selector consequence

The decision boundary is **TU112**, not necessarily the end of GRZ2's first group.

GRZ2 closes a group at the first of these bounds:

```text
112 TUs
512 MiB raw
128 MiB ADD
```

Eigen reaches the raw bound at TU105. P29's first literal frame still covers TUs 1--112.
Truncating the P29 probe to TU105 changes the frame and compares different source extents.
The corrected selector procedure is:

```text
TUs 1..112:
    advance both candidate encoders
    GRZ2 may close its first group before TU112 and begin the next group
    P29 closes its first literal group at TU112

at TU112:
    observe the complete GRZ2 first-group census
    observe the complete P29 first-group census
    choose one codec
    retain the selected encoder's state and discard the other
```

If the complete program has at most 112 TUs, the decision is at program end. There is no unseen
suffix, so both identity runs terminate normally and include the same entropy END bytes. Diagnostic
open-final mode is used only when `prefix_tus < complete_tus`; applying it to an already complete
program would create an artificial tail difference.

The currently running 44-cell matrix remains useful for whole-program labels and GRZ features,
but its P29 short probe used the GRZ first-group TU count. Cells where that count is below 112
must be regenerated with stable Root tags, a suffix-blind open-stream gate, and the common TU112
decision point before fitting the selector.

## Remaining work

1. repeat or tune the Godot C endpoint until every isolated repetition clears 1 GB/s;
2. run the corrected P29-only pass over all 44 cells and merge it with the retained GRZ/zstd rows;
3. fit and evaluate the selector with whole projects held out;
4. replay native-25, preserving the same TU112 decision contract;
5. put the selected codec behind the live compiler-pipe adapter.

The corrected P29-only matrix pass is driven by `run_p29_prefix_matrix.sh`. It freezes an exact
cell ledger before execution, is foreground and resumable, rejects tooling or execution-config
drift, and runs the identity gate for every cell. `summarize_p29_prefix_matrix.py` independently
rechecks artifact hashes, curves, components, complete totals, provenance, and both prefix modes
before emitting a TSV, JSON summary, and Markdown report. Its process timings are explicitly
diagnostic; selector resource accounting belongs to the common-input runner.

The ledger verifier resolves only `corpus.json.payload.path`; it never selects an archive by a
filename glob. This matters because some matrix directories retain an older project-named build
package alongside the later active `ii.tar.zst` corpus generation. The frozen ledger binds the
declared payload path and byte count, payload digest, corpus digest, manifest digest, TU count, and
raw extent before any codec process runs.
