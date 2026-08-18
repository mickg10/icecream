# Protocol-50 M5 capability acceptance

## 2026-08-18 final one-pass closure (authoritative)

M5 capability acceptance is **PASS** at commit `dc5e0ab5` on branch
`local-oracle/issue16-m4-m5`.  This verdict binds the complete one-pass path
from producer pipe through C interning/factorization, protocol-50 socket
dialogue, F reconstruction, and the compiler-verifier pipe.  It supersedes
every intermediate `M5 is OPEN` statement retained later in this report as
audit history.

The final performance correction overlaps two independent pieces of accepted
F work: writing the reconstructed TU into the compiler pipe and committing the
same TU's F cache transaction.  The writer is joined before the final Ack, so
the next TU cannot reuse or overwrite its reconstruction buffer.  The exact
verifier consumes and compares every TU in order; its replies are drained at
session close, and the run fails unless the reply count, byte count, TU count,
worker exits, and all physical ledgers close.

This small final change sits on the earlier one-pass work that:

- pipelines source reads, interning, and causal factorization through bounded
  queues;
- publishes immutable dictionary snapshots instead of serializing the
  producer and materializer behind one lock;
- compresses each Fill wave concurrently and sends each ordered Fill as soon
  as its own materialization completes;
- avoids redundant verifier/fill copies while preserving byte-exact replay;
- uses a bounded default prepared queue of 128 TUs.

### Final executed gates

| Gate | Result | Retained evidence |
|---|---:|---|
| optimized focused binaries | 5/5 PASS in each launcher build | each rate/full root below |
| exact local z3 one-pass | 689/689 TUs; 1,985,715,205 B; 0 failures | `/tanksmall/scratch/ictmp/issue16-m5-f-overlap-final-build-BZwgzw` |
| ASan+UBSan exact one-pass | 689/689 TUs; exit 0; no finding | `/tanksmall/scratch/ictmp/issue16-m5-dc5e0ab5-sanitize-ZTkLdt/asan-z3-rerun.log` |
| thread checker, 8-F focused | 40/40 TUs; exit 0; no finding | same root, `tsan-z3-40.log` |
| thread checker, complete corpus | 689/689 TUs; 1-F; exit 0; no finding | same root, `tsan-z3-full-1f.log` |
| focused complete-rate suite, run 1 | 6/6 exact; required z1/z3 rows PASS | `.../rate-suite-v1` |
| focused complete-rate suite, run 2 | 6/6 exact; required z1/z3 rows PASS | `.../rate-suite-v2` |
| fixed-16 M1-M5 suite | **211/211 exact; overall PASS** | `.../full-fixed16-v1` |

The omitted prefix on the quietbox paths above is:

```text
/home/ttuser/issue16-m5-f-compiler-overlap-dc5e0ab5
```

The first full local ASan+UBSan wrapper invocation printed a complete exact
summary but returned a nonzero status without an instrumentation diagnostic;
it is retained but is not counted as a pass.  A direct rerun of the same
binary returned zero and is the row cited above.  A quietbox thread-checker
binary stopped at runtime startup with `unexpected memory mapping`; that host
run is likewise not counted.  The locally working runtime then completed all
689 TUs and all 689 compiler/cache overlaps successfully.

### Binding complete rates

All three executions use the identical DuckDB manifest and exact physical
socket bytes: 27,705,253 B for zstd-1 and 26,587,144 B for zstd-3.

| Execution | zstd-1 complete | zstd-3 complete | Required result |
|---|---:|---:|---:|
| focused run 1 | 1.117 GB/s | 1.081 GB/s | PASS / PASS |
| focused run 2 | 1.061 GB/s | 1.041 GB/s | PASS / PASS |
| complete fixed-16 run | 1.072 GB/s | 1.074 GB/s | PASS / PASS |

The complete zstd-3 scaling rows are informative rather than independent
1-GB/s gates:

| F stores | focused run 1 GB/s | focused run 2 GB/s | fixed-16 run GB/s |
|---:|---:|---:|---:|
| 1 | 0.519 | 0.481 | 0.514 |
| 4 | 0.926 | 0.963 | 0.952 |
| 16 | 1.037 | 1.057 | 0.998 |
| 32 | 0.929 | 0.914 | 0.945 |

Eight F stores remain the binding **test** point, not the preferred initial
product configuration.  Subsequent owner direction accepts approximately
0.5 GB/s while cold and prioritizes compressibility: the one-F rows retain
12,613,513 B at 157.428x and complete at 0.481-0.519 GB/s, whereas eight
independent F stores retain 26,587,144 B at 74.687x.  The intended follow-up
is one shared per-C-GUID F cache populated by the compressibility-first lane,
then warm fan-out to multiple compiler consumers without resetting that
state.  That shared-cache transition is not implemented or timed by M5.

