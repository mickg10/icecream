# COLD_1F_STICKY_CODEC at breadth: 22 native corpora

The six-corpus row said 5/6. **It does not generalise.** Broadened to every native
corpus with a whole-program z19 reference — 22 projects, 53.7 GB of raw `.ii`, same
method, same binaries, same in-memory clock:

> **Size bar 22/22. Rate bar 11/22. Both bars 11/22.**

The size half of the `/goal` is now universal on this set. The rate half is not, and
what separates pass from fail is a single corpus property.

## Method (unchanged from the six-corpus row, plus a per-corpus identity control)

`C_encode_ready` = corpus bytes resident in memory -> last selected payload fully
encoded, an explicit timestamp pair with nothing subtracted. GRZ2 is clocked by its own
ENC timer over a page-cache-warm mmap: the same bytes-already-in-memory basis. 3 reps,
uncontended box, `taskset -c 0-15`, 8 workers each. Rep spread is under 3.1% on every
row and under 1.5% on 15 of 22.

New for this pass: **each corpus carries its own identity control.** The un-instrumented
build (`codec50-refZ`, `588c1d1a…`, same source, same flags, same static MT libzstd,
`-DICE_LINE_CAP_LOG2=24`) is run over the same prefix with identical arguments, and its
complete wire census plus the retained literal-group wire must match the instrumented
build's byte for byte. **16/16 new corpora PASS**; the six earlier corpora were gated the
same way on catch2's full curve/component/literal comparison. The codec's own
`byte-exact=OK` round-trip verification holds on all 22.

## Result

| corpus | TUs | raw | P29 wire | /z19 | P29 GB/s | GRZ2 wire | /z19 | GRZ2 GB/s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| spdlog | 34 | 98,384,472 | 477,951 | 1.0180 | 0.282 | 436,106 | 0.9288 | 0.521 |
| re2 | 72 | 110,231,455 | 436,132 | 1.0194 | 0.336 | 401,210 | 0.9378 | 0.624 |
| fmt | 50 | 136,350,082 | 923,382 | 1.2558 | 0.251 | 662,261 | 0.9007 | 0.436 |
| leveldb | 72 | 143,868,249 | 616,460 | 1.1222 | 0.349 | 489,989 | 0.8920 | 0.570 |
| json | 99 | 293,917,707 | 1,011,863 | 1.2565 | 0.432 | 743,198 | 0.9229 | 0.783 |
| cereal | 84 | 326,899,429 | 459,195 | 0.9745 | 0.814 | 423,581 | 0.8989 | **1.165** |
| simdjson | 153 | 468,377,342 | 1,260,459 | 1.2380 | 0.590 | 955,208 | 0.9382 | 0.945 |
| range-v3 | 259 | 632,049,016 | 739,437 | 0.9985 | **1.128** | 628,319 | 0.8484 | **1.294** |
| arrow | 297 | 916,009,779 | 2,324,563 | 1.0761 | 0.715 | 1,925,523 | 0.8914 | 0.988 |
| catch2 | 857 | 947,252,235 | 907,327 | 1.1332 | **1.456** | 626,178 | 0.7821 | **1.487** |
| folly | 364 | 1,514,916,526 | 2,427,818 | 1.0955 | 0.854 | 2,682,229 | 1.2103 | **1.113** |
| duckdb | 689 | 1,985,715,205 | 7,877,084 | 1.1115 | 0.602 | 6,576,037 | 0.9279 | 0.815 |
| bitcoin | 621 | 2,311,143,894 | 3,507,130 | 1.1104 | **1.153** | 3,279,236 | 1.0383 | **1.155** |
| gcc | 780 | 2,542,399,239 | 12,749,467 | 1.0949 | 0.475 | 11,050,906 | 0.9490 | 0.747 |
| abseil | 700 | 2,581,008,467 | 5,056,453 | 1.2577 | **1.062** | 3,424,583 | 0.8518 | **1.169** |
| rocksdb | 622 | 3,114,320,596 | 8,533,638 | 1.3765 | 0.856 | 5,456,878 | 0.8802 | **1.009** |
| eigen | 650 | 3,532,268,956 | 1,143,631 | 0.9215 | **2.009** | 2,040,151 | 1.6439 | **1.541** |
| llvm | 1238 | 3,620,271,340 | 7,275,808 | 0.9972 | **1.158** | 7,638,087 | 1.0468 | 0.983 |
| opencv | 1506 | 4,630,994,774 | 6,737,137 | 1.0708 | **1.366** | 6,687,096 | 1.0628 | **1.148** |
| godot | 2207 | 5,932,762,185 | 35,085,071 | 0.6079 | 0.384 | 53,427,567 | 0.9257 | 0.347 |
| qtbase | 1205 | 7,050,790,103 | 9,309,538 | 1.0446 | **1.189** | 10,206,123 | 1.1452 | **1.175** |
| pytorch | 1742 | 10,802,074,073 | 8,935,043 | 0.9329 | **1.630** | 12,500,414 | 1.3051 | **1.284** |

**Selected codec (smaller wire), both bars:**

