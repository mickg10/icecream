# P24: exact mixed-Region materialization across 16 corpora

## Result

P24 integrates the mixed Region materializer into the complete P21 full-file reconstruction. It
reconstructs every byte of all 9,292 translation units through an independent F store and reduces
the measured cold wire from 118,901,432 to **108,378,454 bytes**. The byte-weighted ratio rises from
240.15x to **263.47x**, and the equal-corpus harmonic ratio rises from 232.16x to **276.38x**.

This is a real 10,522,978-byte improvement and wins on every corpus. It is not cold-400 closure. The
400x allowance is 71,386,679 bytes, so the current exact reconstruction remains **36,991,775 bytes
over budget** before generation-key association or the actual socket path is charged.

| balanced 16-corpus reconstruction | P21 | P24 | change |
|---|---:|---:|---:|
| raw bytes / TUs / exact corpora | 28,554,671,510 / 9,292 / 16 | same | -- |
| Line material | 86,988,259 | **80,123,571** | **-6,864,688** |
| Region material | 24,235,561 | **19,749,171** | **-4,486,390** |
| paths | 0 | 828,100 | +828,100 |
| total wire | 118,901,432 | **108,378,454** | **-10,522,978** |
| byte-weighted ratio | 240.15x | **263.47x** | +23.32x |
| equal-corpus harmonic | 232.16x | **276.38x** | +44.22x |
| gap to cold 400x | 47,514,753 | **36,991,775** | -10,522,978 |
| minimum transform/expand pipeline | 1.10 GB/s | **1.07 GB/s** | pass |

The unchanged Root, Block, missing-list, and harness-framing legs match P21 byte for byte. The
machine summary verifies that every mixed subcomponent closes its parent category exactly.

## Exact format and state transition

F stores every completed Region as an immutable raw byte span. C keeps, for each exact Line, the
first completed Region and byte offset where that Line appeared, plus an optional public dense Line
ID. A new Region carries its exact raw length followed by a sequence of the following operations:

| op | fields | F action |
|---|---|---|
| `LITERAL` | byte length | copy that many bytes from the literal stream |
| `PUBLISH_VIEW` | prior-Region delta, offset, length | copy the prior Region span and assign the next public Line ID |
| `PUBLIC_REF` | public Line ID | copy the already-published Region view |
| `BYTE_ARRAY` | style/count record plus raw `u8` values | render the exact decimal/hex initializer Line |
| `MARKER` | path ID, logical line, flags | render the exact line marker |
| `SOURCE_COPY` | path ID, source-Line index | copy one exact compiler-environment source Line |
| `SOURCE_PATCH` | path ID, source-Line index, prefix, suffix, middle | copy the source prefix/suffix and insert exact middle bytes |

The chronological rule is deliberately small:

1. The first private occurrence is materialized by the best applicable literal, byte-array,
   marker, or compiler-environment source operation. C records its Region and byte offset only
   after that Region is complete.
2. The first occurrence in a later Region may use `PUBLISH_VIEW`. Only a completed prior Region is
   eligible. The public ID is implicit and sequential on both sides.
3. Later occurrences use `PUBLIC_REF`.
4. F compares each completed raw Region with the exact interner truth in the harness, stores it,
   expands the ordinary S1 Root/Block stream, and compares the complete output with the original
   `.ii` file.

The streams are independent blocks:

- Region control;
- literal and source-patch middle bytes;
- byte-array style/control;
- byte-array values.

Each uses a persistent zstd-3 context, is flushed at every TU, length-framed, finished explicitly,
decompressed through an independent F context, and compared with its encoder input. The resulting
format therefore assumes an ordered C/F lane. A product implementation must either bind these
contexts to such a lane or measure independently decodable frames.

The source basis in this experiment is limited to files under `/usr/include`, `/usr/lib/gcc`, and
`/usr/local/include`. The research harness opens those paths on both sides. Product form must name a
known compiler-environment asset and use `SOURCE_COPY` only when C and F have the same asset; the
operation itself does not require another cache family.

## Per-corpus result

| corpus | raw bytes | P21 wire | P24 wire | saving | P24 ratio | pipeline GB/s |
|---|---:|---:|---:|---:|---:|---:|
| llvm | 3,620,271,340 | 10,160,230 | 9,332,043 | 828,187 | 387.94x | 5.37 |
| rocksdb | 3,114,320,596 | 10,043,263 | 9,829,948 | 213,315 | 316.82x | 2.01 |
| duckdb | 1,985,715,205 | 10,374,004 | 9,593,739 | 780,265 | 206.98x | 1.07 |
| abseil | 2,581,008,467 | 6,175,917 | 5,870,313 | 305,604 | 439.67x | 3.37 |
| opencv | 4,630,994,774 | 8,981,229 | 8,564,847 | 416,382 | 540.70x | 5.36 |
| godot | 5,932,762,185 | 61,694,666 | 56,078,855 | 5,615,811 | 105.79x | 1.59 |
| fmt | 136,350,082 | 1,244,155 | 1,047,430 | 196,725 | 130.18x | 1.25 |
| spdlog | 98,384,472 | 746,571 | 539,517 | 207,054 | 182.36x | 1.90 |
| catch2 | 947,252,235 | 1,197,783 | 1,006,627 | 191,156 | 941.02x | 5.91 |
| nlohmann-json | 293,917,707 | 1,381,972 | 1,169,044 | 212,928 | 251.42x | 3.06 |
| range-v3 | 632,049,016 | 1,098,147 | 873,391 | 224,756 | 723.67x | 5.64 |
| eigen | 3,532,268,956 | 1,843,320 | 1,377,632 | 465,688 | 2,564.01x | 7.84 |
| re2 | 110,231,455 | 663,005 | 488,152 | 174,853 | 225.81x | 2.35 |
| leveldb | 143,868,249 | 885,371 | 685,889 | 199,482 | 209.75x | 2.04 |
| simdjson | 468,377,342 | 1,682,048 | 1,408,414 | 273,634 | 332.56x | 3.48 |
| cereal | 326,899,429 | 729,751 | 512,613 | 217,138 | 637.71x | 4.78 |