The final M5 code nevertheless clears its stricter eight-F capability gate on
three complete runs without changing physical bytes.  The retained phase logs
show why: the new overlap reduces the F-side final-Ack interval, while the
producer, interner, causal factorizer, component choices, and wire
representation remain unchanged.

### Fixed-16 closure and artifact hashes

The 211-row launcher contains the 45 behavior/transition rows, 160 full-corpus
rows (16 corpora times cold z1, cold z3, both CACHE50 complements, snapshot,
and five orders), and six one-pass rate/scaling rows.  Every row reconstructs
exactly, uses real pipes and sockets, and closes frame, transaction, compiler
byte/TU, and per-TU curve ledgers.

```text
rate-suite-v1/m5-acceptance.tsv
  ea4494f0476a7f98cda4ae1fae7dd71fe5d412ecc6d213c0970a130b24a8aa49
rate-suite-v2/m5-acceptance.tsv
  9b3681c1eb27c87b35526f87cb47cc7cf14cdd120516c0b46abc5d23533406ef
full-fixed16-v1/m5-acceptance.tsv
  cd88a1e778e2f271ce93c89673f000f9fe76744b5e12007af026f651ccf8427b
full-fixed16-v1/m5-acceptance.json
  5d86e8fdacc97952887a1e8d120cf26fe68d7fe375a0451d64736f054a0b5540
```

Both focused roots verify 26/26 retained hashes; the full root verifies
550/550.

### Related Native9 selector checkpoint (separate from M5)

The pending frozen-selector checkpoint was executed after the M5 rate work so
the two measurements did not compete for quietbox resources.  All nine native
holdouts produced independently complete GRZ streams at forced TU100 and
TU200 boundaries and decoded byte-for-byte.  GRZ alone passes 9/9 TU100 cells
and 8/9 TU200 cells; Firefox is the explicit GRZ TU200 miss.  Applying the
policy frozen before those labels selects P29 for Firefox and GRZ elsewhere:

| Metric | Selected bytes | Control bytes | Result |
|---|---:|---:|---:|
| complete cold | 211,299,160 | 210,048,331 zstd-19-long | 1.005955x; aggregate 1.10x gate PASS |
| TU100 | 11,460,367 | 15,260,004 zstd-6-long | 0.751007x; 9/9 PASS |
| TU200 | 18,827,954 | 26,504,769 zstd-6-long | 0.710361x; 9/9 PASS |

This does **not** establish a good general codec selector: only 4/9 cold
choices match per-cell hindsight, only 4/9 cells individually meet the cold
1.10x target, and retained regret is 81,333,446 B.  It establishes the stated
aggregate cold and chronological prefix capability points while leaving
selector improvement as separate research.

```text
/home/ttuser/issue16-selector-v1/native9-prefix-checkpoints-v1
/home/ttuser/issue16-selector-v1/native9-selector-holdout-v1
checkpoint table SHA-256
  2b32ab8c5e634a0883e2637f420d9fdf49a7153ab72cff675dff75d49a284406
selector policy SHA-256
  b5c3ec67f6262e06e0dd4e1554d289b77e974c8f01c55c3f446a38f97017455b
```

## 2026-08-18 binding-rate correction (historical; superseded above)

The 209-row batch scenario matrix below remains valid exactness, lifecycle,
cache, and accounting evidence, but its original **M5 passes** conclusion is
withdrawn.  The rate gate checked the already-prepared relationship and
individual stages; it did not require the complete producer-to-compiler rate.

The retained DuckDB rows actually report:

| policy | prepared relationship | producer-to-wire | process complete |
|---|---:|---:|---:|
| zstd-1 | 1.226 GB/s | 0.620 GB/s | 0.542 GB/s |
| zstd-3 | 1.219 GB/s | 0.622 GB/s | 0.549 GB/s |

At that audit point M5 was **OPEN**, pending a one-pass path that cleared
1 GB/s using the complete timer.  Commits `b1ad1e18` through `dc5e0ab5` added
and closed that binding path:

```text
producer process
  -> bounded raw-TU pipe/read queue
  -> C interning + causal factorization
  -> typed grow-only Region/Block stores
  -> Root / F-generated Need / Fill / prepare+commit socket dialogue
  -> F reconstruction
  -> exact compiler-verifier pipe
```

The new launcher has a focused `--suite rate` and requires `complete >= 1.0
GB/s` in addition to the relationship and individual stage floors.  Do not use
the historical PASS line below as the overall M5 verdict; it describes only
the closed 209-row batch scenario set.

