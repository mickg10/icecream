# Full-file CODEC-50 reconstruction ledger and P22 `ROOT_SLICE` integration

## Decision

Do not port P22 `ROOT_SLICE` as the current product Root representation.  Its earlier Region-digest
capability result is exact, but the gain does not survive integration with the real dense-ID object
path.  Across the balanced full-file reconstruction corpus, P22 sends **810,256 more bytes** than the existing S1
flat-Block representation and reduces the byte-weighted ratio from **194.75x to 193.68x**.  It wins
only fmt, by 4,998 bytes, and loses the other 15 corpora.

The next work must stay on the full-file reconstruction path and target the Line-definition and Region-composition
legs.  Together they are 138,118,681 of the baseline's 146,624,393 bytes (94.2%).

## Scope and correctness

This is the first P22 comparison against the full-file `codec50.cpp` decoder contract.  Every run:

1. reads each chronological `.ii` manifest;
2. interns exact Lines and marker-delimited Regions;
3. sends Line definitions, Region-to-Line composition, Roots or Blocks, paths, missing lists, and
   all framing;
4. installs every object in an independent F-side store built only from serialized data;
5. expands every Root through Regions and Lines; and
6. compares every reconstructed TU byte-for-byte with the original `.ii`.

All 9,292 TUs in both 16-corpus matrices reconstruct exactly.  The result is an exact cold
empty-receiver reconstruction pass, not a two-plane projection.  The P22 candidate uses only
completed earlier Roots, checks every proposed range over exact dense Region IDs, and commits the
current Root only after expansion.

This is not yet total protocol acceptance.  The harness derives one dense-ID conversation from an
empty F and does not yet serialize generation-key-to-dense-ID associations for nonempty or
half-cold F state.  It also does not run these blocks through the actual C/F socket dispatcher or
execute the complementary half-cold object-cache scenario.  Those bytes and state transitions must
be measured before either overall target can be accepted.

## Full-file reconstruction aggregate ledger

| category | S1 baseline | P22 `ROOT_SLICE` | change |
|---|---:|---:|---:|
| Root | 3,687,803 | 5,107,508 | +1,419,705 |
| Line definitions | 118,780,613 | 118,780,613 | 0 |
| Region-to-Line definitions | 19,338,068 | 19,338,068 | 0 |
| Block definitions | 428,568 | 0 | -428,568 |
| path definitions | 828,100 | 828,100 | 0 |
| missing lists | 3,488,397 | 3,309,556 | -178,841 |
| framing | 72,844 | 70,804 | -2,040 |
| **total** | **146,624,393** | **147,434,649** | **+810,256** |
| byte-weighted ratio | **194.75x** | **193.68x** | -0.55% |
| equal-corpus harmonic ratio | **197.99x** | **197.00x** | -0.50% |
| cold-400 allowance | 71,386,679 | 71,386,679 | -- |
| gap to cold-400 | 75,237,714 | 76,047,970 | +810,256 |

The minimum measured C/F pipeline rate is 1.25 GB/s for S1 and 1.06 GB/s for P22.  Both clear the
1 GB/s capability gate, so the rejection is based on serialized size rather than speed.

## Per-corpus result

| corpus | S1 wire | P22 wire | change | S1 ratio | P22 ratio |
|---|---:|---:|---:|---:|---:|
| LLVM | 11,908,351 | 11,992,497 | +84,146 | 304.01x | 301.88x |
| RocksDB | 11,390,768 | 11,671,259 | +280,491 | 273.41x | 266.84x |
| DuckDB | 12,442,305 | 12,547,601 | +105,296 | 159.59x | 158.25x |
| Abseil | 7,072,905 | 7,148,650 | +75,745 | 364.91x | 361.05x |
| OpenCV | 10,811,760 | 10,882,381 | +70,621 | 428.33x | 425.55x |
| Godot | 79,906,920 | 80,032,124 | +125,204 | 74.25x | 74.13x |
| fmt | 1,388,886 | 1,383,888 | **-4,998** | 98.17x | **98.53x** |
| spdlog | 857,116 | 861,484 | +4,368 | 114.79x | 114.20x |
| Catch2 | 1,385,816 | 1,394,330 | +8,514 | 683.53x | 679.36x |
| nlohmann/json | 1,601,207 | 1,611,728 | +10,521 | 183.56x | 182.36x |
| range-v3 | 1,303,599 | 1,326,030 | +22,431 | 484.85x | 476.65x |
| Eigen | 2,017,939 | 2,026,972 | +9,033 | 1,750.43x | 1,742.63x |
| RE2 | 766,284 | 771,228 | +4,944 | 143.85x | 142.93x |
| LevelDB | 1,002,755 | 1,003,880 | +1,125 | 143.47x | 143.31x |
| simdjson | 1,920,187 | 1,930,887 | +10,700 | 243.92x | 242.57x |
| cereal | 847,595 | 849,710 | +2,115 | 385.68x | 384.72x |

