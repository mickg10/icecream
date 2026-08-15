# Balanced cold bootstrap cross-fit: empty start versus pretrained start

Issue: `mickg10/icecream#16`

## Outcome

A charged exact-phrase package is a useful cold-start accelerator, but it is not the
main compression mechanism.  The valid cross-fit comparison now covers all 16
corpora, gives each corpus one vote, and contains no project-specific budget or
policy setting.

Across 28,554,671,510 raw `.ii` bytes, the structural online rows are:

| metric | empty start | pretrained start | change |
|---|---:|---:|---:|
| charged structural bytes | 63,085,685 | **59,497,403** | -3,588,282 B |
| byte-weighted ratio | 452.63x | **479.93x** | +6.03% |
| equal-corpus harmonic ratio | 328.83x | **361.22x** | +9.85% |
| final winners | - | **15 / 16** | cereal prefers empty |
| exact replay | 16 / 16 | 16 / 16 | pass |

The balanced startup curve gives the more important interpretation.  The package is
very expensive at TU 1, is still 1.17% behind overall at TU 5, moves 8.93% ahead by
TU 10, and peaks at an 11.39% equal-corpus advantage at TU 25.  The online learner
therefore catches most of the reusable structure quickly even without pretraining.
Pretraining removes startup discovery time; it does not replace chronological
learning or solve the complete cold target.

Removing any one corpus leaves the final equal-corpus improvement between 8.71% and
11.03%.  Removing DuckDB gives 10.13%.  The result is not carried by DuckDB or by any
other single project.

## Cross-fit design

The earlier package was trained on RocksDB and OpenCV, so only the other 14 corpora
were valid holdouts.  This experiment completes the matrix with a reciprocal package:

| package | training traces | evaluated targets | raw / charged package | SHA-256 |
|---|---|---|---:|---|
| A | RocksDB + OpenCV | the other 14 corpora | 248,831 / 79,138 B | `8fb47610e74309ea15bd1c0c2080ee07b5931518bb8e638b49ddb2785f322430` |
| B | LLVM + Godot | RocksDB + OpenCV | 248,586 / 82,087 B | `4a2950b18807e6981e87cbacb28311e5789c089fbc630669eae9ebb9a4c504a6` |

Every target is disjoint from the package used to score it.  Both package families use
the same exact phrase class, fixed budgets, and encoding policy:

- whole context runs plus overlapping 2/4/8/16/32-Region phrases;
- 256 KiB raw static-package budget;
- 512 KiB C-side online candidate budget in dense-ID accounting;
- promotion threshold 2 and at most 8 KiB promoted after one TU;
- encode and decode TU `t` from state through `t-1`, then learn `t`;
- first-profitable-use publication for target-learned phrases;
- actual zstd-3 definition and payload frames;
- complete package charge at TU 0;
- independent F reconstruction of every exact Region sequence.

The two new empty-start rows reproduce the previously retained RocksDB and OpenCV
rows byte-for-byte.  The reciprocal package is canonically exported, reloaded in two
fresh processes, and independently decoded.

This is a **cross-fit capability baseline**, not leave-one-out training and not one
common deployable package.  It deliberately answers the immediate question with two
disjoint package families while keeping all 16 targets valid.  A final package choice
still requires portable raw-source training and a proper leave-one-out or balanced
K-fold matrix.

## Corpus-balanced cold learning curve

The ratio columns are equal-corpus harmonic ratios.  The package count is charged in
full at TU 0.  `Eligible` is explicit because short corpora leave an absolute-TU
checkpoint after their final TU.

| TU | eligible | empty start | pretrained start | package change | package smaller now |
|---:|---:|---:|---:|---:|---:|
| 1 | 16 | 27.43x | 0.36x | -98.70% | 0 / 16 |
| 5 | 16 | 50.63x | 50.04x | -1.17% | 10 / 16 |
| 10 | 16 | 74.26x | 80.89x | +8.93% | 12 / 16 |
| 25 | 16 | 133.75x | 148.98x | +11.39% | 12 / 16 |
| 50 | 15 | 196.20x | 216.01x | +10.10% | 11 / 15 |
| 100 | 10 | 241.96x | 256.50x | +6.01% | 8 / 10 |
| 200 | 9 | 285.31x | 301.01x | +5.50% | 8 / 9 |
| final | 16 | 328.83x | **361.22x** | **+9.85%** | **15 / 16** |

The TU-50/100/200 rows are survivor cohorts, so their percentages must not be read as
the same 16-corpus population over time.  The full per-corpus curves remain the source
of truth.  Up through TU 34 all 16 corpora participate.

Twelve packages become permanently smaller between TU 4 and TU 6.  The delayed
crossings are Eigen at TU 93, OpenCV at TU 111, and Catch2 at TU 277.  Cereal never
repays the package.

## Per-corpus result

| corpus | empty final | pretrained final | change | permanently smaller from TU |
|---|---:|---:|---:|---:|
| LLVM | 780.97x | 819.50x | +4.93% | 6 |
| RocksDB | 187.89x | 189.92x | +1.08% | 4 |
| DuckDB | 271.16x | 288.91x | +6.55% | 4 |
| Abseil | 279.87x | 284.97x | +1.82% | 4 |
| OpenCV | 639.18x | 646.91x | +1.21% | 111 |
| Godot | 687.08x | 751.05x | +9.31% | 5 |
| fmt | 112.76x | 122.17x | +8.35% | 4 |
| spdlog | 212.50x | 258.73x | +21.76% | 4 |
| Catch2 | 804.49x | 818.99x | +1.80% | 277 |
| nlohmann/json | 358.47x | 406.54x | +13.41% | 4 |
| range-v3 | 709.99x | 811.83x | +14.34% | 4 |
| Eigen | 1,417.00x | 2,507.23x | +76.94% | 93 |
| RE2 | 281.34x | 336.44x | +19.58% | 6 |
| LevelDB | 174.75x | 199.16x | +13.96% | 4 |
| simdjson | 514.31x | 620.81x | +20.71% | 5 |
| cereal | **1,203.18x** | 1,004.57x | **-16.51%** | never |

