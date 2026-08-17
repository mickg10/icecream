# Pre-shared bootstrap transfer matrix: 16 exact structural corpora

## Outcome

When the pre-shared package is already installed at C and F, its package bytes are not build traffic. Under that accounting, the installed-package row is the smallest fixed method across the complete 16-corpus suite: **58,225,297 B**, saving **4,860,388 B (7.70%)** versus empty online learning.

The C-only seed remains nearly tied at **58,342,404 B** (7.52% below empty). The installed row is smaller by 117,107 B over the complete suite.

This result covers the exact **Region-program structural layer only**. It does not include first-use Line text, all value/literal streams, missing-object traffic, or final protocol framing, so its ratios do not establish total cold 400x. The current packages are target-disjoint cross-fit capability artifacts learned from expanded traces; they are not yet portable raw-source bootstrap models.

## Accounting

For every corpus, method, and TU boundary:

```text
build_wire_bytes = cumulative_charged_bytes - initial_model_wire_bytes
compression_ratio = cumulative_raw_bytes / build_wire_bytes
ideal_1_Gbit/s_seconds = build_wire_bytes / 125,000,000
ideal_1_GB/s_seconds = build_wire_bytes / 1,000,000,000
```

Definitions actually selected during the build remain charged. Only the immutable pre-installed package is excluded. Archive-storage size and raw `.ii` size are not used as substitutes for protocol bytes.

## Aggregate checkpoint matrix

Checkpoint cohorts contain only corpora that reach that absolute TU. `full` uses every corpus at its own complete endpoint. Ratios in this table are byte-weighted because the question is total transfer; the equal-corpus harmonic result is also retained in the machine summary.

| TU | corpora | raw GiB | empty MiB | installed MiB | C-only MiB | installed saved | C-only saved | empty ratio | installed ratio | C-only ratio | installed @1 Gbit/s | installed @1 GB/s |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 50 | 15 | 1.987 | 10.485 | 8.437 | 8.599 | 19.53% | 17.99% | 194.08x | 241.20x | 236.65x | 0.070774s | 0.008847s |
| 100 | 10 | 2.916 | 14.800 | 13.264 | 13.395 | 10.38% | 9.49% | 201.74x | 225.12x | 222.90x | 0.111265s | 0.013908s |
| 150 | 10 | 4.380 | 19.322 | 17.616 | 17.741 | 8.83% | 8.18% | 232.11x | 254.58x | 252.79x | 0.147774s | 0.018472s |
| 200 | 9 | 5.243 | 21.653 | 19.962 | 20.076 | 7.81% | 7.28% | 247.94x | 268.94x | 267.41x | 0.167455s | 0.020932s |
| 250 | 9 | 6.604 | 23.721 | 21.874 | 21.983 | 7.79% | 7.33% | 285.07x | 309.14x | 307.61x | 0.183493s | 0.022937s |
| 300 | 8 | 7.245 | 24.965 | 23.179 | 23.279 | 7.15% | 6.75% | 297.18x | 320.08x | 318.70x | 0.194444s | 0.024305s |
| full | 16 | 26.594 | 60.163 | 55.528 | 55.640 | 7.70% | 7.52% | 452.63x | 490.42x | 489.43x | 0.465802s | 0.058225s |

## Per-corpus requested-boundary matrix

Each cell is cumulative **empty / installed / C-only build wire in MiB**. A dash means the corpus completed before that TU. `full` is always present.

