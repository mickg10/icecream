# Cross-docker-profile transfer for the P29+BSC online tier

**How much of a project's cold start does a prior of the SAME project built under a DIFFERENT
docker profile remove?** **20–39 % on average, never more than 54 %** — while a rebuild of the
*same* profile removes **98.8 %**. A prior from another build environment is worth something, but
it is not a warm start.

**Shared history is per-(project, profile), not per-project.**

72 pair measurements over 12 projects × 4 docker profiles, **144 P29+BSC runs, every one
byte-exact**. The corpus is local-oracle's frozen `verified-44-cells.tsv` (11 projects × 4
profiles), plus abseil carried as a labelled `v2-pool` bonus.

## Result (verified-44 only; abseil excluded from every aggregate)

`gap_closed = (cold − warm_xprofile) / (cold − warm_sameprofile)`. "survives" = the fraction of
the cold wire still on the wire after the cross-profile prior.

| prior → target | n | gap_closed mean | median | min | max | survives |
|---|---:|---:|---:|---:|---:|---:|
| conan-gcc → debian-gcc | 11 | 39.0 % | 37.1 % | 29.2 | 53.5 | 61.5 % |
| debian-gcc → conan-gcc | 11 | 38.3 % | 37.0 % | 28.9 | 52.7 | 62.1 % |
| linuxbrew → debian-gcc | 11 | 35.4 % | 32.7 % | 27.1 | 47.2 | 65.0 % |
| linuxbrew → conan-gcc | 11 | 32.9 % | 30.2 % | 26.2 | 45.1 | 67.5 % |
| debian-gcc → fedora-clang-libcxx | 11 | **20.8 %** | 17.8 % | 11.4 | 41.0 | 79.5 % |
| fedora-clang-libcxx → conan-gcc | 11 | **19.9 %** | 18.2 % | 12.6 | 39.4 | 80.4 % |

Transfer is symmetric — both directions of a pair agree to within 0.7 pt, a good sanity signal on
an experiment where the two runs share nothing but the corpus.

### The dashboard's two buckets

| condition | n | gap_closed mean | median |
|---|---:|---:|---:|
| **same-system** (debian-gcc ↔ conan-gcc, both gcc) | 22 | **38.6 %** | 37.1 % |
| **distinct-system** (gcc ↔ fedora-clang-libcxx) | 22 | **20.3 %** | 18.0 % |

Same-system transfer is worth **~1.9×** distinct-system transfer — the yellow/green ordering
holds. The honest headline is that even the best condition leaves **62 % of the cold-start cost
on the wire**.

### Floor

`warm_sameprofile / cold` = **1.17 % mean** (0.08 %–3.44 %). Rebuilding the identical profile
removes 96.6–99.92 % of the wire. That is the reference the 20–39 % is measured against.

## What actually costs you: the standard library, not the compiler

The four profiles are **not** two systems. From each cell's `environment.json` and the header
paths inside the `.ii`:

| profile | compiler | standard library |
|---|---|---|
| debian-gcc | g++ 12.2.0 (Debian 12) | libstdc++ **12** |
| conan-gcc | g++ 14.2.0 (Debian 14) | libstdc++ **14** |
| linuxbrew | **Homebrew clang 22.1.8** | libstdc++ **12** |
| fedora-clang-libcxx | clang 20.1.8 (Fedora 42) | **libc++** |

`linuxbrew` is a *clang that uses Debian's libstdc++-12*. That makes `linuxbrew ↔ debian-gcc` the
one pair that changes the **compiler alone** with the standard library held fixed, and
`debian-gcc ↔ conan-gcc` the pair that changes the **libstdc++ major alone** with the compiler
family held fixed. The axes separate cleanly:

| what changes between prior and target | n | gap_closed mean | median |
|---|---:|---:|---:|
| **compiler only** (gcc-12 → clang-22, libstdc++-12 both) | 11 | **35.4 %** | 32.7 % |
| **libstdc++ minor only** (12 ↔ 14, gcc both) | 22 | **38.6 %** | 37.1 % |
| both (clang/libstdc++-12 ↔ gcc/libstdc++-14) | 11 | **32.9 %** | 30.2 % |
| **standard-library family** (libstdc++ ↔ libc++) | 22 | **20.3 %** | 18.0 % |