## 2026-08-18 typed-path reconciliation (historical; superseded above)

A later completion audit found that the 209 scenario rows and the one-pass
rate path did not use the same Root representation.  The historical batch
path encoded a Block as `final_NREG + block_id`; the one-pass path correctly
used an explicitly typed token and grew Region/Block stores as definitions
arrived.  Thus the old matrix remained useful lifecycle evidence, but it did
not by itself exercise the exact Root form whose rate was being bound.

Commit `58f41bbf` removes that split:

- both scenario and one-pass executables now encode Region as `id << 1` and
  Block as `(id << 1) | 1`;
- typed dependency closure and typed exact expansion live in the shared
  `FStore` implementation used by both executables;
- focused state tests cover mixed Region/Block order and rejection of an
  undefined typed ordinal;
- the smoke suite now includes direct one-pass grow-only and bounded-cache
  rows in addition to the lifecycle rows;
- the launcher independently closes frame categories, prepared/committed/
  aborted transactions, decode rejections, compiler-consumption replies, and
  optional aggregate worker summaries.

The stricter compiler ledger exposed a failover-reporting ambiguity.  A worker
may be deliberately stopped after several TUs have already been consumed, so
its final aggregate summary can be unavailable.  C now records the per-TU
consumption-reply ledger at each final Ack and reports the partial aggregate
summary separately.  In the retained 4-F failover smoke row, the reply ledger
closes **56,079,068 B / 20 TUs**; the intentionally incomplete summaries cover
**50,057,710 B / 18 TUs** and explicitly report one lost summary.

The same audit found two incompatible `H200` calculations.  The batch path
used a 5%-raw window but silently enlarged it to at least 64 TUs; the one-pass
path used a fixed 32-TU window and did not check the required following 10%
persistence interval.  Neither is the ruled metric.  Commit `5f336d13`
replaces both with one shared calculation:

```text
candidate at a complete-TU boundary
  trailing complete-TU window covers at least 5% of total raw bytes
  trailing raw / trailing physical wire >= 200x
  the rolling trailing window remains >= 200x through at least
  the following 10% of total raw bytes
```

A candidate in the final 10% cannot establish persistence.  Focused tests
exercise a transient 200x hit followed by a drop and prove that it is not
reported.  Consequently, every historical `H200` value in the table below is
withdrawn until the complete typed matrix regenerates it with this shared
definition.  Historical cold/CACHE50/socket byte ledgers are unaffected.

Current executed gates on the branch tip:

| Gate | Result |
|---|---:|
| warning-clean optimized builds | PASS |
| focused M1 header/transport, M2 transaction, M4 protocol, and M5 state tests | PASS |
| typed Root closure and raw-weighted H200 focused cases | PASS |
| snapshot count/byte allocation bounds, optimized and ASan+UBSan | PASS |
| expanded typed smoke suite | **45/45 exact** |
| batch rejection/rollback under ASan+UBSan | PASS, 20 commits / 2 prepared aborts |
| bounded one-pass grow/removal under ASan+UBSan | PASS, 79,376 / 127,633 / 856 removals and 20 compactions |

Latest retained smoke root:
`/tanksmall/scratch/ictmp/issue16-snapshot-bounds-P5YEOI/smoke`. It passes all
**45/45** scenario rows, the five focused binaries, complete artifact hashes,
and the enforced batch/one-pass physical-wire equivalence row.

The focused optimized and instrumented snapshot logs are retained at:

```text
/tanksmall/scratch/ictmp/issue16-snapshot-bounds-P5YEOI/optimized/test.log
/tanksmall/scratch/ictmp/issue16-snapshot-bounds-P5YEOI/asan-ubsan/test-rerun.log
```

The acceptance launcher now rebuilds and runs all five focused regression
binaries before any scenario row: `cap_header_test`, `cap_transport_test`,
`cap_m2_test`, `cap_m4_test`, and `cap_m5_state_test`. The first complete
warning-clean rehearsal of that combined gate is retained at
`/tanksmall/scratch/ictmp/issue16-m5-focused-build-NXbUv8`.

The same audit removed an invalid F-snapshot restriction: Block child count
had been bounded by the number of distinct Regions even though a Block is a
sequence and may reference one Region repeatedly. The follow-up allocation
audit closes the remaining count/byte mismatch. Before any snapshot-driven
resize, restore now checks the container's element limit, the explicit
allocation cap in elements, checked element-byte multiplication, and the
minimum encoded bytes remaining in the file. In particular, Block children
are bounded by both `MAX_PAYLOAD / sizeof(uint32_t)` and the available encoded
child words. The reader tracks exact remaining bytes, and cumulative byte
vectors are checked against their aggregate budget before allocation.

