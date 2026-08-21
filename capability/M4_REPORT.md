# Protocol-50 M4: authoritative independent-component socket codec

Date: 2026-08-17

Branch: `local-oracle/issue16-m4-m5`

Baseline: `implementer/issue16-capability@e1f91fbb`

## Decision

Expanded M4 is accepted as a codec and state-transition checkpoint:

- all 9,292 translation units in the fixed 16 reconstruct byte-for-byte;
- the only byte total is the physical two-process socket ledger;
- every Root, Block, Need, path, Region-control, RAW_RUN, BYTE_ARRAY-control, and
  BYTE_ARRAY-values component is independently RAW/zstd-1/zstd-3 selectable;
- no entropy state crosses a component or TU boundary;
- Blocks, Regions, public Lines, paths, and C authority discoveries commit only after
  complete TU reconstruction and acknowledgement;
- restart, late join, public-Line eviction, unequal rebind, malformed Fill, and one
  rejected compressed component followed by a valid retry are exact;
- every tested compressed policy is bounded by its per-component RAW fallback.

M5 is **not** accepted. The fixed-16 best-byte row reaches only 0.617 GB/s at its slowest
relationship and seven corpora are below 1 GB/s. The eviction implementation is deliberately
simple but falls to 0.139--0.222 GB/s because it repeatedly scans the public-Line vector. Peak
RSS reaches about 7.1 GiB per process on Godot because the capability executable preloads the
whole corpus and retains append-only research stores. M5 must address overlap, scheduling, and
bounded state rather than relabeling the synchronous M4 loop.

## What changed

### One physical representation

`cap_m4_main` uses an actual `AF_UNIX` socket pair and a forked F process. The payload F decodes
is exactly the payload counted. There is no parallel virtual-byte calculation.

The relationship is:

```text
C                                  F
Hello(protocol=50, generation) --> latch generation and dimensions

Root(TU, root, Blocks) ---------> stage Blocks; derive Region closure
                             <--- Need(TU, missing Regions, public-Line drops)
Fill(TU, paths, 4 material lanes) -> stage Fill; reconstruct complete .ii
                             <--- Ack(TU, accepted)

Done ---------------------------> final accounting check
                             <--- Ack(exact, TUs, raw, failures, pre-Ack wire)
```

Each semantic component carries one explicit selector followed by either raw bytes or one
self-contained zstd frame. The `best` row evaluates RAW, zstd-1, and zstd-3 using the complete
component-envelope cost and sends the smallest. Empty and tiny components normally choose RAW.

### Complete transaction boundary

The deferred M4 corrections are implemented:

1. all socket payload parsing uses bounded canonical varints and requires complete consumption;
2. Block definitions are staged and become visible only after exact TU reconstruction;
3. Fill rollback restores Region views/data, path extent, public-Line bytes/presence/last-use,
   public ordinal position, and vector extents;
4. C journals first-touched Line authority, path definitions, public ordinal allocation,
   counters, and optional census output;
5. a rejected attempt rolls C authority back, resets the C mirror of F, restores F from an
   explicit Rejoin exchange, and retries the same TU;
6. Rejoin and resynchronization payloads are bounded and fully consumed.

The C rollback item was discovered during this expansion: the old F transaction could reject a
Fill while C retained public-Line/path discoveries made while constructing it. The new
`MixedEncoder::AuthorityTransaction` makes those discoveries tentative as well.

## Fixed-16 physical-wire matrix

The matrix covers 28,554,671,510 input bytes and 9,292 TUs. `z1` and `z3` mean
`min(RAW, selected level)` per component; `best` means `min(RAW, z1, z3)`.

| policy | physical socket bytes | weighted ratio | slowest relationship |
|---|---:|---:|---:|
| RAW | 424,295,701 | 67.299x | 0.721 GB/s |
| zstd-1 | 120,058,876 | 237.839x | 0.726 GB/s |
| zstd-3 | 116,672,313 | 244.742x | 0.667 GB/s |
| best actual component | **116,634,584** | **244.822x** | **0.617 GB/s** |

The best selector saves only 37,729 bytes (0.032%) versus zstd-3 while performing both
compressions. That is a useful negative result: M5 should not use dual compression on its timed
path. zstd-1 is only 3.39 MB larger across the complete fixed 16 and is the initial throughput
candidate; zstd-3 remains the byte control.