The median final ratio change is +8.83%; the lower and upper quartiles of per-corpus
change are +1.81% and +16.96%.  The lower end is especially informative: RocksDB,
OpenCV, Catch2, and Abseil gain only 1.08%-1.82%.  Pretraining does not remove their
residual structural cost.

## Design consequence

The package should be treated as an optional bootstrap seed, not as the primary
decoder-visible state transition:

1. Keep the empty-start online learner as the required baseline and fallback.
2. Do not spend more design effort choosing a package from one project result.
3. Test a C-only seed mode: retain pretrained phrases as encoder candidates, send no
   complete package at TU 0, and publish only the exact definitions that repay
   themselves in the current TU.  F then retains the same simple installed-definition
   store it already needs for online learning.
4. Charge and report the C-only artifact size and memory even though it is not C-to-F
   traffic.  The current packages are about 80 KiB, so this experiment can remain
   bounded and isolated.
5. Train the eventual portable seed from raw source, then evaluate it with all-16
   leave-one-out or balanced K-fold curves.  Expanded `.ii` packages remain a
   capability control because their contents differ across build environments.

The C-only seed experiment is the smallest direct follow-up because it reuses the
existing first-profitable-use definition path.  It should need no new F predictor and
no new message kind: seeded definitions arrive through the same immutable definition
block as target-learned definitions.

## Acceptance boundary

All ratios in this report cover the exact Region-program structural layer only.  They
exclude Line text, Region-to-Line composition, values, residual literals,
missing-object exchanges, and the final combined framing.  They are not complete cold
compression ratios and must not be added to independently measured ratios.

The Python harness is executable semantics, not the product-speed implementation.
The accepted C/F implementation still needs the complete >=1 GB/s gate.

## Retained evidence and reproduction

Committed summaries:

- `linecache/ml-artifacts/online-bootstrap-pretraining-crossfit-16corpus.tsv`
- `linecache/ml-artifacts/online-bootstrap-pretraining-crossfit-16corpus-summary.json`

Summary SHA-256 values:

```text
2eb9a3da8c337d71bb1bbde73a1bc633fa9662ca040e351072a0c6652aefd83d  online-bootstrap-pretraining-crossfit-16corpus.tsv
0f30c30953cd5003dc9f247e2388931f253ab95f9c758dbfd570cac7c496742f  online-bootstrap-pretraining-crossfit-16corpus-summary.json
```

Retained raw reports and timing logs:

- `/tmp/online-bootstrap-common-llvm-godot-export.json`
- `/tmp/online-bootstrap-common-llvm-godot-export.{stdout,stderr}`
- `/tmp/online-bootstrap-common-llvm-godot.zst`
- `/tmp/online-bootstrap-crossfit-rocksdb.{json,tsv,stdout,stderr}`
- `/tmp/online-bootstrap-crossfit-opencv.{json,tsv,stdout,stderr}`

The reciprocal package and its two missing holdouts are reproduced with:

```sh
python3 linecache/online_bootstrap_curves.py \
  --train linecache/traces/ml-llvm.bin linecache/traces/ml-godot.bin \
  --test linecache/traces/ml-cereal.bin --max-tus 1 \
  --package-out /tmp/online-bootstrap-common-llvm-godot.zst \
  --static-budget 262144 --online-budget 524288 --thresholds 2 \
  --level 3 --model-level 3 --publication first-use --budget-basis ids32 \
  --row-set pretrained \
  --report /tmp/online-bootstrap-common-llvm-godot-export.json

for corpus in rocksdb opencv; do
  python3 linecache/online_bootstrap_curves.py \
    --package-in /tmp/online-bootstrap-common-llvm-godot.zst \
    --test "linecache/traces/ml-${corpus}.bin" \
    --online-budget 524288 --thresholds 2 \
    --level 3 --model-level 3 --publication first-use --budget-basis ids32 \
    --row-set all \
    --curve-tsv "/tmp/online-bootstrap-crossfit-${corpus}.tsv" \
    --report "/tmp/online-bootstrap-crossfit-${corpus}.json"
done
```

Package B training scans 3,445 TUs, 9,553,033,525 raw bytes, and 20,649,158 phrase
observations in 83.40 seconds.  Its export smoke uses 1,241,144 KiB peak RSS.  The
fresh RocksDB and OpenCV evaluations take 630.63 and 684.43 harness seconds, with
2,336,616 and 2,320,664 KiB peak RSS.  These are Python research costs, not product
estimates.

Verification:

- all 16 empty/pretrained online row pairs have identical target boundaries;
- all 32 selected online rows reconstruct every Region exactly;
- RocksDB and OpenCV empty rows reproduce the previous retained totals exactly;
- package B canonical export/import passes in both fresh target processes;
- both package hashes and charged sizes are recorded in the machine summary;
- checkpoint and final ledgers regenerate deterministically;
- TSV shape and exactness checks pass.
