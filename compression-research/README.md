# compression-research

Everything behind the icecream cross-TU compression work: the corpus set it is measured
on, the scripts that can rebuild that set from bare clones, the measurements, and the
reports written off them.

Generated 2026-08-15T14:58:55Z.

## What the corpora are

Sixteen C++ projects, each preprocessed into a corpus of `.ii` translation units — the
exact bytes a distributed-compile transport would have to move. **9,292 TUs,
27231.9 MiB logical.**

| corpus | project | TU | .ii MiB | z19long .ii MiB | z19long ii/src |
|---|---|---|---|---|---|
| `corpus` | LLVM | 1,238 | 3452.6 | 7.3 | 0.10 |
| `corpus2` | RocksDB | 622 | 2970.0 | 6.0 | 1.52 |
| `corpus3` | DuckDB | 689 | 1893.7 | 6.8 | 0.99 |
| `corpus4` | abseil+protobuf | 700 | 2461.4 | 3.9 | 1.09 |
| `corpus5` | OpenCV | 1,506 | 4416.5 | 6.3 | 0.74 |
| `corpus6` | Godot | 2,207 | 5657.9 | 55.6 | 0.89 |
| `corpus7` | fmt | 50 | 130.0 | 0.7 | 1.70 |
| `corpus8` | spdlog | 34 | 93.8 | 0.5 | 2.38 |
| `corpus9` | Catch2 | 857 | 903.4 | 0.8 | 3.39 |
| `corpus10` | nlohmann-json | 99 | 280.3 | 0.8 | 2.00 |
| `corpus11` | range-v3 | 259 | 602.8 | 0.7 | 2.48 |
| `corpus12` | Eigen | 650 | 3368.6 | 1.3 | 0.75 |
| `corpus13` | re2 | 72 | 105.1 | 0.4 | 2.00 |
| `corpus14` | LevelDB | 72 | 137.2 | 0.5 | 0.77 |
| `corpus15` | simdjson | 153 | 446.7 | 1.0 | 1.81 |
| `corpus16` | cereal | 84 | 311.8 | 0.5 | 2.01 |

The set is deliberately lopsided. Header-only template libraries (Catch2, Eigen,
range-v3, cereal) sit at one end, where hundreds of small TUs each re-expand the same
megabytes of headers; large application codebases (LLVM, Godot, OpenCV) sit at the
other, where each TU is mostly its own content. Cross-TU redundancy differs by two
orders of magnitude across that range, which is the point — a codec that only works on
the Catch2 end of the spectrum has not been tested.

Full numbers, method and caveats: **[reports/corpus-metrics.md](reports/corpus-metrics.md)**.
Machine-readable: [data/corpus-metrics.tsv](data/corpus-metrics.tsv) and
[data/corpus-catalog.json](data/corpus-catalog.json).

## The toolchain caveat

Read this before quoting any ratio. A `.ii` is the project's source *plus every system
and toolchain header it pulls in*, expanded inline, once per TU. The raw-source figures
cover only the project's own checkout and exclude libstdc++, glibc and compiler
intrinsics entirely. So the two compressed columns answer different questions:

* `z19long src` — *ship the project's source; the far end already has the toolchain.*
* `z19long .ii` — *ship all preprocessed output.*

That pair is the meaningful distributed-compile comparison. It is **not** a
like-for-like compression benchmark of identical bytes.

Two more things that bite: `--long=31` (2 GiB) is zstd's maximum window, so the six
corpora larger than that are window-limited and their `.ii` number is a *lower bound*
on the redundancy present; and the LLVM row's source column covers the whole monorepo
while only `llvm/` was preprocessed. Both are flagged in the metrics report.

## Layout

| | |
|---|---|
| `corpus-infra/` | rebuild the corpora (`regenerate_corpuses.sh` + `recipes/`), build the transfer archives (`snapshot_corpuses.sh`), and reproduce the metrics (`collect_cheap.sh`, `collect_zstd.sh`, `build_metadata.py`) |
| `data/` | the metrics table, the consolidated corpus catalog, and the study/leave-one-out datasets |
| `reports/` | the metrics report plus the design notes, analyses and published HTML reports |

Corpus data itself is **not** in the repo — the `.ii` files are 27231.9 MiB
and the transfer archives are gitignored.

## Running it

```bash
cd corpus-infra

# rebuild any corpus from a bare clone: clone -> configure -> -E replay -> manifest -> verify TU
./regenerate_corpuses.sh                     # all 16
./regenerate_corpuses.sh corpus7 corpus9     # some
./regenerate_corpuses.sh --force corpus7     # redo configure + preprocess

# reproduce the metrics table
./collect_cheap.sh && ./collect_zstd.sh -P 6 && python3 build_metadata.py

# build the transfer archives (zstd -3 --long=27) for shipping to another box
./snapshot_corpuses.sh -P 5
```

Moving the corpora to another machine — transfer, checksum, unpack, and the manifest
path rewrite that is easy to forget — is documented in
[corpus-infra/REHYDRATE.md](corpus-infra/REHYDRATE.md).
Requirements: git, cmake + ninja, GCC, python3, zstd, and `scons` for corpus6 only.
