# Fixed-16 complete P29 + bounded-BSC result

## Result

The same fixed **112-TU** literal-group policy was run through the complete P29
encoder, independently decoded group frames, ordinary P29 decoder, final `.ii` byte
comparison, and physical component ledger on all 16 fixed corpora.

- exact reconstruction and closed ledgers: **16/16**
- cold size gate: **10/16**
- measured C and F rate at least 1 GB/s: **14/16**
- cold size and speed together: **9/16**
- TU100 prefix gate: **9/10** eligible corpora
- TU200 prefix gate: **7/9** eligible corpora

This is the breadth result for one actual integrated candidate. It must not be described
as the earlier projected 13/16 best-of selector: that number combines several different
mechanisms, some of which have not yet been integrated behind one complete decoder.

## Cold size and measured rate

| corpus | target P29 | fixed-112 complete | cold gate | delta | size | C / F GB/s | joint |
|---|---:|---:|---:|---:|:---:|---:|:---:|
| LLVM | 9,328,055 | 7,282,357 | 8,369,687 | -1,087,330 | **PASS** | 4.014 / 6.114 | **PASS** |
| RocksDB | 9,823,481 | 8,537,705 | 6,970,751 | +1,566,954 | **FAIL** | 3.126 / 3.974 | **FAIL** |
| DuckDB | 9,589,726 | 7,873,485 | 7,883,713 | -10,228 | **PASS** | 1.716 / 2.774 | **PASS** |
| Abseil | 5,863,183 | 5,058,543 | 4,531,877 | +526,666 | **FAIL** | 4.570 / 5.842 | **FAIL** |
| OpenCV | 8,565,550 | 6,742,098 | 7,255,924 | -513,826 | **PASS** | 7.539 / 8.140 | **PASS** |
| Godot | 39,589,524 | 35,094,169 | 64,118,473 | -29,024,304 | **PASS** | 1.031 / 1.158 | **PASS** |
| fmt | 1,046,398 | 932,222 | 813,913 | +118,309 | **FAIL** | 0.798 / 1.032 | **FAIL** |
| spdlog | 539,513 | 477,912 | 519,412 | -41,500 | **PASS** | 0.926 / 1.273 | **FAIL** |
| Catch2 | 1,006,039 | 907,733 | 932,544 | -24,811 | **PASS** | 7.152 / 7.757 | **PASS** |
| nlohmann-json | 1,168,818 | 1,009,497 | 894,095 | +115,402 | **FAIL** | 1.350 / 1.822 | **FAIL** |
| range-v3 | 874,082 | 738,136 | 842,700 | -104,564 | **PASS** | 4.545 / 5.350 | **PASS** |
| Eigen | 1,377,958 | 1,141,797 | 1,542,344 | -400,547 | **PASS** | 13.132 / 9.927 | **PASS** |
| RE2 | 488,243 | 436,126 | 472,488 | -36,362 | **PASS** | 1.130 / 1.535 | **PASS** |
| LevelDB | 689,700 | 618,813 | 609,514 | +9,299 | **FAIL** | 1.186 / 1.598 | **FAIL** |
| simdjson | 1,408,595 | 1,258,040 | 1,139,682 | +118,358 | **FAIL** | 2.039 / 2.448 | **FAIL** |
| cereal | 512,588 | 459,374 | 541,711 | -82,337 | **PASS** | 3.217 / 3.962 | **PASS** |

The two rate misses are the one-group fmt and spdlog runs. Their complete raw inputs
are only 136 MB and 98 MB, so fixed harness and store construction costs are material;
the table records the measured complete-path proxy without amortizing them away.

## Chronological prefix gates

Frames are charged at their group's first TU. TU100 therefore includes literal material
through TU112, and TU200 includes material through TU224, while each zstd-6-long
reference contains only the requested prefix. Passing rows absorb that deliberate
lookahead charge.

| corpus | TU100 candidate / z6-long (delta) | TU200 candidate / z6-long (delta) |
|---|---:|---:|
| LLVM | 1,351,630 / 1,593,762 (-242,132) **PASS** | 2,361,052 / 2,884,547 (-523,495) **PASS** |
| RocksDB | 3,953,028 / 3,840,198 (+112,830) **FAIL** | 5,629,451 / 5,414,625 (+214,826) **FAIL** |
| DuckDB | 1,519,485 / 2,129,543 (-610,058) **PASS** | 2,124,228 / 2,847,389 (-723,161) **PASS** |
| Abseil | 1,583,152 / 1,645,017 (-61,865) **PASS** | 2,270,609 / 2,329,125 (-58,516) **PASS** |
| OpenCV | 624,012 / 639,458 (-15,446) **PASS** | 1,535,725 / 1,493,491 (+42,234) **FAIL** |
| Godot | 1,064,247 / 1,290,414 (-226,167) **PASS** | 2,057,414 / 2,211,547 (-154,133) **PASS** |
| Catch2 | 427,504 / 532,201 (-104,697) **PASS** | 537,411 / 617,728 (-80,317) **PASS** |
| range-v3 | 473,605 / 619,941 (-146,336) **PASS** | 675,619 / 897,361 (-221,742) **PASS** |
| Eigen | 675,102 / 768,113 (-93,011) **PASS** | 900,138 / 1,159,213 (-259,075) **PASS** |
| simdjson | 1,092,592 / 1,162,921 (-70,329) **PASS** | — |

