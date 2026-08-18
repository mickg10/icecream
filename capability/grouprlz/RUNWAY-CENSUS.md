# Short-runway P29-winner census: is the runway rule safe?

**Question.** The selector's classifier collapsed to one causal counter — *P29+BSC iff
`remaining_tus >= ~1093`* (total job count `>= ~1205`). Its known failures are short-runway
P29-winners: projects where P29+BSC wins the complete size despite few total TUs. Are they common
(rule breaks) or rare (rule confirmed)?

**Answer, in two halves.**

1. **On new corpora the shape is rare.** 19 previously unmeasured corpora, byte-exact. **Eleven
   of them have < 400 TUs and GRZ2 won every single one — zero short-runway P29-winners.** On the
   axis the rule was feared to fail, it did not fail once.
2. **But the rule is already violated inside the selector's own training corpus, and my rows add
   a third violator.** The verified-44 contains **five** P29-winning cells below the 1205
   threshold — rocksdb at **418 TUs in all four profiles** (p29/grz 0.89–0.97) and range-v3
   fedora at 575 (0.90). My new rows add **eigen native at 650 TUs, P29 winning by 1.8×**.

So: the *frequency* estimate is reassuring, the *threshold* is not. Details below, including a
finding that I think matters more than either — the winner is **not a stable property of a
project**, so it cannot be predicted from project identity or TU count alone.

## The census (19 new corpora, 0 non-exact)

Sorted by TU count. `rule_predicts` = the classifier under test (`p29` iff total_tus ≥ 1205).

| corpus | provenance | TUs | P29 bytes | GRZ2 bytes | winner | p29/grz | rule | ✓ |
|---|---|---:|---:|---:|---|---:|---|---|
| corpus8 (spdlog) | native-gcc11 | 34 | 477,912 | 436,106 | grz | 1.096 | grz | ✓ |
| magnum | debian-gcc v2-pool | 40 | 528,646 | 395,586 | grz | 1.336 | grz | ✓ |
| corpus7 (fmt) | native-gcc11 | 50 | 932,222 | 662,261 | grz | 1.408 | grz | ✓ |
| corpus13 (re2) | native-gcc11 | 72 | 436,126 | 401,210 | grz | 1.087 | grz | ✓ |
| corpus14 (leveldb) | native-gcc11 | 72 | 618,813 | 489,989 | grz | 1.263 | grz | ✓ |
| corpus16 (cereal) | native-gcc11 | 84 | 459,374 | 423,581 | grz | 1.085 | grz | ✓ |
| corpus10 (json) | native-gcc11 | 99 | 1,009,497 | 743,198 | grz | 1.358 | grz | ✓ |
| draco | debian-gcc v2-pool | 136 | 496,341 | 368,956 | grz | 1.345 | grz | ✓ |
| taglib | debian-gcc v2-pool | 148 | 547,167 | 407,355 | grz | 1.343 | grz | ✓ |
| corpus15 (simdjson) | native-gcc11 | 153 | 1,258,040 | 955,208 | grz | 1.317 | grz | ✓ |
| corpus11 (range-v3) | native-gcc11 | 259 | 738,136 | 628,319 | grz | 1.175 | grz | ✓ |
| corpus2 (rocksdb) | native-gcc11 | 622 | 8,537,705 | 5,456,878 | grz | 1.565 | grz | ✓ |
| **corpus12 (eigen)** | native-gcc11 | **650** | **1,141,797** | 2,040,151 | **p29** | **0.560** | grz | **✗** |
| corpus3 (duckdb) | native-gcc11 | 689 | 7,873,485 | 6,576,037 | grz | 1.197 | grz | ✓ |
| corpus4 (abseil) | native-gcc11 | 700 | 5,058,543 | 3,424,583 | grz | 1.477 | grz | ✓ |
| corpus9 (catch2) | native-gcc11 | 857 | 907,733 | 626,178 | grz | 1.450 | grz | ✓ |
| corpus (llvm) | native-gcc11 | 1238 | 7,282,357 | 7,638,087 | p29 | 0.953 | p29 | ✓ |
| **corpus5 (opencv)** | native-gcc11 | **1506** | 6,742,098 | 6,687,096 | **grz** | **1.008** | p29 | **✗** |
| corpus6 (godot) | native-gcc11 | 2207 | 35,094,169 | 53,427,567 | p29 | 0.657 | p29 | ✓ |

