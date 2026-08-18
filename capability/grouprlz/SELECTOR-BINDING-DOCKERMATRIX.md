# Binding the causal selector over the ii-matrix docker cells

Measured on `ttuser@tt-quietbox2` 2026-08-18, cold, one encode per (project, profile)
cell, warm page cache, 3 reps per rate, decode always single-thread.

## Corpus: what actually exists

`/home/ttuser/ictmp/ii-matrix/<project>/<profile>/`. The authoritative marker of a
built, verified cell is `corpus.json` (`schema: ice-ii-corpus-v1`, carrying
`payload.path`, `raw_bytes`, `tu_count`, sha256).

**44 verified cells = 11 projects x 4 profiles**, 74.54 GB raw, 21,657 TUs:
catch2, cereal, eigen, fmt, leveldb, nlohmann-json, opencv, range-v3, re2, rocksdb,
spdlog against debian-gcc, conan-gcc, linuxbrew, fedora-clang-libcxx.

Two things that look like the corpus but are not:

- `all150.tsv` is a **scouting list**, not a build ledger. Of its 75 split-A
  `present=True` projects only draco and magnum have any built cell, and each has only
  `debian-gcc`. The 11 verified projects do not appear in `all150.tsv` at all.
- `heavy-batch-summary.tsv` is a **stale** log of one batch (it records
  `nlohmann-json linuxbrew FAIL 91/99` and `range-v3 fedora 0/243`, both of which are
  now complete 4-profile cells, and `catch2 total=107` against the current 861).

Two archive generations coexist and the layout differs between them:

| generation | archive | manifest location | payload |
|---|---|---|---|
| newer (has `corpus.json`) | `ii.tar.zst` | `<cell>/manifest.tsv` | `ii/` only |
| older | `<project>-<profile>.ii.tar.zst` | inside the tar | `ii/` + manifest + logs |

A cell expands to `ii/00000000.ii ... ii/NNNNNNNN.ii`; `manifest.tsv`
(`ordinal, relative_path, raw_bytes, sha256, original_path`) gives the corpus order and
its `raw_bytes` column sums exactly to the concatenated size. There is no `tus/` dir.

## Result over the 36-cell slice (9 projects x 4 profiles, 24.02 GB)

| profile | cells | raw | causal selector | / z19 | **x raw** | hindsight x | GRZ2 / P29+BSC wins |
|---|---:|---:|---:|---:|---:|---:|:---:|
| debian-gcc | 9 | 4.75 GB | 8,306,507 | 1.1139 | **571.88** | 678.58 | 3 / 6 |
| conan-gcc | 9 | 5.09 GB | 8,533,091 | 1.1128 | **596.25** | 708.90 | 3 / 6 |
| linuxbrew | 9 | 5.04 GB | 7,856,027 | 1.0674 | **640.91** | 746.85 | 3 / 6 |
| fedora-clang-libcxx | 9 | 9.15 GB | 8,858,425 | 1.0844 | **1032.60** | 1188.27 | 3 / 6 |
| **ALL** | **36** | **24.02 GB** | **33,554,050** | **1.0946** | **715.87** | 839.37 | **12 / 24** |

Reference points on the same 36 cells: GRZ2 alone 818.54x (0.957x z19), P29+BSC alone
667.13x (1.174x z19), whole-cell `zstd -19 --long=31` alone 783.59x.

Byte-exact decode of the selected codec: **36/36**. GRZ2 wire SHA-256 identical between
`-j 1` and `-j 8` on every cell.

### Does the selector hold on clang/libcxx as well as gcc?

Structurally yes, with one substitution. Every profile gives the same 3/6 split, and the
same three projects win with GRZ2 on the three gcc-family profiles: catch2, cereal,
range-v3. On fedora-clang-libcxx the membership changes -- **range-v3 drops out**
(GRZ2 grows to 1,036,737 vs P29+BSC 936,761) and **spdlog joins** (GRZ2 becomes
rate-legal at 1.221 GB/s where it was 0.875-0.908 elsewhere).

The 1.7x higher x on fedora-clang-libcxx is a corpus property, not a codec win: libc++
inflates preprocessed output about 1.9x (9.15 GB vs 4.75-5.09 GB for the identical nine
projects) while the compressed wire grows only ~7%. Both codecs get faster on it for the
same reason -- more redundancy per byte (re2 GRZ2 0.944 GB/s on fedora vs 0.529 on
debian).

### Where the causal selector loses

Hindsight best-of would be 839.37x; the causal rate-legal selector reaches 715.87x, a
**14.7% loss**. GRZ2 is the smaller wire on 32 of 36 cells but is rate-legal on only 17,
so on 19 cells the selector is forced onto the larger P29+BSC output.

That forcing is expensive enough to invert the comparison against plain zstd: the causal
selector emits **1.0946x whole-program z19**, i.e. more bytes than simply running
`zstd -19 --long=31` over the cell (783.59x vs 715.87x). GRZ2 alone would have been
0.957x z19. The size gate is also missed per-cell on **19 of 36** cells (fmt, leveldb,
nlohmann-json, re2 on all four profiles; spdlog on three), all of them cells where the
selector was forced onto P29+BSC.

## P29+BSC 1 GB/s floor: 36/36 miss

P29+BSC is the always-on baseline, so the scheme's wall-clock C rate is P29+BSC's rate.
Charged for its `loaded+interned` phase (`load_corpus` + `Interner::process`, which the
codec's printed per-stream proxy excludes), it measures **0.271-0.860 GB/s** across the
36 cells -- every cell misses. Even on its own published proxy basis it misses on 6
cells (fmt conan/debian/linuxbrew 0.771-0.794, re2 conan/debian/linuxbrew 0.802-0.861).

This reproduces the fixed-16 result (16/16 miss) on an independent corpus.

## Decode floor

The selected codec decodes below 500 MB/s single-thread on 5 of 36 cells: cereal on all
four profiles (GRZ2, 0.196-0.255 GB/s) and spdlog/fedora-clang-libcxx (GRZ2, 0.375).
Every cell where the selector was forced onto P29+BSC decodes comfortably.

## Reproducing

- `selector_dm_cell.sh <project> <profile>` -- expands the cell, concatenates in
  manifest order, verifies the concatenation against `corpus.json` `raw_bytes` and
  `tu_count`, takes the `zstd -19 --long=31 -T0` reference with `stat -Lc %s` on the real
  file, then measures both codecs at 1 thread and at their frozen parallel policy and
  round-trips GRZ2 through a separate decode process. Expanded inputs are deleted after.
- `selector_dm_sweep.sh <project>...` drives all four profiles per project.
- `selector_dm_summarize.py` -> `selector-binding-dockermatrix.tsv` +
  `selector-binding-dockermatrix.notes` (per-profile and overall aggregates).

Evidence under `/home/ttuser/selbind/dm/<project>-<profile>/` on quietbox2.

Contention note: local-oracle's `run_selector_matrix.sh` was traversing the same 44
cells on the same 16 physical cores throughout, at load average 2.7-4.4. Rates here are
therefore conservative.

opencv and eigen (the remaining 8 verified cells, 50.5 GB) are measured separately.
