# P28: complete S1 ceiling and material-bearing superblock decision

## Outcome

P28 broadens S1's deterministic longest-previous-factor search from 64 to
1,024 candidates.  All 48 complete cold/bit-0/bit-1 executions reconstruct all
9,292 TUs byte-for-byte, and every corpus is no larger than the P27 control.
The change saves **11,474 bytes in every receiver state**:

| complete 16-corpus row | wire | ratio | cold-400 gap |
|---|---:|---:|---:|
| P27, S1 chain 64 | 93,980,202 | 303.837x | 22,593,523 |
| P28, S1 chain 1,024 | **93,968,728** | **303.874x** | **22,582,049** |

| receiver state | P27 | P28 | P28 ratio | target result |
|---|---:|---:|---:|---:|
| cold | 93,980,202 | **93,968,728** | **303.874x** | open by 22,582,049 |
| cache bit 0 | 62,151,794 | **62,140,320** | **459.519x** | passes by 80,633,038 |
| cache bit 1 | 43,863,608 | **43,852,134** | **651.158x** | passes by 98,921,224 |

This is the best measured S1 parameter row, but it is deliberately a small
cleanup rather than a claimed continuation factor.  The decisive result is the
complete structural ceiling: **no Root/Block-only coder can reach cold-400**.

## What was measured

`--structure-ceiling` retains the exact uncompressed S1 Root and Block streams
while the normal complete P27 encoder and independent F decoder run.  After
the timed pass it measures three stronger-than-product whole-run forms:

1. one zstd-3 frame for every Root concatenated together and one for every
   Block definition concatenated together;
2. one joint zstd-3 frame; and
3. one joint zstd-3 frame with long-distance matching and a 128 MiB window.

The diagnostic also computes the impossible zero-byte bound by deleting every
current Root byte, Block-definition byte, and four-byte Root frame.  The
whole-run floors omit TU boundaries and are therefore diagnostic lower-cost
forms, not proposed retryable wire frames.  The zero-byte row is stronger: it
is an absolute maximum saving for any coder confined to this ledger leg.

The aggregate result is:

| structural form | structure wire | saving vs P28 | projected total | projected ratio |
|---|---:|---:|---:|---:|
| current causal S1 | **4,300,340** | — | **93,968,728** | **303.874x** |
| whole-run, separate Root/Block zstd-3 | 4,366,716 | **-66,376** | 94,035,104 | 303.660x |
| whole-run, joint zstd-3 | 4,392,297 | **-91,957** | 94,060,685 | 303.577x |
| whole-run, joint zstd-3 + LDM/win27 | 4,392,216 | **-91,876** | 94,060,604 | 303.577x |
| impossible zero-byte Root+Block | **0** | **4,300,340** | **89,668,388** | **318.447x** |

Even free structure leaves **18,281,709 bytes** above the balanced cold-400
allowance.  Naive batching is worse because the current per-TU Root frames
preserve locally useful symbol neighborhoods while generation-local IDs drift
over the full run.

## Complete per-corpus zero bound

| corpus | P28 total | current structure | joint-LDM saving | zero-structure total | zero-structure ratio |
|---|---:|---:|---:|---:|---:|
| LLVM | 9,328,055 | 314,310 | 37,750 | 9,013,745 | 401.64x |
| RocksDB | 9,823,481 | 1,454,277 | -124,292 | 8,369,204 | 372.12x |
| DuckDB | 9,589,726 | 572,311 | -19,074 | 9,017,415 | 220.21x |
| Abseil | 5,863,183 | 732,936 | -50,564 | 5,130,247 | 503.10x |
| OpenCV | 8,563,234 | 305,603 | 20,100 | 8,257,631 | 560.81x |
| Godot | 41,693,465 | 440,446 | 58,356 | 41,253,019 | 143.81x |
| fmt | 1,046,398 | 105,142 | -23,535 | 941,256 | 144.86x |
| spdlog | 539,513 | 18,024 | -1,330 | 521,489 | 188.66x |
| Catch2 | 1,006,039 | 96,958 | 8,662 | 909,081 | 1,041.99x |
| nlohmann/json | 1,168,818 | 39,963 | -3,693 | 1,128,855 | 260.37x |
| range-v3 | 874,082 | 51,526 | 5,527 | 822,556 | 768.40x |
| Eigen | 1,377,958 | 38,348 | 7,801 | 1,339,610 | 2,636.79x |
| RE2 | 488,243 | 19,085 | 1,288 | 469,158 | 234.96x |
| LevelDB | 685,350 | 58,539 | -9,323 | 626,811 | 229.52x |
| simdjson | 1,408,595 | 39,846 | -1,269 | 1,368,749 | 342.19x |
| cereal | 512,588 | 13,026 | 1,720 | 499,562 | 654.37x |