Three things follow:

1. **Any profile change is already most of the damage.** Swapping only the compiler, with
   byte-identical standard-library headers on disk, still throws away ~65 % of the transfer —
   because the preprocessed text differs pervasively anyway: different predefined macros gate
   different `#if` branches *inside* libstdc++, different builtins are available, and line
   markers are emitted differently. **Sharing header files is not sharing preprocessed lines.**
2. Compiler-family and stdlib-minor changes cost about the same, and they compose roughly
   additively (35.4 % and 38.6 % alone, 32.9 % together).
3. **libc++ is the cliff.** Crossing standard-library *families* halves transfer again, to
   20.3 %. It is also the most expensive profile in absolute raw terms — fedora-clang-libcxx
   cells are 1.2–3.1× larger raw than the same project's gcc cells (catch2 873 MB → 2.66 GB,
   re2 85 MB → 217 MB, leveldb 175 MB → 322 MB).

**Deployment read:** key shared history by **(compiler, standard library)** — not by "system", and
not by project. Two gcc images that differ only in libstdc++ minor version share almost as little
as a gcc and a clang image do.

### Per-project spread

The pattern is uniform; only the level moves. **spdlog (52.7/53.5 % on the gcc pairs) and eigen
(50.8/51.1 %) transfer best**; both are header/template-dominated, so a large share of their lines
are their own headers, which survive a toolchain change. **range-v3 (28.9/29.2 %) transfers
worst.** rocksdb, opencv, catch2, cereal, leveldb, nlohmann-json, re2 and fmt fill the 11–43 %
band. Full per-row detail in `xprofile-transfer.tsv`.

## Cross-*project* transfer is strictly worse, and we already measured it

Not re-run here, deliberately. `GENERIC-DICT-COLDSTART.md` (commit c1daf4e) measured the
cross-project case directly: a 23.6 MB generic dictionary trained on 21.4 GiB of 12 disjoint
projects closes **5.0 %** of RocksDB's bootstrap, **10.0 %** of Abseil's, **1.8 %** of Firefox's
and **0.7 %** of LLVM-full's. That study showed the binding constraint is **distinct-line
coverage** — `gap_closed` tracked it almost exactly — and the same constraint governs here. A
different project shares far fewer lines with the target than the *same* project built with a
different toolchain does, so cross-project transfer is bounded below the 20–39 % measured on this
page. Both results are the same mechanism seen at two distances.

## Method

**Corpus — authoritative, verified, not asserted.** Every cell is the `ice-ii-corpus-v1` payload
`~/ictmp/ii-matrix/<project>/<profile>/ii.tar.zst` declared by that cell's `corpus.json`, with
`manifest.tsv` sitting **beside** the archive (the tar contains only `ii/`). All **44/44** cells
were checked against local-oracle's frozen `capability/selector/verified-44-cells.tsv` (@265ffb7,
copy retained in `xprofile-runs/`): payload sha256 matches, **and** the re-derived TU count and
summed raw bytes match the frozen values. Zero mismatches. Script: `gdict-harness/prep44.py`.
Cells need a 2 GB window — `zstd -dc --long=31 ii.tar.zst | tar -xf - -C DIR`.

**Independent corroboration.** For the 33 cells where local-oracle also ran P29 (`p29/full/` in
`matrix44-20260818T0230Z`), **my cold wire equals theirs exactly, 33/33** — different harness,
same byte. See `p29_cold_local_oracle` / `p29_cold_grouprlz` in `xprofile-runs/cell-inventory.tsv`.

**Codec, pinned per row.** Same P29+BSC configuration as the fixed-16-native runs. Each TSV row
carries three SHA-256 pins:

| pin | value |
|---|---|
| `p29_source_sha256` (`codec50.cpp`) | `2fccb899d518441ca81357f7543028bfe58fa734155c550926e22ff3e4fc125f` |
| `p29_stock_binary_sha256` | `8adb8b394970dd4e67aa82222874ad9a95c2d808c577c649a290b0d7d52a2493` |
| `p29_run_binary_sha256` (this study) | `0a092e877a0862d7bce782b22800c9a756711df10bf3961652a1e02449ed0f2e` |

