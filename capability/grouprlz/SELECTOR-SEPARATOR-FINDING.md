# Where the frozen rule breaks, and the feature that explains it

## 1. Independent reproduction of local-oracle's fixed-16 result: confirmed

Computed from the `.tu` boundary maps (cumulative raw at TU112 is `tu[min(112,total)]`,
so this needs no re-encode) joined to the committed GRZ2 and P29+BSC fixed-16 size
ledgers. The frozen rule is `cumulative raw at TU112 >= 500 MB -> P29+BSC`.

| corpus | raw@TU112 | actual winner | rule picks | regret |
|---|---:|---|---|---:|
| rocksdb | 739,716,080 | GRZ2 | P29+BSC | **3,080,827** |
| eigen | 612,714,455 | P29+BSC | P29+BSC | ok |
| llvm | 404,433,872 | P29+BSC | GRZ2 | **355,730** |
| godot | 219,324,625 | P29+BSC | GRZ2 | **18,333,398** |
| other 12 | 98 MB - 363 MB | GRZ2 | GRZ2 | ok |

**frozen rule fixed-16 = 92,798,922 B = 307.7x raw = 0.9679x z19.** Oracle 71,028,967
(402.0x / 0.7409x); always-P29 78,568,007 (363.4x / 0.8195x); always-GRZ 90,616,449
(315.1x / 0.9452x). Every figure matches local-oracle's independently: the rule passes
<=1.10x z19, **fails 400x**, and is **worse than always-P29**. Only Eigen is picked right.

## 2. The feature that separates RocksDB from LLVM/Godot/Eigen

Ran the corrected-P29 TU112 probe (56c1744, stable Root tags, suffix-blind) plus the
bounded GRZ2 probe on all 16 fixed corpora and measured the decision-point features.
**Region reuse depth** -- `p29_region_occurrences / p29_regions` at TU112, i.e. how many
times P29 gets to re-use each structural region it has defined -- is the discriminator:

| corpus | label | region reuse depth | TU112 census ratio |
|---|---|---:|---:|
| eigen | **P29BSC** | **84.2** | 0.8931 |
| llvm | **P29BSC** | **39.3** | 0.8811 |
| godot | **P29BSC** | **36.4** | 0.8852 |
| **rocksdb** | **GRZ2** | **3.6** | 0.5385 |
| abseil | GRZ2 | 5.1 | 0.6135 |
| fmt | GRZ2 | 5.2 | 0.7172 |
| (9 others) | GRZ2 | 9.1 - 72.5 | 0.73 - 0.93 |

**RocksDB has the lowest region reuse depth of all sixteen -- 3.6 -- an order of magnitude
below the three P29 winners.** That is the causal story: P29+BSC pays off only when the
structural regions it defines are re-used many times, so its model has something to
amortize. RocksDB's fixed-16 first-112 TUs barely re-use regions at all, so P29 has
nothing to amortize while GRZ2's long-range byte copy still works.

By contrast raw extent points the wrong way (RocksDB is the *largest* raw@TU112 in the
set), and the census ratio does not separate: the three P29 winners sit in a narrow
0.881-0.893 band while GRZ winners span 0.539-0.935 on **both** sides of it.

## 3. It explains the RocksDB reversal between generations

| | region reuse depth @ TU112 | complete winner |
|---|---:|---|
| rocksdb, fixed-16 (622 TU, 3.11 GB) | **3.6** | GRZ2 |
| rocksdb, docker debian-gcc (418 TU) | 32.8 | P29+BSC |
| rocksdb, docker conan-gcc | 34.8 | P29+BSC |
| rocksdb, docker fedora-clang-libcxx | 49.9 | P29+BSC |
| rocksdb, docker linuxbrew | 56.0 | P29+BSC |

The same project flips label between generations, and the feature flips with it -- a
9x-15x change in region reuse depth. So RocksDB is not evidence that "the separator is
corpus-specific"; it is evidence that **raw extent was the wrong variable** and region
reuse depth is tracking the real mechanism.

## 4. What it is not: a standalone rule

Region reuse depth alone does **not** separate the docker 44. P29-win range 16.2-89.6
overlaps GRZ-win range 5.1-74.1; the best single threshold gives TP=4, FP=0, FN=6. The
adjacent pair is decisive: cereal/conan-gcc has depth 74.1 and GRZ2 wins, eigen/debian-gcc
has 76.2 and P29+BSC wins.

So the honest statement is: **region reuse depth is the right causal variable and it
answers the question that was asked -- it separates RocksDB from LLVM/Godot/Eigen and
explains the reversal -- but it is not by itself a sufficient classifier.** On fixed-16 a
two-sided band (`0.87 <= ratio <= 0.90` and depth `>= 30`) does score 3/3 with no false
positives, but that is two thresholds fitted to three positives on sixteen points, and a
non-monotone band with no mechanism behind it. It is recorded here as a curiosity, not a
proposal.

The frozen 500 MB rule remains the zero-cost baseline any richer classifier must beat.

## Provenance

P29 probes used local-oracle's reference build `codec50-stable-root-final-l23`
(`6cb0d9db...`), not my own 56c1744 rebuild: my `-lzstd` build links a libzstd without
multithreading, which is invisible until a corpus with real compressed blobs exercises
the path and then fails loudly with `blob zstd worker count: Unsupported parameter`
(Godot). My rebuild and local-oracle's were already proven byte-identical on
catch2/debian-gcc, so this is a build-environment substitution, not a codec change --
worth recording as a trap for anyone rebuilding codec50.