Focused fixtures cover oversized and truncated Block-child arrays, oversized
public-Line tables and F dimensions, oversized and truncated C path tables,
and exhausted aggregate byte budgets. A repeated-child Block and a
zero-dimension snapshot still round-trip, preserving the valid grow-on-arrival
state before the first non-empty TU.

A final scenario/rate comparison found four remaining wire differences. The
batch factorizer could index a 3-Region sequence across the next TU boundary,
Block definitions used a different order, an empty material Fill had two
representations, and batch Hello carried final Region/Block counts while the
one-pass path grew dimensions from typed definitions. These are now unified:

- batch factorization is causal at each complete-TU boundary;
- Block definitions use sorted unique ordinals;
- empty material components have one representation;
- both paths latch zero initial dimensions and use the same bounded typed
  dimension scanner to grow Region/Block stores;
- both final worker summaries carry and verify the pre-Ack physical ledger.

The resulting 4-F, 20-TU fmt comparison is byte-identical across batch and
one-pass: all seven frame categories, all eight component ledgers, every
per-TU wire curve row, and the **3,073,809-byte** physical total match. The
retained comparison is
`/tanksmall/scratch/ictmp/issue16-m5-wire-parity-GLn29q`. The smoke launcher
now enforces this equality between `mesh-4f-roundrobin` and
`onepass-typed-grow`.

This was the intermediate **OPEN** status.  The final typed fixed-16 rerun and
three uncontended quietbox complete-rate executions are now recorded in the
authoritative closure section above.

Historical batch date: 2026-08-17

Branch: `local-oracle/issue16-m4-m5`

Historical parent checkpoint: `da1dafc3` (`capability: complete transactional actual-socket M4`)

Target host: `tt-quietbox2`

Historical retained run: `/tmp/issue16-m5-final2-fixed16-20260817`

## Historical batch-scenario result

The final one-command batch run executed 209 rows and every row closed all of
these independently checked ledgers:

| Check | Result |
|---|---:|
| byte-exact reconstructed TUs | 209/209 |
| physical frame ledger equals counted socket bytes | 209/209 |
| prepared/committed/aborted transaction ledger | 209/209 |
| real compiler-pipe mode used | 209/209 |
| compiler-pipe bytes and TU count equal accepted input | 209/209 |
| binding relationship and per-stage rate rows | 2/2 |
| repeated cold wire-size equivalence | 1/1 |
| retained-file hashes verified | 539/539 |

The complete run covers the fixed 16 corpora with cold zstd-1 and zstd-3 component policies, both CACHE50 complements, a 50%-raw snapshot/resume, reverse/three shuffled/novelty-max orders, change/revert sequences, 1/4/8/16/32 F stores, sticky/round-robin/random/failover assignment, restart, late join, bounded eviction of all three stores, 24-worker host concurrency, real input/output pipes, latency tails, and the binding full-DuckDB rate rows.

This is the M1-M5 capability harness described by `CAPABILITY-PLAN.md`. It is not the separate P29+BSC or GROUP-RLZ compression candidate and it is not a daemon landing.

## Executed dataflow

```text
producer process
  reads ordered .ii files
       |
       | real pipe, exact TU lengths from manifest metadata
       v
C coordinator process
  receives bytes -> interns Lines/Regions -> orders TUs -> builds online Blocks
  owns one SourceGeneration and global immutable ordinal authority
       |
       | one AF_UNIX socketpair per F; typed protocol-50 frames
       | Hello -> Root/Block -> F-generated Need -> Fill -> prepared Ack
       v
F worker process (1..32 independent stores)
  owns Region arena + public-Line bytes + Block definitions + bounded LRU state
  stages Block and Fill changes -> reconstructs complete TU
       |
       | prepared Ack: no externally visible commit yet
       v
C ordered decision
  commits accepted prefix; aborts every prepared suffix after first rejection
       |
       +-- abort --> F rolls back Fill; no compiler output/cache commit
       |
       `-- commit --> F starts two independent local operations
                            |
                            +--> exact bytes written through real compiler pipe
                            |         |
                            |         v
                            |    verifier consumes and byte-compares TU,
                            |    then queues one asynchronous reply
                            |
                            `--> commit Fill/Block/cache state and evict
                                      |
                                      v
                            join pipe writer; return final Ack + removals
                                      |
                                      v
                            C commits authority and repairs that F mirror

session Done --> close compiler pipe --> drain exactly one verifier reply per TU
             --> require verifier exit 0 --> return exact worker summary
```