The machine-readable JSON and TSV retain every input log digest and verify all
reported arithmetic.

## S1 parameter screen

The 16-corpus 1,024-chain row is monotonic: eleven corpora shrink and five are
byte-identical.  The largest savings are Godot 2,489 bytes, LLVM 2,239,
RocksDB 1,933, and Abseil 1,696.  S1 itself remains a subsecond pass: 0.4 s on
5.93 GB Godot and 0.3 s on 3.11 GB RocksDB on the local benchmark host.

Lowering the minimum match from three Regions to two is rejected.  It creates
many one-use Blocks whose definitions cost more than their Root-token saving:

| representative | min 3 / chain 64 | min 2 / chain 64 | change |
|---|---:|---:|---:|
| RocksDB | 9,825,414 | 9,827,601 | +2,187 |
| Abseil | 5,864,879 | 5,869,175 | +4,296 |
| fmt | 1,046,398 | 1,051,777 | +5,379 |
| LevelDB | 685,363 | 685,632 | +269 |

The current flat longest-match representation is therefore already close to
the useful cold limit for Root syntax.  Pair promotion and nested Blocks remain
useful warm-learning controls, but they cannot evade the measured 4.30 MB
absolute cold bound.

### Speed control

Three full Godot repetitions on the designated Zen 4 host produce the same
41,693,465-byte wire and exact reconstruction.  S1 takes 0.3 s.  The measured
already-ingested two-process proxy ranges are:

| repetition | C encode | F decode | pipeline minimum |
|---|---:|---:|---:|
| 1 | 1.067 GB/s | 1.194 GB/s | **1.067 GB/s** |
| 2 | 1.072 GB/s | 1.183 GB/s | **1.072 GB/s** |
| 3 | 1.074 GB/s | 1.197 GB/s | **1.074 GB/s** |

The full one-process research harness is 18.30–18.42 s wall, including the
6.5 s initial read/parse/intern pass, both simulated endpoints, and exact
verification.  As with P27, this proves the codec subphase floor; it does not
turn the complete serial preprocessor-pipe-to-wire path into a 1 GB/s result.

## Exact material-layout screens

Two semantic layouts were tested against all 112 recovered Godot blob members
as one favorable whole-generation batch.  All zstd 1/3/6/9 rows independently
decode every member, and a late-invalid control frame leaves F state unchanged.

| zstd-3 factor layout | complete frame | change vs P27 plain |
|---|---:|---:|
| P27 plain catalog order | **16,330,019** | — |
| group translations by original-string ID | 18,063,260 | +1,733,241 |
| exact translation COPY/PATCH from its original | 16,198,300 | -131,719 |

Grouping translations by original ID destroys valuable within-language
neighborhoods and is rejected.  Original-relative patches reduce the raw
translation stream from 62,540,670 to 60,954,798 bytes, but zstd already
captures almost all of that relation: only 131,719 compressed bytes remain.
This exact mode is retained in the standalone benchmark for reproducibility,
but its whole-generation upper-bound gain is far below a continuation factor
and it is not added to the integrated wire.

## Decision: superblocks must carry material

