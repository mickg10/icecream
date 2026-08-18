# Cross-docker-profile transfer for the P29+BSC online tier

**How much of a project's cold-start does a prior of the SAME project built under a DIFFERENT
docker profile remove?** Answer: **17–37 % on average, never more than 51 %** — while a rebuild of
the *same* profile removes **99.2 %** of it. A prior from another build environment is worth
something, but it is not a substitute for warming up in the environment you are actually building
in.

48 project×pair measurements over 8 projects × 4 docker profiles, 96 P29+BSC runs, **every one
byte-exact**.

## Result

`gap_closed = (cold − warm_xprofile) / (cold − warm_sameprofile)`. "survives" = the fraction of
the cold wire still on the wire after the cross-profile prior.

| prior → target | n | gap_closed mean | median | min | max | survives |
|---|---:|---:|---:|---:|---:|---:|
| debian-gcc → conan-gcc | 8 | 37.3 % | 34.9 % | 32.8 | 51.1 | 63.0 % |
| conan-gcc → debian-gcc | 8 | 37.3 % | 35.8 % | 33.4 | 50.8 | 63.1 % |
| linuxbrew → debian-gcc | 8 | 35.1 % | 35.0 % | 28.1 | 45.7 | 65.2 % |
| linuxbrew → conan-gcc | 8 | 32.6 % | 31.2 % | 26.3 | 45.1 | 67.7 % |
| fedora-clang-libcxx → conan-gcc | 8 | **17.5 %** | 14.7 % | 11.2 | 39.4 | 82.7 % |
| debian-gcc → fedora-clang-libcxx | 8 | **17.4 %** | 12.9 % | 10.2 | 41.0 | 82.8 % |

Transfer is symmetric (both directions of a pair agree to within 0.1 pt), which is a good
sanity signal.

### The dashboard's two buckets

| condition | n | gap_closed mean | median |
|---|---:|---:|---:|
| **same-system** (debian-gcc ↔ conan-gcc, both gcc) | 16 | **37.3 %** | 35.4 % |
| **distinct-system** (gcc ↔ fedora-clang-libcxx) | 16 | **17.5 %** | 13.3 % |

Same-system transfer is worth **about 2× distinct-system transfer** — but the honest headline is
that even the *best* condition leaves **63 % of the cold-start cost on the wire**.

### Floor and ceiling

`warm_sameprofile / cold` = **0.80 % mean** (0.03 %–3.44 %). Rebuilding the identical profile
removes 96.6–99.97 % of the wire. That is the reference the 17–37 % is measured against.

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
family held fixed. So the axes separate:

| what changes between prior and target | n | gap_closed |
|---|---:|---:|
| compiler only (gcc-12 → clang-22, libstdc++-12 both) | 8 | **35.1 %** |
| libstdc++ minor only (12 ↔ 14, gcc both) | 16 | **37.3 %** |
| both (clang/libstdc++-12 ↔ gcc/libstdc++-14) | 8 | **32.6 %** |
| standard library **family** (libstdc++ ↔ libc++) | 16 | **17.5 %** |

Three things follow:

1. **Any** profile change is already most of the damage. Swapping only the compiler, with
   byte-identical standard-library headers on disk, still throws away ~65 % of the transfer —
   because the preprocessed text differs pervasively anyway (different predefined macros gate
   different `#if` branches *inside* libstdc++, different builtins, different line-marker
   emission). Sharing header files is not sharing preprocessed lines.
2. Compiler-family and stdlib-version changes cost about the same, and they compose roughly
   additively (35.1 % and 37.3 % each, 32.6 % together).
3. **libc++ is the cliff.** Crossing standard-library *families* halves transfer again, to
   17.5 %. It is also the most expensive profile in absolute terms — fedora-clang-libcxx cells
   are 1.3–3.1× larger raw than the same project's gcc cells (catch2 99 → 309 MiB, leveldb
   42 → 105 MiB, abseil 262 → 488 MiB).

**Deployment read:** key the shared history by (compiler, standard library) — not by "system".
Two gcc images that differ only in libstdc++ minor version share almost as little as a gcc and a
clang image do. Cross-profile priors are a ~1/3 discount at best, not a warm start.

### Per-project spread

The pattern is uniform across all 8 projects; only the level moves. **eigen is the outlier in the
good direction** (45–51 % on the gcc-ish pairs, 39–41 % even across libc++) — it is
template-header-dominated, so a large share of its lines are its own headers, which survive a
toolchain change. **rocksdb, abseil, nlohmann-json, catch2, leveldb, re2, spdlog all sit in a
tight 10–37 % band.** Full per-row detail in `xprofile-transfer.tsv`.