The verifier consumer stands in for the downstream compiler process.  A final
per-TU Ack proves that F completed the pipe write and cache transaction.  The
verifier's byte comparison is asynchronous: session acceptance additionally
requires exactly one successful verifier reply per committed TU, matching
aggregate byte/TU counts, a clean verifier exit, and a closed worker summary.
Thus exact downstream consumption is a complete-session property rather than
an inaccurate claim that each final Ack waits for the comparison reply.

## Transaction boundary

The first M5 draft allowed an F to emit compiler bytes and commit cache state before C knew the ordered accepted prefix. If an earlier TU in the same wave failed, a later accepted TU could then be retried after already producing output. That ordering was incorrect.

The corrected boundary is two-phase:

1. C journals one authority transaction per in-flight TU and sends a complete Fill.
2. F validates components, stages Block/Fill state, reconstructs the TU, and sends a prepared Ack with no removals.
3. C finds the first rejected TU in logical order.
4. C sends commit only to the accepted prefix and abort to every prepared suffix TU.
5. An aborted F rolls back without compiler output or cache mutation.
6. A committed F writes the complete TU to the verifier pipe concurrently with
   committing its local cache transaction, joins the writer, and returns the
   final Ack only after both operations finish.
7. C commits its authority only after the matching final Ack.
8. At session close, F drains exactly one post-comparison verifier reply per
   committed TU and reports the exact compiler byte/TU ledger; any mismatch or
   nonzero verifier exit rejects the complete run.

The retained rejection scenario deliberately damages logical TU 5 in a four-TU wave. TU 4 commits; TUs 6 and 7 have already prepared and must abort; TUs 5-7 are then retried. The observed ledger is:

```text
prepared_accepted = 22
decode_rejected   = 1
committed         = 20
aborted           = 2
compiler_tus      = 20
compiler_bytes    = 56,079,068
```

The p99/max latency for this row is 165.177 ms. Its timestamp starts at the first attempt, so the retained curve includes session closure, suffix abort, restart, and retry rather than restarting the clock at the successful attempt.

## Cache and snapshot state

Each F has independent generation-scoped state:

| Store | Identity | Representation | Removal policy |
|---|---|---|---|
| Region | typed generation-local `u32` ordinal | immutable view into F Region arena | indexed LRU by resident bytes |
| public Line | typed generation-local `u32` ordinal | immutable owned bytes | indexed LRU by resident bytes |
| Block | typed generation-local `u32` ordinal | Region-child vector | indexed LRU by resident bytes |

The indexed heaps hold one entry per resident object; repeated touches update an existing heap entry and do not grow policy memory. Region removal records dead arena bytes, and compaction rewrites every live view when dead space exceeds the configured slack or the remaining live bytes.

The deliberately small integrated row uses 64 KiB Region, 64 KiB public-Line, and 2 KiB Block limits over the first 20 fmt TUs. It remains exact after:

```text
80,068 Region removals
128,616 public-Line removals
1,010 Block removals
20 Region-arena compactions
```

The bounded snapshot/resume row reproduces exactly those same four counts and the same final live-byte totals. Its physical wire differs from the uninterrupted bounded row only by the deterministic snapshot session controls.

C snapshots contain global public-Line authority, paths, counters, and every per-F mirror. F snapshots contain only dynamic state for the latched generation: resident Region bytes, public Lines, Blocks, paths, LRU ticks, limits, and cumulative removal/compaction counters. Focused tests cover round trips, generation mismatch, and partial snapshot rejection.

## Fixed-16 cold results

> **Historical metric warning:** the byte, ratio, latency, and memory columns
> below remain the retained batch observations. The `H200` column used the
> superseded calculation and is not current acceptance evidence; it will be
> replaced by the complete typed rerun.

The table below shows the zstd-3 component policy. Every zstd-1 counterpart also executed and is retained in the TSV/JSON. `C50` is the cumulative raw/wire ratio at the TU crossing half of raw bytes. `H200` is the earliest stable trailing-window 200x crossing, reported as raw fraction and TU. `none` means that the corpus did not establish that crossing under the retained definition.