## Remaining byte floor

The exact P24 mixed streams contain:

| component | wire bytes |
|---|---:|
| Region control | 19,749,171 |
| ordinary literal/patch bytes | 41,162,351 |
| byte-array control | 553,002 |
| byte-array values | **38,399,809** |
| selectors | 8,409 |

The byte-array value plane is now almost exactly the entire cold-400 gap. Godot alone sends
36,812,754 bytes for 38,949,170 actual `u8` values. These are not primarily counters or formatting;
most of the large Godot contribution is already-compressed generated content.

Four Godot translation generators provide a useful exact audit. Across 108 arrays they contain
29,453,332 DEFLATE bytes. All 108 inflate successfully, and `zlib.compress(payload, 9)` reproduces
all 108 original byte streams exactly on the measurement host. Their inflated payload is
114,071,960 bytes:

| coding of inflated payload | bytes | encode GB/s |
|---|---:|---:|
| zstd-3, default window | 26,122,414 | 0.208 |
| zstd-1, 128 MiB window + long matching | 20,904,236 | 0.286 |
| zstd-3, 128 MiB window + long matching | **17,818,563** | 0.199 |
| zstd-6, 128 MiB window + long matching | 16,449,482 | 0.093 |

This proves that decompressed translation blobs contain cross-blob structure, but it does not yet
justify a wire operation. The best permitted-level row saves at most 11,634,769 bytes before blob
metadata and exact regeneration are charged, remains far below 1 GB/s, and leaves more than 25 MB
of the aggregate cold gap. Exact DEFLATE reconstruction at F also has to meet the speed gate. A
compressed-blob operation should therefore remain a separate contender, not be folded into P24.

## Rejected adjacent operations

- A raw in-Region backward `LOCAL_REF` was byte-exact, but persistent zstd already represented the
  repeated bytes more cheaply. It changed fmt by +10,303 bytes, DuckDB by +159,043, and Godot by
  +116,306. It is removed from the selected format.
- Sending every referenced project source file is strongly negative. A causal admission experiment
  that waits for observed reuse avoids the broad regression, but every admitted file in the
  representative low-threshold rows still loses: fmt grows by 295,518 bytes, DuckDB by 12,367, and
  the first 1,000 Godot TUs by 53,142. Conservative admission sends no files and exactly reproduces
  ordinary P24. Keep compiler-environment source copies; do not add project-source packages from
  this evidence.

## Scope and next gates

P24 proves complete `.ii` reconstruction in one empty-receiver dense-ID conversation. It does not
yet prove the complete protocol. The following remain open:

1. generation key and 64-to-32 association for a nonempty or half-cold F;
2. complete complementary half-cold object-cache execution and the 200x objective;
3. actual C/F socket framing and ordered-lane lifecycle;
4. multi-F assignment, bounded eviction, and reorder/change/revert replay over that state;
5. a factor-sized treatment of the literal and already-compressed value planes.

The next codec contender must either expose structure inside the already-compressed blobs at more
than 1 GB/s or avoid retransmitting project-owned content through a separately charged reusable
asset. More Line-ID or Root-ID variations cannot close the measured gap.

## Reproduction and retained evidence

Build and run one corpus:

```sh
g++ -O3 -march=native -std=c++17 -DICE_LINE_CAP_LOG2=23 \
  -Wall -Wextra -Werror linecache/codec50.cpp -lzstd -o /tmp/codec50-p24
/tmp/codec50-p24 --manifest MANIFEST --z 3 --mixed-regions --byte-array-lines
```

Regenerate the committed summary after producing all 16 logs:

```sh
python3 linecache/summarize_mixed_region_codec.py \
  --p21-dir /tmp/issue16-complete-p21-final-z3 \
  --p24-dir /tmp/issue16-complete-p24-final-z3 \
  --output linecache/ml-artifacts/mixed-region-p24-16corpus-summary.json \
  --tsv linecache/ml-artifacts/mixed-region-p24-16corpus.tsv
```

Retained full matrix: `/tmp/issue16-complete-p24-final-z3`. Every log digest is recorded in the
machine summary.