**Rule accuracy on these 19 rows: 17/19 (89.5 %).** Both misses are informative and unalike:

* **corpus12 / eigen, 650 TUs — P29 wins by 1.79×.** A long way below the threshold and a long
  way from marginal. This is the same project as one of the two known failures, now reproduced on
  a completely different corpus (native gcc-11 build, 650 TUs, 3.53 GB) from the matrix cell.
  Eigen is a genuine, reproducible, project-shaped violator.
* **corpus5 / opencv, 1506 TUs — GRZ wins by 0.8 %.** A miss on the *other* side, and marginal
  enough to be noise around the decision boundary rather than a structural failure.

## The threshold is already violated in the selector's own corpus

Straight from `matrix44-20260818T0230Z/selector-measurements.tsv`, P29-winning cells with
`tus < 1205`:

| project | profile | TUs | GRZ2 | P29 | p29/grz |
|---|---|---:|---:|---:|---:|
| rocksdb | conan-gcc | 418 | 2,679,877 | 2,555,954 | 0.954 |
| rocksdb | debian-gcc | 418 | 2,535,516 | 2,469,686 | 0.974 |
| rocksdb | fedora-clang-libcxx | 418 | 2,553,438 | 2,281,173 | 0.893 |
| rocksdb | linuxbrew | 418 | 2,444,715 | 2,278,544 | 0.932 |
| range-v3 | fedora-clang-libcxx | 575 | 1,036,737 | 936,761 | 0.904 |

**rocksdb at 418 TUs is a short-runway P29-winner by the strict `< 400`-ish definition, in all
four profiles** — five of the ten P29 wins in the verified-44 sit below the threshold the rule
uses. I cannot check this against your accounting because I do not have your `remaining_tus`
derivation or your lineage grouping; a `total >= 1205` reading of the rule calls all five wrong.
**Please reconcile before the rule is treated as validated** — if your lineage accounting already
excuses these, say so and I will drop the point.

## The finding I would act on first: the winner is not a project property

The same project flips winner when the build scope changes:

| project | corpus | TUs | winner |
|---|---|---:|---|
| rocksdb | **matrix cell** | 418 | **p29** (all 4 profiles) |
| rocksdb | **native corpus2** | 622 | **grz**, by 1.56× |
| range-v3 | matrix, 3 profiles | 537/575 | grz |
| range-v3 | matrix, fedora | 575 | **p29** |
| range-v3 | native corpus11 | 259 | grz |
| eigen | matrix, all 4 profiles | 1516/1522 | **p29** |
| eigen | native corpus12 | 650 | **p29** |

RocksDB is the sharp case: at 418 TUs P29 wins in every profile; at 622 TUs of the same project
GRZ2 wins by 56 %. Two different builds of one codebase, opposite answers, and the *larger* one
goes to GRZ — the opposite direction from what a runway rule predicts. Only eigen is stable
across every scope and profile measured.

A classifier keyed on project identity will therefore not transfer, and one keyed on TU count has
to explain rocksdb-418-p29 versus rocksdb-622-grz. Whatever the real feature is, it is a property
of *this build's content*, not of the project or of how many jobs remain.

### Features I checked that do not separate

I looked for a cleaner discriminator and did not find one, which is worth recording so nobody
re-spends the compute:

* **mean TU size** — eigen 5.43 MB/TU (p29) sits next to rocksdb 5.01 MB/TU (grz) and cereal
  3.89 MB/TU (grz). No clean split.
* **raw bytes per distinct line** (line-reuse depth) — eigen 35.6 kB is the extreme, but catch2
  at 13.4 kB is second and GRZ wins it comfortably.
* **P29 literal fraction** — eigen is lowest at 0.149 %, but llvm at 0.929 % is a P29 winner
  while several GRZ winners sit below it.

Each is suggestive and none is decisive on 19 points. Offered as leads, not as a proposed
feature.