| Corpus | TUs | Raw bytes | Socket bytes | Ratio | C50 | H200 fraction / TU | Second half | p99 / max ms | F peak MiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| LLVM | 1,238 | 3,620,271,340 | 14,189,957 | 255.129x | 226.140x | 0.204 / 204 | 292.695x | 11.74 / 55.44 | 3,882.1 |
| RocksDB | 622 | 3,114,320,596 | 15,220,938 | 204.608x | 160.604x | 0.365 / 179 | 281.957x | 32.99 / 94.45 | 3,592.4 |
| DuckDB | 689 | 1,985,715,205 | 12,609,970 | 157.472x | 159.545x | 0.156 / 99 | 155.445x | 53.17 / 105.34 | 2,454.2 |
| Abseil | 700 | 2,581,008,467 | 10,471,576 | 246.478x | 168.955x | 0.187 / 172 | 457.511x | 16.17 / 46.63 | 2,926.4 |
| OpenCV | 1,506 | 4,630,994,774 | 12,094,846 | 382.890x | 316.052x | 0.096 / 245 | 485.671x | 10.23 / 65.06 | 4,981.4 |
| Godot | 2,207 | 5,932,762,185 | 61,809,839 | 95.984x | 59.884x | 0.146 / 347 | 242.225x | 16.06 / 1,957.02 | 7,304.3 |
| fmt | 50 | 136,350,082 | 1,443,817 | 94.437x | 57.309x | none | 281.461x | 46.90 / 46.90 | 337.4 |
| spdlog | 34 | 98,384,472 | 768,234 | 128.066x | 69.373x | none | 938.025x | 40.49 / 40.49 | 252.1 |
| Catch2 | 857 | 947,252,235 | 2,347,456 | 403.523x | 269.126x | 0.086 / 84 | 808.750x | 4.11 / 26.41 | 1,104.0 |
| nlohmann-json | 99 | 293,917,707 | 1,721,584 | 170.725x | 107.639x | 0.695 / 69 | 438.110x | 35.35 / 35.35 | 501.8 |
| range-v3 | 259 | 632,049,016 | 1,782,167 | 354.652x | 297.521x | 0.249 / 65 | 440.579x | 12.70 / 40.12 | 811.2 |
| Eigen | 650 | 3,532,268,956 | 3,228,951 | 1,093.937x | 770.680x | 0.098 / 64 | 1,887.648x | 12.57 / 74.77 | 3,671.6 |
| RE2 | 72 | 110,231,455 | 809,948 | 136.097x | 94.313x | none | 256.954x | 27.43 / 27.43 | 256.0 |
| LevelDB | 72 | 143,868,249 | 1,099,849 | 130.807x | 92.396x | 0.887 / 65 | 230.745x | 43.12 / 43.12 | 307.9 |
| simdjson | 153 | 468,377,342 | 2,212,450 | 211.701x | 134.808x | 0.401 / 66 | 501.499x | 37.84 / 47.85 | 694.0 |
| cereal | 84 | 326,899,429 | 1,093,307 | 299.001x | 216.114x | 0.756 / 64 | 485.624x | 52.91 / 52.91 | 472.4 |

Godot's approximately 2-second maximum is a single large-TU tail; its p99 remains 16.06 ms. The reported F peak is the maximum `ru_maxrss` of one F process. It includes pages shared from the forked corpus/interner mapping and must not be multiplied by the worker count as if every page were private.

## CACHE50 and order gates

Both parity complements are real starting F stores, not discounted estimates. The full results are:

| Corpus | bit0 bytes / ratio | bit1 bytes / ratio |
|---|---:|---:|
| LLVM | 10,260,458 / 352.837x | 9,717,325 / 372.558x |
| RocksDB | 11,556,380 / 269.489x | 11,583,535 / 268.858x |
| DuckDB | 8,070,541 / 246.045x | 9,033,865 / 219.808x |
| Abseil | 8,223,391 / 313.862x | 8,162,383 / 316.208x |
| OpenCV | 8,096,939 / 571.944x | 8,404,594 / 551.008x |
| Godot | 19,137,343 / 310.010x | 51,288,577 / 115.674x |
| fmt | 971,122 / 140.405x | 957,442 / 142.411x |
| spdlog | 456,192 / 215.665x | 466,524 / 210.888x |
| Catch2 | 1,872,346 / 505.917x | 1,889,175 / 501.411x |
| nlohmann-json | 1,150,378 / 255.497x | 1,156,212 / 254.207x |
| range-v3 | 1,332,050 / 474.493x | 1,314,816 / 480.713x |
| Eigen | 2,496,942 / 1,414.638x | 2,492,294 / 1,417.276x |
| RE2 | 521,412 / 211.410x | 548,865 / 200.835x |
| LevelDB | 760,290 / 189.228x | 748,168 / 192.294x |
| simdjson | 1,516,147 / 308.926x | 1,555,238 / 301.161x |
| cereal | 794,677 / 411.361x | 770,453 / 424.295x |