| corpus | TU 50 | TU 100 | TU 150 | TU 200 | TU 250 | TU 300 | full |
|---|---:|---:|---:|---:|---:|---:|---:|
| llvm | 0.804 / 0.626 / 0.629 | 1.023 / 0.824 / 0.828 | 1.211 / 1.007 / 1.011 | 1.575 / 1.366 / 1.370 | 1.742 / 1.532 / 1.536 | 1.893 / 1.681 / 1.684 | 4.421 / 4.138 / 4.136 |
| rocksdb | 3.014 / 2.812 / 2.782 | 7.671 / 7.468 / 7.428 | 9.755 / 9.554 / 9.505 | 10.870 / 10.664 / 10.605 | 11.236 / 11.033 / 10.965 | 11.514 / 11.305 / 11.234 | 15.807 / 15.560 / 15.471 |
| duckdb | 0.724 / 0.483 / 0.492 | 0.862 / 0.616 / 0.625 | 1.248 / 0.991 / 1.000 | 1.579 / 1.295 / 1.304 | 1.791 / 1.482 / 1.491 | 1.963 / 1.638 / 1.647 | 6.984 / 6.479 / 6.490 |
| abseil | 0.879 / 0.700 / 0.706 | 2.489 / 2.289 / 2.298 | 3.272 / 3.071 / 3.079 | 3.820 / 3.616 / 3.622 | 4.415 / 4.207 / 4.214 | 5.235 / 5.024 / 5.030 | 8.795 / 8.562 / 8.568 |
| opencv | 0.323 / 0.247 / 0.250 | 0.384 / 0.306 / 0.310 | 0.825 / 0.707 / 0.712 | 1.180 / 1.015 / 1.020 | 1.382 / 1.219 / 1.224 | 1.660 / 1.496 / 1.501 | 6.910 / 6.749 / 6.723 |
| godot | 0.333 / 0.228 / 0.230 | 0.406 / 0.294 / 0.296 | 0.451 / 0.336 / 0.339 | 0.530 / 0.414 / 0.417 | 0.773 / 0.635 / 0.638 | 0.913 / 0.767 / 0.770 | 8.235 / 7.458 / 7.458 |
| fmt | 1.153 / 0.989 / 0.994 | — | — | — | — | — | 1.153 / 0.989 / 0.994 |
| spdlog | — | — | — | — | — | — | 0.442 / 0.287 / 0.290 |
| catch2 | 0.224 / 0.176 / 0.180 | 0.295 / 0.239 / 0.244 | 0.401 / 0.337 / 0.342 | 0.431 / 0.362 / 0.368 | 0.473 / 0.397 / 0.403 | 0.493 / 0.417 / 0.423 | 1.123 / 1.028 / 1.033 |
| nlohmann-json | 0.617 / 0.449 / 0.453 | — | — | — | — | — | 0.782 / 0.614 / 0.619 |
| range-v3 | 0.398 / 0.272 / 0.276 | 0.491 / 0.356 / 0.361 | 0.614 / 0.472 / 0.477 | 0.742 / 0.575 / 0.581 | 0.830 / 0.654 / 0.659 | — | 0.849 / 0.667 / 0.673 |
| eigen | 0.317 / 0.290 / 0.409 | 0.434 / 0.343 / 0.473 | 0.682 / 0.500 / 0.632 | 0.927 / 0.654 / 0.790 | 1.080 / 0.715 / 0.852 | 1.296 / 0.851 / 0.989 | 2.377 / 1.268 / 1.419 |
| re2 | 0.341 / 0.205 / 0.209 | — | — | — | — | — | 0.374 / 0.237 / 0.241 |
| leveldb | 0.458 / 0.290 / 0.294 | — | — | — | — | — | 0.785 / 0.613 / 0.618 |
| simdjson | 0.667 / 0.459 / 0.461 | 0.745 / 0.530 / 0.532 | 0.864 / 0.640 / 0.643 | — | — | — | 0.869 / 0.644 / 0.647 |
| cereal | 0.234 / 0.209 / 0.233 | — | — | — | — | — | 0.259 / 0.235 / 0.259 |

## Complete per-corpus endpoints