Keep P28's 1,024-chain S1 cleanup.  Stop treating Root-token refinements as a
route to cold-400.

The next superblock candidate must replace bytes in the material ledger.  Its
smallest composable form is one optional `MATERIAL_BLOCK_REF` operation inside
the existing Region materializer:

1. C tokenizes a completed Region into the existing exact operations and typed
   payload spans.
2. C learns immutable, variable-length operation programs only from completed
   TUs; the current TU is encoded from the previously committed store.
3. A candidate Block may contain fixed operations plus explicit typed slots for
   literal, integer-array, path, and ordinal values.  F stores the immutable
   program once and fills the slots from the current Region.
4. C compares the complete compressed current-TU candidate—including every new
   Block definition and slot stream—with unchanged P27.  It publishes only a
   candidate that repays itself immediately; otherwise P27 is emitted.
5. F expands the selected program into the unchanged Region decoder and commits
   new definitions only after the entire frame reconstructs exactly.

This keeps the block boundary clean: the Root layer still names Regions, while
the materializer optionally names a reusable exact program.  Unlike another
Root grammar, it can touch the 19.75 MB Region/control plane and the 41.16 MB
literal plane.  The residual census should choose the first typed family; a
general word dictionary, alpha-Line grammar, semantic control split, and the
two MO translation layouts above already have negative ceilings.

## Reproduction

Build:

```sh
g++ -O3 -march=native -std=c++17 -DICE_LINE_CAP_LOG2=23 \
  -Wall -Wextra -Wpedantic -Werror linecache/codec50.cpp \
  -lzstd -lz -pthread -o /tmp/codec50-p28
```

Run one complete row:

```sh
/tmp/codec50-p28 --manifest MANIFEST --z 3 \
  --mixed-regions --byte-array-lines --direct-ordinals \
  --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor \
  --structure-ceiling
```

Regenerate checked evidence:

```sh
PYTHONPATH=linecache python3 linecache/summarize_s1_p28.py \
  --p27-cold-dir /tmp/issue16-p27-cold \
  --p28-cold-dir /tmp/issue16-p27-s1-chain1024 \
  --p27-bit0-dir /tmp/issue16-p27-bit0 \
  --p28-bit0-dir /tmp/issue16-p28-bit0 \
  --p27-bit1-dir /tmp/issue16-p27-bit1 \
  --p28-bit1-dir /tmp/issue16-p28-bit1 \
  --output linecache/ml-artifacts/s1-p28-16corpus-summary.json \
  --tsv linecache/ml-artifacts/s1-p28-16corpus.tsv

PYTHONPATH=linecache python3 linecache/summarize_structure_ceiling.py \
  --log-dir /tmp/issue16-p27-s1-chain1024 \
  --output linecache/ml-artifacts/structure-ceiling-p28-16corpus-summary.json \
  --tsv linecache/ml-artifacts/structure-ceiling-p28-16corpus.tsv
```

Translation-layout ceiling:

```sh
g++ -O3 -march=native -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  linecache/mo_factor_bench.cpp -lzstd -o /tmp/mo-factor-layout-bench

/tmp/mo-factor-layout-bench \
  --input /tmp/issue16-godot-blob-inflated.raw \
  --lengths /tmp/issue16-godot-blob-inflated.raw.lengths \
  --transpose-translations

/tmp/mo-factor-layout-bench \
  --input /tmp/issue16-godot-blob-inflated.raw \
  --lengths /tmp/issue16-godot-blob-inflated.raw.lengths \
  --patch-translations
```

Retained logs are `/tmp/issue16-p27-structure-ceiling`,
`/tmp/issue16-p27-s1-chain1024`, `/tmp/issue16-p28-bit{0,1}`,
`/tmp/issue16-s1-*.log`, `/tmp/issue16-p28-mo-layout-{plain,transpose,patch}.log`, and
`tt-quietbox2:/tmp/issue16-p28-godot-z3-r{1,2,3}.log`.