All 80 full reverse/shuffle/novelty rows are exact. Across the five orders, the narrowest/widest socket ratio ranges include 150.681-151.435x for DuckDB, 94.298-94.737x for Godot, 198.067-199.179x for RocksDB, and 1,067.089-1,078.130x for Eigen. The complete per-corpus ranges are in `m5-acceptance.tsv`.

Every full-corpus 50%-raw snapshot row is exact. Its deterministic socket overhead relative to the corresponding uninterrupted zstd-1 row is 197-199 bytes. The compiler consumer still receives exactly the complete corpus byte and TU totals after resume.

## Change and revert gates

The launcher constructs retained A/B/A fixtures by changing one shared header line or one generated hexadecimal line in the first 24 Godot TUs. Each phase's physical wire is derived from its contiguous per-TU curve:

| Scenario | Phases, raw/wire ratio | Complete ratio |
|---|---|---:|
| header edit | A 47.281x; header-B 714.526x | 88.694x |
| generated edit | A 47.281x; generated-B 582.783x | 87.467x |
| header revert | A1 47.281x; header-B 716.164x; A2 517.982x | 122.565x |
| branch A-B-A | A1 47.281x; branch-B 723.633x; A2 517.982x | 122.637x |

All four reconstruct every byte exactly. The reverted A2 phase reuses retained definitions while still carrying the complete physical control ledger.

## Historical batch throughput and multi-F scaling

The following table is retained batch relationship evidence.  It is not the
final complete one-pass rate gate; current binding values are in the
authoritative closure section at the top of this report.

The historical batch rate point used eight F stores on the complete 689-TU
DuckDB corpus. Both component policies independently passed the prepared
relationship and selected stage rates at 1 GB/s or more:

| Policy | Socket bytes | Ratio | Relationship | source pipe | interning | factorization | C transform | F decode/cache | compiler pipe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| zstd-1 | 27,720,944 | 71.632x | 1.226 GB/s | 2.595 | 2.898 | 22.808 | 2.509 | 1.464 | 1.318 |
| zstd-3 | 26,608,015 | 74.628x | 1.219 GB/s | 2.641 | 2.912 | 23.073 | 2.432 | 1.436 | 1.352 |

The zstd-3 scaling curve exposes the performance/bytes tradeoff rather than hiding it:

| F stores | Socket bytes | Ratio | Relationship GB/s | F decode/cache GB/s | compiler-pipe GB/s | p99 / max ms |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 12,609,970 | 157.472x | 0.596 | 1.853 | 1.459 | 51.28 / 105.24 |
| 4 | 19,985,430 | 99.358x | 0.992 | 1.741 | 1.405 | 106.09 / 120.61 |
| 8 | 26,608,015 | 74.628x | 1.219 | 1.436 | 1.352 | 144.32 / 144.93 |
| 16 | 37,869,400 | 52.436x | 1.414 | 1.129 | 1.153 | 205.64 / 205.81 |
| 32 | 55,990,009 | 35.466x | 1.455 | 0.728 | 0.730 | 322.78 / 323.02 |

Eight stores were the balanced point in this historical prepared-relationship
measurement. Four stores measured 0.992 GB/s in that run. Sixteen and 32
improved aggregate relationship rate only modestly while duplicating more
cache discovery and reducing summed per-worker service efficiency. The
current complete-timer evidence is reported at the top of this document.

At eight F stores, the live semantic stores total 170,640,068 Region bytes, 18,400,058 public-Line bytes, and 7,318,816 Block bytes across the workers (about 187.3 MiB combined). The much larger per-process RSS number includes the fork-shared corpus and interner pages used by the exact verifier.

The 24-worker full-fmt row is exact. The deliberately 64-KiB bounded-cache row is not a performance operating point: its 0.121 GB/s relationship rate measures extreme refill/removal/compaction churn and exists to prove bounded behavior and C-mirror repair.

## Corrections made during the M5 audit