## Literal-group selector behavior

| corpus | groups | zstd-3 / BSC selected | original literal wire | grouped wire | saved |
|---|---:|---:|---:|---:|---:|
| LLVM | 12 | 0 / 12 | 6,649,726 | 4,604,028 | 2,045,698 |
| RocksDB | 6 | 0 / 6 | 3,521,608 | 2,235,832 | 1,285,776 |
| DuckDB | 7 | 0 / 7 | 5,118,367 | 3,402,126 | 1,716,241 |
| Abseil | 7 | 0 / 7 | 2,563,162 | 1,758,522 | 804,640 |
| OpenCV | 14 | 0 / 14 | 5,979,976 | 4,156,524 | 1,823,452 |
| Godot | 20 | 0 / 20 | 13,672,703 | 9,177,348 | 4,495,355 |
| fmt | 1 | 0 / 1 | 379,562 | 265,386 | 114,176 |
| spdlog | 1 | 0 / 1 | 211,643 | 150,042 | 61,601 |
| Catch2 | 8 | 4 / 4 | 325,225 | 226,919 | 98,306 |
| nlohmann-json | 1 | 0 / 1 | 457,867 | 298,546 | 159,321 |
| range-v3 | 3 | 0 / 3 | 385,286 | 249,340 | 135,946 |
| Eigen | 6 | 0 / 6 | 753,867 | 517,706 | 236,161 |
| RE2 | 1 | 0 / 1 | 195,959 | 143,842 | 52,117 |
| LevelDB | 1 | 0 / 1 | 245,321 | 174,434 | 70,887 |
| simdjson | 2 | 0 / 2 | 529,961 | 379,406 | 150,555 |
| cereal | 1 | 0 / 1 | 182,720 | 129,506 | 53,214 |

Catch2 is the only corpus where the actual-byte selector retained zstd-3 groups
(4 of 8). All other groups selected BSC. The four-byte packed frame is included in
every group total; zstd-10 was not evaluated on this speed row and selected no group.

## Host-dependent source-reuse finding

The ordered `.ii` bytes are identical across the local and target hosts, but ordinary
P29 produced a different internal source-reuse plan for OpenCV and LevelDB:

| corpus | ordered `.ii` SHA-256 | local / target P29 | delta | local / target source-reused raw |
|---|---|---:|---:|---:|
| OpenCV | `1287b36659df27e5b30dd29c8cf997e3f1ab8c6a435d0b2c0ae83e72c944cb5e` | 8,563,234 / 8,565,550 | +2,316 | 2,476,812 / 2,458,815 |
| LevelDB | `83fca047531416b8827f7892f94403fc5273d84c8ef6e614daa2af762ce59166` | 685,350 / 689,700 | +4,350 | 1,599,820 / 1,571,028 |

The cause is P29 consulting host-local system-source paths while forming its reuse
plan. That is deterministic only when C and F see the same source package. Before M5
can use this path, source reuse must be tied to an explicitly matching package identity
or the exact referenced bytes must travel in the normal material stream. Silent reads
from unrelated host-local include trees are not a valid distributed-codec contract.

The fixed-112 totals above deliberately use the target host's own P29 plan. Their cold
gates remain the canonical whole-program references, so OpenCV's pass is unaffected and
LevelDB's miss is reported as the actual +9,299-byte target-host result.

## Verification and retained evidence

For every corpus the validator requires one exact row per TU, monotonic raw/wire
cumulatives, equality between curve and component totals, equality between every
per-TU component sum and physical wire, group-count closure, selector-count closure,
literal-frame file size equality, full raw-byte equality with the fixed reference,
and exact final reconstruction reported by the independent decoder.

Target build: `-O3 -march=znver3`, `OMP_NUM_THREADS=1`, `taskset -c 0-15` on
`ttuser@tt-quietbox2` (hostname `tt-quietbox`). Target evidence remains under
`/tmp/issue16-p29-bsc-integration-20260817`; the copied report evidence is under
`/tmp/issue16-p29-bsc-fixed16-evidence-20260817`.

Machine-readable results: `ml-artifacts/p29-bsc-group-fixed16.json` and
`ml-artifacts/p29-bsc-group-fixed16.tsv`.