### Best row by corpus

| corpus | raw bytes | socket bytes | ratio | C transform | F decode/expand | relationship | C/F peak RSS MiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| LLVM | 3,620,271,340 | 10,070,317 | 359.499x | 3.927 | 1.744 | 1.114 | 3,832.0 / 3,914.9 |
| RocksDB | 3,114,320,596 | 10,409,127 | 299.191x | 4.606 | 1.224 | **0.907** | 3,447.7 / 3,585.2 |
| DuckDB | 1,985,715,205 | 10,091,961 | 196.762x | 2.849 | 1.460 | **0.918** | 2,387.0 / 2,456.5 |
| Abseil | 2,581,008,467 | 6,402,976 | 403.095x | 7.761 | 1.609 | 1.253 | 2,833.1 / 2,920.6 |
| OpenCV | 4,630,994,774 | 9,650,052 | 479.893x | 6.318 | 1.403 | 1.084 | 4,826.6 / 4,978.6 |
| Godot | 5,932,762,185 | 58,198,278 | 101.941x | 1.488 | 1.146 | **0.617** | 7,145.8 / 7,139.6 |
| fmt | 136,350,082 | 1,246,633 | 109.375x | 1.635 | 1.279 | **0.671** | 325.3 / 337.9 |
| spdlog | 98,384,472 | 729,097 | 134.940x | 2.111 | 1.613 | **0.868** | 252.5 / 252.8 |
| Catch2 | 947,252,235 | 1,284,015 | 737.727x | 12.789 | 2.277 | 1.688 | 1,101.1 / 1,107.7 |
| nlohmann/json | 293,917,707 | 1,485,521 | 197.855x | 3.035 | 1.676 | 1.013 | 486.1 / 500.3 |
| range-v3 | 632,049,016 | 1,147,527 | 550.792x | 8.698 | 1.703 | 1.324 | 798.0 / 809.0 |
| Eigen | 3,532,268,956 | 1,889,111 | 1,869.805x | 15.192 | 2.008 | 1.652 | 3,630.9 / 3,685.7 |
| RE2 | 110,231,455 | 656,382 | 167.938x | 2.510 | 1.668 | **0.916** | 260.5 / 256.4 |
| LevelDB | 143,868,249 | 876,274 | 164.182x | 2.276 | 1.603 | **0.845** | 308.1 / 310.1 |
| simdjson | 468,377,342 | 1,770,301 | 264.575x | 4.730 | 1.659 | 1.173 | 671.4 / 690.0 |
| cereal | 326,899,429 | 727,012 | 449.648x | 6.821 | 2.084 | 1.490 | 472.1 / 473.1 |

Rates are GB/s of original bytes. Relationship timing begins after Hello and ends after the final
TU acknowledgement; corpus load, interning, and S1 construction are outside it. C and F component
work individually exceed 1 GB/s in every best row. The sub-1 GB/s end-to-end rows therefore expose
the cost of the strictly serial Root/Need/Fill/Ack relationship and motivate bounded inflight work.

## Byte closure

Best-row physical frame totals:

| frame | bytes | frames |
|---|---:|---:|
| Hello | 451 | 16 |
| Root | 4,239,760 | 9,292 |
| Need | 3,307,912 | 9,292 |
| Fill | 109,022,718 | 9,292 |
| Done | 64 | 16 |
| Ack | 63,679 | 9,308 |
| Rejoin | 0 | 0 |
| **total** | **116,634,584** | **27,924** |

Selected component bodies, including their selector bytes:

| component | bytes |
|---|---:|
| Root | 3,647,273 |
| Block | 515,668 |
| Need | 3,242,705 |
| path | 820,805 |
| Region control | 18,167,790 |
| RAW_RUN | 50,897,558 |
| BYTE_ARRAY control | 562,939 |
| BYTE_ARRAY values | 38,411,521 |
| **component total** | **116,266,259** |
| outer lengths, TU/base fields, and packed headers | **368,325** |
| **physical total** | **116,634,584** |

The result remains 24,769,797 bytes above the 91,864,787-byte P29 research control. M4 is the
self-contained M3 grammar with independent frames; it does not contain P29's material operation
and does not reuse P29's cross-TU entropy history.