| corpus | selected | sel/z19 | sel GB/s | BOTH |
|---|---|---:|---:|:---:|
| spdlog | GRZ2 | 0.9288 | 0.521 | rate |
| re2 | GRZ2 | 0.9378 | 0.624 | rate |
| fmt | GRZ2 | 0.9007 | 0.436 | rate |
| leveldb | GRZ2 | 0.8920 | 0.570 | rate |
| json | GRZ2 | 0.9229 | 0.783 | rate |
| cereal | GRZ2 | 0.8989 | 1.165 | **TRUE** |
| simdjson | GRZ2 | 0.9382 | 0.945 | rate |
| range-v3 | GRZ2 | 0.8484 | 1.294 | **TRUE** |
| arrow | GRZ2 | 0.8914 | 0.988 | rate |
| catch2 | GRZ2 | 0.7821 | 1.487 | **TRUE** |
| folly | P29+BSC | 1.0955 | 0.854 | rate |
| duckdb | GRZ2 | 0.9279 | 0.815 | rate |
| bitcoin | GRZ2 | 1.0383 | 1.155 | **TRUE** |
| gcc | GRZ2 | 0.9490 | 0.747 | rate |
| abseil | GRZ2 | 0.8518 | 1.169 | **TRUE** |
| rocksdb | GRZ2 | 0.8802 | 1.009 | **TRUE** |
| eigen | P29+BSC | 0.9215 | 2.009 | **TRUE** |
| llvm | P29+BSC | 0.9972 | 1.158 | **TRUE** |
| opencv | GRZ2 | 1.0628 | 1.148 | **TRUE** |
| godot | P29+BSC | 0.6079 | 0.384 | rate |
| qtbase | P29+BSC | 1.0446 | 1.189 | **TRUE** |
| pytorch | P29+BSC | 0.9329 | 1.630 | **TRUE** |

- **Size: 22/22.** Worst case is folly at 1.0955x z19; the bar is 1.10x. Selected total
  **107,943,218 B over 53,692,005,124 raw = 497.4x raw, 0.8083x z19.**
- **Rate: 11/22.** Per codec, P29+BSC clears 1 GB/s on 9/22 and GRZ2 on 11/22.
- The 11 that pass carry **39.5 GB of the 53.7 GB (73.7%)** of raw in the set.

## What separates pass from fail: novel-material density, not size

Correlation of the selected codec's C-encode rate against candidate corpus properties,
n=22:

| property | Pearson r |
|---|---:|
| raw bytes | +0.400 |
| log10 raw bytes | +0.514 |
| TU count | +0.237 |
| **log10 whole-program compressibility (raw/z19)** | **+0.956** |
| **log10 distinct lines per MB of raw** | **−0.958** |

Sort the 22 by whole-program compressibility and the rate bar is almost a step function:

```
fail  fmt 185  spdlog 210  gcc 218  re2 258  leveldb 262  duckdb 280  json 365
      arrow 424  simdjson 460                                   godot 103 (also fail)
PASS  llvm 496  rocksdb 502  abseil 642                         folly 684 (the one miss)
      cereal 694  bitcoin 732  opencv 736  qtbase 791  range-v3 854
      pytorch 1128  catch2 1183  eigen 2846
```

A single threshold anywhere in **raw/z19 in [461, 496] classifies 21 of 22 correctly** —
every corpus below it fails the rate bar and every corpus above it passes, except folly.
Corpus size does
not: cereal passes at 327 MB while arrow fails at 916 MB and godot fails at 5.9 GB.

This is the Godot stage split generalising. There, 74% of `C_encode_ready` went to
**material construction and serialisation** — not entropy coding (5%), not interning
(20%). That cost is proportional to how much genuinely novel material the corpus
contains, so the encode rate falls as novel-material density rises. Godot is not an
outlier; it is the extreme of a continuum that runs through gcc, duckdb, leveldb and fmt.

**Consequence for the `/goal`:** the cold codec meets the size bar everywhere on this
set, and meets the rate bar exactly where the corpus is redundant enough that most input
bytes resolve to already-defined material. The two bars are not independent of the
corpus; a claim of the form "the cold codec does N at >= 1 GB/s" has to name the
redundancy regime it holds in.

## folly: the one corpus where the two bars pick different codecs

folly is the single inversion in the threshold, and it is instructive rather than noisy.
GRZ2 encodes it at **1.113 GB/s** — comfortably over the rate bar — but at **1.2103x
z19**, which fails the size bar. P29+BSC lands at **1.0955x z19**, inside the size bar,
but at **0.854 GB/s**. The selector takes the smaller wire and therefore fails on rate.
It is the only corpus in the 22 where each bar is satisfiable but not by the same codec.

## What this does and does not close

- **Closes:** the size half of COLD_1F_STICKY across 22 native projects, byte-exact,
  with a per-corpus identity control and both codecs measured on one basis.
- **Does not close:** the rate half. 11/22, with a measured, mechanistic separator.
- **Correction of record:** the earlier five-of-six and six-of-six rows were a
  favourable sample — their median whole-program compressibility (678x) sits well above
  this set's (499x), which lands right in the threshold region. The honest breadth number
  for both bars is **11/22**.

Data: `selector-cold-cencode-native22.tsv` (one row per corpus, both codecs, both bars,
rep spread, peak RSS, identity control). Harness `~/selbind/coldc2.sh`, sweep
`~/selbind/coldc2sweep.sh`, verdict `~/selbind/coldc2verdict.py`, mechanism
`~/selbind/coldc2mech.py` on quietbox2.