## Method

**Corpus.** The docker matrix at `~/ictmp/ii-matrix/<project>/<profile>/`. A cell is
`<project>-<profile>.ii.tar.zst` (+ `.sha256`, all verified). **Cells need a 2 GB window** —
plain `zstd -dc` fails with "Frame requires too much memory"; use
`zstd -dc --long=31 CELL | tar -xf - -C DIR`. A cell unpacks to `ii/00000000.ii …` plus
`manifest.tsv` (`ordinal | relative_path | raw_bytes | sha256 | original_path`), which is the
canonical TU order and the only trustworthy TU list — see the inventory caveat below.

**Set.** 8 projects have all four profiles: abseil (159 TU), catch2 (107), eigen (1516/1522),
leveldb (39), nlohmann-json (99/91), re2 (22), rocksdb (367), spdlog (8). Per-cell TU counts,
raw bytes, compiler and stdlib are in `xprofile-runs/cell-inventory.tsv`. Partial projects:
range-v3 (2 profiles), draco/magnum/taglib (1), cereal/fmt/opencv/simdjson (0).

**Codec.** Identical P29+BSC configuration and binary as the generic-dict study — see
`GENERIC-DICT-COLDSTART.md` and `gdict-harness/BUILD.md`. Anchors reproduced byte-identically.

**Pre-seed.** manifest = `[prior-profile TUs] ++ [target-profile TUs]`, one chronological pass
with retained state. The prior is content-hashed and installed on both sides, so **it never
appears on the wire**: the reported number is `cum_wire(last TU) − cum_wire(last prior TU)` from
the run's own per-TU curve. Prefixes are **cycle-padded to a multiple of 112** (repeating
already-interned, ~0-wire TUs) so no BSC literal group straddles the train/test boundary.

**Byte-exactness.** `byte-exact=OK` on all 96 runs, and `exact=true` on every row of every per-TU
curve, checked separately for the test segment. `xprofile-runs/xprofile-boundaries.tsv` lists
prefix TUs, prefix cumulative wire, total cumulative wire, subtracted test wire and both `exact`
flags for all 96 runs — **zero** non-exact rows. Every row of `xprofile-transfer.tsv` is
re-derivable from that file alone.

## Caveats

* **`raw-manifest.txt` next to a tarball is stale for several cells** and disagrees with the
  packed content (catch2 861 vs 107 TUs, spdlog 168 vs 8, re2 72 vs 22, leveldb 94 vs 39,
  rocksdb 418 vs 367). The canonicalizer errors on duplicates and never drops TUs, so those
  files were overwritten by a later re-preprocess after packing. **`manifest.tsv` inside the
  tarball is authoritative** and is what this study uses. Anyone sizing the matrix from
  `raw-manifest.txt` will overcount.
* Two projects are **not TU-aligned across profiles**: nlohmann-json (99 debian-gcc vs 91
  elsewhere) and eigen (1516 gcc vs 1522 clang). Cross-profile transfer is still well defined
  (the prior is only history), but their `cold`/`warm_sameprofile` references are per-profile, so
  compare within a target profile, not across.
* spdlog (8 TU) and re2 (22 TU) are small enough that their absolute wires are dominated by
  fixed overhead; they agree with the larger projects on ratios, which is why they are kept.
* An earlier revision of the padding helper truncated the pad when the prior had fewer TUs than
  the pad length, silently misaligning leveldb/re2/spdlog. Fixed (cycle-pad) and those three
  projects were re-run from scratch; the published numbers are all post-fix.
* `warm_sameprofile` is an *identical* rebuild — the true floor. A realistic incremental rebuild
  sits above it, which would raise every `gap_closed` slightly without changing any ordering.

## Artifacts

* `xprofile-transfer.tsv` — 48 rows: project, target/prior profile, compiler & stdlib axis
  labels, test TUs and raw bytes, cold / warm_xprofile / warm_sameprofile wire, gap_closed,
  survives, decode_exact.
* `xprofile-runs/cell-inventory.tsv` — the 32 cells with compiler, stdlib, TU count, raw bytes,
  tarball size.
* `xprofile-runs/xprofile-boundaries.tsv` — the 96-run boundary ledger.
* `gdict-harness/` — `extract_cells.sh` (sha-verified `--long=31` unpack + manifest emit),
  `mkman2.py` (cycle-padded prefix + test manifests), `xprofile_sweep.sh` (the 12-condition
  design), `xp_collect.py`, plus the codec build recipe.
