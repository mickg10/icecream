# Corpus coverage status for the transfer and model experiments

Status captured on 2026-08-17 from the retained manifests on `quietbox2` and
the replay-verified four-profile archive root. Counts below distinguish a
project corpus from a project/profile cell.

## Direct answer

There are currently **25 unique native project corpora containing 39,486
translation units**. The complete P25-P29 transfer matrix uses only the first
16 of them: **16 projects and 9,292 translation units**.

Separately, the heterogeneous archive harness has **44 verified pilot cells**:
11 projects built under each of four v1 profiles. Those cells contain 21,657
translation units and 74,539,226,911 raw bytes. They are useful development
data, but they are not included in the P25-P29 report and are not a completed
four-profile run of all available projects.

The 11 pilot projects are all already members of the native 25, so the current
number of distinct measured programs is 25, not 25 + 11.

## Native project inventory

| # | project | native corpus | TUs | in fixed-16 P25-P29 matrix |
|---:|---|---|---:|:---:|
| 1 | LLVM | `corpus` | 1,238 | yes |
| 2 | RocksDB | `corpus2` | 622 | yes |
| 3 | DuckDB | `corpus3` | 689 | yes |
| 4 | Abseil + Protobuf | `corpus4` | 700 | yes |
| 5 | OpenCV | `corpus5` | 1,506 | yes |
| 6 | Godot | `corpus6` | 2,207 | yes |
| 7 | fmt | `corpus7` | 50 | yes |
| 8 | spdlog | `corpus8` | 34 | yes |
| 9 | Catch2 | `corpus9` | 857 | yes |
| 10 | nlohmann-json | `corpus10` | 99 | yes |
| 11 | range-v3 | `corpus11` | 259 | yes |
| 12 | Eigen | `corpus12` | 650 | yes |
| 13 | RE2 | `corpus13` | 72 | yes |
| 14 | LevelDB | `corpus14` | 72 | yes |
| 15 | simdjson | `corpus15` | 153 | yes |
| 16 | cereal | `corpus16` | 84 | yes |
| 17 | GCC | `corpus17` | 780 | no |
| 18 | Firefox | `corpus18` | 9,646 | no |
| 19 | Qt6 | `corpus19` | 1,205 | no |
| 20 | ClickHouse | `corpus20` | 11,741 | no |
| 21 | PyTorch | `corpus21` | 1,742 | no |
| 22 | Folly | `corpus22` | 364 | no |
| 23 | Arrow | `corpus23` | 297 | no |
| 24 | Bitcoin | `corpus24` | 621 | no |
| 25 | V8 | `corpus25` | 3,798 | no |

The nine omitted native projects account for **30,194 TUs**. Consequently, the
16-project report cannot be described as the complete available-corpus result.

## Four-profile pilot inventory

The four v1 profiles are `debian-gcc`, `fedora-clang-libcxx`, `linuxbrew`, and
`conan-gcc`.

| project | verified cells | TU range per profile |
|---|---:|---:|
| Catch2 | 4 | 861 |
| cereal | 4 | 80 |
| Eigen | 4 | 1,516-1,522 |
| fmt | 4 | 51 |
| LevelDB | 4 | 94 |
| nlohmann-json | 4 | 99 |
| OpenCV | 4 | 1,485-1,500 |
| range-v3 | 4 | 537-575 |
| RE2 | 4 | 72 |
| RocksDB | 4 | 418 |
| spdlog | 4 | 168 |

The 44 archives are complete and replay-verified under corpus schema v1. The
four images remain v1 pilot inputs, however; no cell has yet been accepted
under a frozen v2 dependency-superset image set. Therefore:

```text
verified v1 pilot cells                 44
unique projects represented             11
frozen-v2 four-profile cells              0
Firefox four-profile cells                0
```

## Candidate expansion

The candidate inventory contains 150 additional CMake projects with an
estimated minimum of 100 C/C++ translation units. It is split 75/75 between
the implementer and local-oracle lanes. Seventy-five source trees have been
cloned (about 18 GB), but cloning is not corpus generation: none of those 150
projects has a frozen-v2 four-profile archive yet. Large non-CMake anchors such
as Firefox, GCC, Godot, and V8 remain separate adapters.

## Transfer-matrix accounting contract

Every current P25-P29 row is both:

```text
receiver learned state at TU 0 = empty
installed pre-shared S package = none
```

The human and machine matrices therefore report an explicit `S one-time
transfer` column of zero. When an S package is evaluated, report its compressed
transfer once per installation, immediately after the one-build column:

```text
TU 50 ... TU 300 | full build wire | S one-time transfer | 4x build wire
```

`S one-time transfer` is never folded into each build. The existing
pre-shared structural experiment is not yet a complete P29 row, so its package
bytes must not be pasted into this table as though the two codecs had already
been integrated and replayed.

## Required next coverage

1. Run the current exact research codec over all 25 native corpora, including
   the nine large omitted projects.
2. Publish the four-profile pilot cells as a clearly labelled development
   matrix, without treating them as frozen-v2 results.
3. Freeze the four v2 image digests, then grow exact project/profile cells and
   report coverage continuously rather than waiting for a nominal final size.
4. For every codec/start-state row, retain exact reconstruction, per-TU
   checkpoints, full build wire, one-time S bytes, and retained-build totals.