The stock pin is **byte-identical to the binary local-oracle pins** in every cell's
`tooling.sha256`. The run binary is a rebuild that only raises two fixed interner capacities
(`gdict-harness/codec50-bigcap.patch`) and statically links zstd 1.4.8 built with
`ZSTD_MULTITHREAD=1` — both required, because the stock binary aborts (`short`) above 131 k
distinct short lines and Ubuntu's system libzstd 1.4.8 rejects `ZSTD_c_nbWorkers`. It reproduces
the stock binary's output byte-for-byte on the rocksdb (8,537,705) and firefox (37,391,954)
anchors, and now on all 33 cross-checked cold cells. Recipe: `gdict-harness/BUILD.md`.

**Pre-seed.** manifest = `[prior-profile TUs] ++ [target-profile TUs]`, one chronological pass
with retained state. The prior is content-hashed and installed on both sides, so **it never
appears on the wire**: the reported number is `cum_wire(last TU) − cum_wire(last prior TU)` from
the run's own per-TU curve. Prefixes are **cycle-padded to a multiple of 112** (repeating
already-interned, ~0-wire TUs) so no BSC literal group straddles the boundary.

**Byte-exactness.** `byte-exact=OK` on all **288/288** run stdouts (144 runs × 2 phases), and
`exact=true` on every row of every per-TU curve, checked separately for the test segment.
`xprofile-runs/xprofile-boundaries.tsv` lists prefix TUs, prefix cumulative wire, total cumulative
wire, subtracted test wire and both `exact` flags for all 144 runs — **zero** non-exact rows.
Every row of `xprofile-transfer.tsv` is re-derivable from that file alone.

## Caveats

* **Three projects are not TU-aligned across profiles** and are flagged `tu_aligned=false` on the
  affected rows (10 of 72): **eigen** 1516/1516/1522/1522, **range-v3** 537/537/575/575,
  **opencv** 1500/1500/1500/1485. They are *flagged, not truncated*: aligning on min would need a
  TU→source-file mapping that the cell manifests do not carry, and dropping arbitrary TUs would
  corrupt that profile's own `cold` and `warm_sameprofile` references. Cross-profile transfer is
  still well defined (the prior is only history); compare within a target profile, not across.
  The other eight projects are exactly rectangular.
* **abseil is `v2-pool`, not authoritative.** It has no `corpus.json`, so it is not an
  `ice-ii-corpus-v1` cell; it is carried from the legacy `abseil-<profile>.ii.tar.zst` archive,
  labelled in the `corpus_set` column and **excluded from every aggregate above**. Its rows sit
  squarely inside the verified-44 band (16.5–35.7 %).
* An earlier revision of this study (commit 3f3766b) measured the **wrong archives** — the
  earlier partial `<project>-<profile>.ii.tar.zst` packings rather than the authoritative
  `ii.tar.zst` — giving 107 TUs for catch2 instead of 861, 367 for rocksdb instead of 418, 8 for
  spdlog instead of 168. Every number on this page is from the verified payloads. That revision
  also claimed `raw-manifest.txt` was stale; **that claim was wrong and is retracted** —
  `raw-manifest.txt` agreed with the authoritative cells all along.
* `warm_sameprofile` is an *identical* rebuild — the true floor. A realistic incremental rebuild
  sits above it, which would raise every `gap_closed` slightly without changing any ordering.

## Artifacts

* `xprofile-transfer.tsv` — 72 rows: project, corpus_set, target/prior profile, per-side compiler
  and stdlib, axis labels, `tu_aligned`, test TUs and raw bytes, cold / warm_xprofile /
  warm_sameprofile wire, gap_closed, survives, decode_exact, and the three P29 SHA pins.
* `xprofile-runs/cell-inventory.tsv` — the 44 cells with compiler, stdlib, TU count, raw bytes,
  and the local-oracle vs grouprlz cold-wire cross-check.
* `xprofile-runs/verified-44-cells.tsv` — local-oracle's frozen manifest, retained verbatim.
* `xprofile-runs/xprofile-boundaries.tsv` — the 144-run boundary ledger.
* `gdict-harness/` — `prep44.py` (verify + extract), `mkman2.py` (cycle-padded manifests),
  `x44_sweep.sh` (the 12-condition design), `x44_collect.py`, `x44_bounds.py`, plus the codec
  build recipe and capacity patch.