## Retained-state second pass

All 16 corpora ran twice through one generation. Per-TU wire accounting excludes the one-time
Hello and final Done/Ack so the pass boundary is not confused with a product build identifier.

```text
first chronological pass     116,633,743 B
second retained-state pass     4,085,383 B
two-pass physical total       120,719,975 B
second-pass ratio               6,989.47x
```

The generation is the persistent-state identity. “Pass” is only a replay/report boundary.

## Recovery scenarios

| scenario | socket bytes | ratio | relationship | result |
|---|---:|---:|---:|---|
| DuckDB restart at TU 344 | 12,772,794 | 155.464x | 0.725 GB/s | exact |
| DuckDB late join at TU 344 | 8,965,826 | 126.234x | 0.724 GB/s | exact |
| DuckDB public budget 100 | 10,775,402 | 184.282x | **0.222 GB/s** | exact; 170,124 removals |
| DuckDB rejected component TU 344 | 12,773,999 | 155.450x | 0.792 GB/s | exact; one failed attempt |
| RocksDB restart at TU 311 | 14,207,627 | 219.201x | 1.063 GB/s | exact |
| RocksDB late join at TU 311 | 6,591,093 | 200.427x | 1.255 GB/s | exact |
| RocksDB public budget 100 | 11,622,330 | 267.960x | **0.139 GB/s** | exact; 287,316 removals |

The eviction results diagnose an implementation defect for M5 performance, not a codec defect:
`evict_to_budget` finds every victim with a full vector scan. M5 needs a bounded queue/clock or
heap and explicit Region/Block eviction as well as public-Line eviction.

## Validation and reproduction

Warning-clean release builds use:

```sh
g++ -O3 -march=native -std=c++23 -DICE_LINE_CAP_LOG2=23 \
  -Wall -Wextra -Wpedantic -Werror \
  capability/cap_m4_main.cpp capability/cap_codec.cpp -lzstd
```

Focused gates:

```text
cap_header_test       PASS
cap_transport_test    PASS (actual two-process round trip)
cap_m2_test           PASS (rebind, rollback, op10 reservation)
cap_m4_test           PASS (components, bounds, staged C/F state)
cap_m4_test ASan/UBSan PASS
Python compilation    PASS
git diff --check      PASS
```

Complete matrix command:

```sh
python3 capability/run_m4_matrix.py \
  --output /tanksmall/scratch/ictmp/issue16-m4-evidence-20260817 \
  --suite all
```

Retained evidence:

```text
/tanksmall/scratch/ictmp/issue16-m4-evidence-20260817/m4-matrix.json
  sha256 da08cb9de731f6cef1efc81231e93621ecabd2f2b6a831cad77203e15ac4d5b9
/tanksmall/scratch/ictmp/issue16-m4-evidence-20260817/m4-matrix.tsv
  sha256 e01f59d9ad656507fc62442f6502529fce7ebd9c64da3de5dc1598702f157db4
/tanksmall/scratch/ictmp/issue16-m4-evidence-20260817/logs/
/tanksmall/scratch/ictmp/issue16-m4-evidence-20260817/SHA256SUMS
```

The independent implementer checkpoint `1c8bcb6f` was also reviewed. It establishes independent
mixed-lane frames, but does not implement the expanded actual-socket/component-selection and
transaction boundary above, so it remains a comparison checkpoint rather than the M4 result.

## M5 work opened by these measurements

M5 must now add and execute, without changing the accepted M4 semantic payload:

1. per-TU wire curves, cumulative C50, and the settled trailing-window H200 definition;
2. actual CACHE50 complements and a serializable 50%-raw state snapshot/resume;
3. standard, reverse, three deterministic shuffles, novelty-max, header/generated edit, revert,
   and A-B-A sequences;
4. 1/4/8/16/32 persistent F stores with sticky, round-robin, random, and failover assignments;
5. bounded Region, public-Line, and Block state with efficient removal and explicit C mirror repair;
6. worker restart and late join under each relevant state regime;
7. bounded inflight work and a 24-job host-concurrency measurement;
8. complete pipe-shaped timing, tail latency, RSS/state bounds, and a one-command scenario launcher.