## Why the capability result reversed

P22's standalone atom stream had to define or reference each Region by a 20-byte exact digest
triple before it could copy prior Root ranges.  Avoiding repeated digest atoms created the reported
29.67% structural win.

The full-file harness already assigns each immutable Region a dense `u32` identity and sends its
Line composition once.  S1 Roots therefore operate on compact Region IDs and name repeated ranges
with reusable flat Blocks.  P22 removes 428,568 bytes of Block definitions and saves 180,881 bytes
of missing/framing traffic, but its per-Root opcode/range programs add 1,419,705 Root bytes.  The net
is the measured 810,256-byte loss.  The earlier gain was representation-specific, not a missing
factor in the full-file dense-ID reconstruction.

This also validates the architecture rule that F's hot expansion path should remain dense-vector
based.  Exact digest or cache keys belong at object association boundaries; they should not be paid
again inside every Root program.

## Baseline stability finding

Godot contains 2,641,125 distinct Lines, exceeding the harness's fixed `2^21` long-Line table.  The
old run terminated with `line tbl`.  The harness now exposes `ICE_LINE_CAP_LOG2`; the balanced run
uses `2^23`, completes exactly, and peaks at 7.72 GiB while holding the full 5.93 GB corpus in memory.
This is a harness sizing correction, not the desired product allocation strategy.

## Next exact candidate and outcome

The full-file baseline identifies the next order unambiguously:

1. integrate P21 sorted split-front plus `BYTE_ARRAY` decoding into the full-file F store;
2. assign newly received Lines dense F-local IDs in decoded lexical order and translate
   Region-to-Line composition through that map, so no permutation block is hidden;
3. remeasure Region composition after remapping rather than substituting the 86,865,600-byte Line
   result arithmetically;
4. implement an actual complementary half-cold object cache over the same decoder; and
5. only then evaluate the next Line program against the remaining measured cold gap.

P21's independently exact Line result would save about 31.9 MB versus this baseline Line leg, but
that substitution alone would still leave roughly 43.3 MB above the cold-400 allowance before any
dense-ID remapping effect.  Therefore P21 is necessary integration work, not target closure.

That sequence has now been executed in
[`FULL-FILE-P21-16CORPUS-REPORT.md`](FULL-FILE-P21-16CORPUS-REPORT.md).  After the measured
lexicographic-ID Region penalty, integrated P21 reaches 118,901,432 bytes / 240.15x and remains
47,514,753 bytes above cold-400.  Generation association, the actual C/F path, and complementary
half-cold execution remain outside both reconstruction ledgers.

## Reproduction

Build the wide balanced harness:

```text
g++ -O3 -march=native -std=c++17 -DICE_LINE_CAP_LOG2=23 \
  linecache/codec50.cpp -o /tmp/issue16-codec50-p22 -lzstd
```

Run each of `/tanksmall/scratch/ictmp/corpus{,2..16}/manifest.txt` once with `--z 3` for S1 and
once with `--z 3 --prior-root` for P22.  Summarize with:

```text
python3 linecache/summarize_complete_codec50.py \
  --baseline-dir /tmp/issue16-complete-baseline-z3 \
  --candidate-dir /tmp/issue16-complete-p22-z3 \
  --output linecache/ml-artifacts/complete-codec50-p22-16corpus-summary.json \
  --tsv linecache/ml-artifacts/complete-codec50-p22-16corpus.tsv
```

The machine summary records SHA-256 for every retained log.  Retained logs are under
`/tmp/issue16-complete-baseline-z3` and `/tmp/issue16-complete-p22-z3`.