## Method

Measurements are produced by **local-oracle's own runner**,
`~/issue16-selector-v1/tools/run_selector_cell.sh`, unmodified — so these rows are directly
comparable to `selector-measurements.tsv`. That runner does the complete P29+BSC encode, the
complete GRZ2 encode under the frozen policy
(`-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 -j 8 --gtu 112 --graw 512 --gadd 128 --hist 1024`), a
GRZ2 decode with `cmp` against the raw concatenation, and the whole-program zstd baselines.

**Anchor.** Before measuring anything new I ran the runner on an already-measured cell,
catch2/debian-gcc, and reproduced **both** codecs exactly: GRZ2 **627,742** and P29 **980,948**
against the selector's 627,742 / 980,948, with matching z19 788,682 and z6 1,012,912. Several
corpora independently re-confirm the P29 side against previously published fixed-16 values
(rocksdb 8,537,705; cereal 459,374; spdlog 477,912; fmt 932,222; re2 436,126; leveldb 618,813;
json 1,009,497; simdjson 1,258,040; range-v3 738,136).

**Cells.** The native corpora and the three v2-pool libraries are not `ice-ii-corpus-v1` cells,
so `gdict-harness/make_cell.py` packages each one into that exact shape — `ii/NNNNNNNN.ii`
(hardlinked, no second copy of ~30 GB), `manifest.tsv` with per-TU size and SHA-256, a
deterministic tar compressed `zstd -3 --long=31`, and a `corpus.json` whose payload digest is
computed from the archive actually written. `prepare_selector_cell.py` then verifies every TU's
byte count and digest against the manifest before either codec sees it. The packaged cells are
reusable by the selector directly.

**Exactness.** 19/19 rows carry `grz_decode_exact=true` (the runner's `cmp` receipt) and
`p29_byte_exact=true` (`byte-exact=OK`). Zero non-exact rows.

**Pins**, per row: P29 source `2fccb899…`, P29 run binary `0a092e87…` (the big-capacity/MT-zstd
rebuild, reproduces the stock `8adb8b39…` byte-for-byte — the stock binary aborts on the larger
corpora), GRZ2 binary `647883b7…` (local-oracle's frozen `grz2g-selector`).

## Scope and what is missing

* **The 150-pool sample could not be run as briefed.** Only 18 projects in `~/ictmp/ii-matrix`
  have `.ii` at all; the rest of the 150 exist as sources only and would need docker builds, which
  is outside a bounded measurement task. I substituted the closest available shapes: the eight
  small native libraries plus draco (136), magnum (40) and taglib (148) from the v2-pool package.
  If more small header/template-heavy libraries are wanted, the blocker is corpus *construction*,
  not measurement — say so and I will scope the builds separately.
* draco / magnum / taglib come from the older `<project>-<profile>.ii.tar.zst` package (no
  `corpus.json`); they are labelled `debian-gcc-v2pool` and should not be mixed into
  verified-44 aggregates.
* corpus18 (gecko, 9,646 TU), corpus20 (clickhouse, 11,741) and corpus25 (chromium, 3,798) were
  skipped as out-of-budget giants. They are long-runway, where the rule already predicts P29 and
  godot at 2,207 TUs agrees; they would test breadth, not the decisive question.
* corpus17/19/21/22/23/24 were excluded as already measured by the selector.

## Artifacts

* `runway-census.tsv` — 19 rows: corpus, provenance, total_tus, raw_bytes, mean_tu_bytes,
  P29_bytes, GRZ2_bytes, winner, p29_over_grz, short_runway_p29_winner, rule_predicts,
  rule_correct, z19_bytes, grz_decode_exact, p29_byte_exact, and the three SHA pins.
* `gdict-harness/make_cell.py` — package any ordered TU list as an `ice-ii-corpus-v1` cell.
* `gdict-harness/sweep.sh`, `gdict-harness/runway_collect.py` — the driver and collector.
* Retained runs on quietbox2: `~/runway/runs/<corpus>/` (wires, curves, timings, digests);
  packaged cells at `~/runway/cells/<corpus>/`.