| corpus | TUs | raw GiB | empty B | installed B | C-only B | installed saved | C-only saved | empty | installed | C-only |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| llvm | 1238 | 3.372 | 4,635,624 | 4,338,526 | 4,336,582 | 6.41% | 6.45% | 780.97x | 834.45x | 834.82x |
| rocksdb | 622 | 2.900 | 16,574,832 | 16,316,312 | 16,222,540 | 1.56% | 2.13% | 187.89x | 190.87x | 191.97x |
| duckdb | 689 | 1.849 | 7,323,010 | 6,793,962 | 6,805,445 | 7.22% | 7.07% | 271.16x | 292.28x | 291.78x |
| abseil | 700 | 2.404 | 9,222,174 | 8,978,077 | 8,984,575 | 2.65% | 2.58% | 279.87x | 287.48x | 287.27x |
| opencv | 1506 | 4.313 | 7,245,168 | 7,076,497 | 7,050,009 | 2.33% | 2.69% | 639.18x | 654.42x | 656.88x |
| godot | 2207 | 5.525 | 8,634,807 | 7,820,134 | 7,820,558 | 9.43% | 9.43% | 687.08x | 758.65x | 758.61x |
| fmt | 50 | 0.127 | 1,209,222 | 1,036,902 | 1,042,007 | 14.25% | 13.83% | 112.76x | 131.50x | 130.85x |
| spdlog | 34 | 0.092 | 462,996 | 301,114 | 304,544 | 34.96% | 34.22% | 212.50x | 326.73x | 323.06x |
| catch2 | 857 | 0.882 | 1,177,453 | 1,077,476 | 1,083,120 | 8.49% | 8.01% | 804.49x | 879.14x | 874.56x |
| nlohmann-json | 99 | 0.274 | 819,926 | 643,832 | 648,648 | 21.48% | 20.89% | 358.47x | 456.51x | 453.12x |
| range-v3 | 259 | 0.589 | 890,225 | 699,412 | 705,428 | 21.43% | 20.76% | 709.99x | 903.69x | 895.98x |
| eigen | 650 | 3.290 | 2,492,788 | 1,329,694 | 1,488,122 | 46.66% | 40.30% | 1417.00x | 2656.45x | 2373.64x |
| re2 | 72 | 0.103 | 391,804 | 248,506 | 252,641 | 36.57% | 35.52% | 281.34x | 443.58x | 436.32x |
| leveldb | 72 | 0.134 | 823,262 | 643,251 | 647,944 | 21.87% | 21.30% | 174.75x | 223.66x | 222.04x |
| simdjson | 153 | 0.436 | 910,698 | 675,329 | 678,634 | 25.84% | 25.48% | 514.31x | 693.55x | 690.18x |
| cereal | 84 | 0.304 | 271,696 | 246,273 | 271,607 | 9.36% | 0.03% | 1203.18x | 1327.39x | 1203.58x |

## Pre-shared artifacts

These bytes are installation/package footprint and are reported separately from build wire.

| SHA-256 | targets | definitions raw B | package B |
|---|---:|---:|---:|
| `4a2950b18807e6981e87cbacb28311e5789c089fbc630669eae9ebb9a4c504a6` | 2 | 248,586 | 82,083 |
| `8fb47610e74309ea15bd1c0c2080ee07b5931518bb8e638b49ddb2785f322430` | 14 | 248,831 | 79,134 |

## Interpretation and next experiment

The installed package is smaller at 13/16 complete endpoints; the C-only seed is smaller at 3/16. Once installation is correctly excluded, the old conclusion that C-only seeding wins 15/16 is no longer true: that conclusion compared against a row that charged the package on every build.

The difference between the two pre-shared placements is small at aggregate completion, while both remain materially better than empty start. The next decision should therefore be driven by the complete codec and its reorder/change curves, not this structural-only margin.

Required next measurements:

1. Train the same candidate representation from portable raw-source corpora and test it on project/profile holdouts.
2. Carry the selected definitions into the complete codec and account for Line text, values, residuals, missing replies, and framing.
3. Run standard, reverse, deterministic shuffles, dependency-root replacement, delayed change, revert, and half-cold state.
4. Require exact reconstruction and at least 1 GB/s on the complete C++ path.

## Machine-readable evidence

- `linecache/ml-artifacts/pre-shared-transfer-matrix-16corpus.tsv`
- `linecache/ml-artifacts/pre-shared-transfer-matrix-16corpus-aggregate.tsv`
- `linecache/ml-artifacts/pre-shared-transfer-matrix-16corpus-summary.json`