| Defect found | Correction and retained proof |
|---|---|
| F output/cache commit preceded C's ordered decision | two-phase prepare/commit; middle-wave rejection gives 20 commits, 2 aborts, and exactly 20 compiler deliveries |
| forced recovery discarded worker summaries and could undercount accepted pipe work | normal close after prepared abort; forced-stop recovery derives only already-acknowledged exact deliveries |
| compiler consumer did not acknowledge each TU | second pipe carries one acknowledgement only after full byte comparison; session close drains and checks exactly one reply per committed TU |
| complete producer-to-compiler rate was not measured | bounded one-pass producer/interner/factorizer path and mandatory `complete >= 1.0 GB/s` rate rows |
| producer and materializer serialized on mutable dictionary state | immutable causally published dictionary snapshots, with direct/snapshot byte-identity coverage |
| Fill work serialized inside a wave | concurrent component compression and ordered send as soon as each Fill is materialized |
| F compiler-pipe write serialized cache commit | joined writer overlaps the pipe write with the independent cache transaction; exact 689-TU thread-check run |
| shallow prepared queue starved the eight-F operating point | bounded default queue depth 128, with high-water byte/count reporting |
| launcher silently used the in-memory input/output path | every run appends `--real-pipes`; parser rejects any non-real row |
| rate gate was attached to every one-F behavior row | behavior rows remain fully measured; dedicated full-DuckDB eight-F rows bind aggregate and per-stage rates |
| F decoder timing double-counted compiler-pipe blocking | decoder/cache and compiler-pipe clocks are now disjoint |
| retry curves restarted the latency clock | first logical send timestamp survives every retry |
| cache-removal list parsing performed a quadratic duplicate scan | idempotent removals now parse linearly |
| diagnostic varint sizes made physical wire totals timing-dependent | final worker summary is a fixed 161-byte record; repeated cold ledger/curve equivalence is a launcher gate |
| `--resume` trusted only path and command | every row records and checks executable SHA-256 plus ordered consumed-corpus SHA-256 |
| snapshots and generated fixtures were not all in retained hashes | executables, logs, curves, snapshots, and every fixture are now covered by `SHA256SUMS` |

## Validation outside the 209-row launcher

Warning-clean GCC builds and focused tests were rerun from the current source:

```text
cap_header_test       PASS
cap_transport_test    PASS (forked two-process frame round trip)
cap_m2_test           PASS (rebind, complete rollback, reserved op)
cap_m4_test           PASS (bounded components and staged C/F replay)
cap_m5_state_test     PASS (LRU/compaction/snapshots)
```

The current M5 state test and middle-wave rejection scenario also pass with address and undefined-behavior instrumentation. The instrumented rejection retains the same logical transaction result: 22 prepared, 1 rejected, 20 committed, 2 aborted, and exact 56,079,068 compiler bytes over 20 TUs.

Local retained audit: `/tmp/issue16-m1-m5-final-audit-20260817`.

## Current reproduction and retained evidence

One-command final fixed-16 run on quietbox:

```sh
python3 capability/run_m5_acceptance.py \
  --suite full \
  --corpus-root /home/ttuser/ictmp \
  --output /home/ttuser/issue16-m5-f-compiler-overlap-dc5e0ab5/full-fixed16-v1 \
  --timeout 1800
```

Primary files:

```text
full-fixed16-v1/m5-acceptance.json
  sha256 5d86e8fdacc97952887a1e8d120cf26fe68d7fe375a0451d64736f054a0b5540
full-fixed16-v1/m5-acceptance.tsv
  sha256 cd88a1e778e2f271ce93c89673f000f9fe76744b5e12007af026f651ccf8427b
full-fixed16-v1/SHA256SUMS
  sha256 48593ca04d06f4cc8ef0590d35c0abc109595fa4824afae62cbbf79771d14c40
full-fixed16-v1/logs/
full-fixed16-v1/curves/
full-fixed16-v1/snapshots/
full-fixed16-v1/fixtures/
```

`sha256sum -c SHA256SUMS` verified 550/550 entries. Each row additionally
records its exact command, executable hash, ordered corpus-content hash, log
hash, and curve hash. `--resume` reuses a row only when command, executable,
corpus hash, log, and curve inputs are present and compatible; otherwise it
reruns it.

## Scope boundary and remaining product work

The M5 capability target is a product-shaped prototype with actual local
sockets, independent F stores, bounded state, snapshots, real pipes, exact
downstream consumption, and a complete-rate measurement. It does not by
itself land protocol 50 in the production icecc daemons. In particular:

- the downstream consumer is an exact verifier process, not an invoked compiler;
- the historical batch runner prepares the complete benchmark corpus before
  starting relationships; only the newer one-pass runner binds the producer,
  protocol, reconstruction, and verifier under one complete timer;
- the fixed-16 M5 semantic payload is the ruled reduced grammar, not the newer P29+BSC or GROUP-RLZ research codec;
- eight independent F stores trade approximately 2.1x the one-F wire bytes for the measured aggregate rate; a shared cache module or affinity policy could recover some reuse in a later daemon integration;
- H200 remains `none` where the retained stable-window definition is not established.

Those boundaries are deliberate in `CAPABILITY-PLAN.md`.  The historical
batch-scenario run is retained only as audit history; the current typed
one-pass rate gates and complete fixed-16 rerun have now passed.  M5 capability
acceptance is closed at `dc5e0ab5`; production-daemon integration remains a
separate implementation milestone.
