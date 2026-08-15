# Corpus metrics: preprocessed (.ii) vs raw source at zstd -19 --long=31

Generated 2026-08-15T14:58:55Z with `zstd v1.5.7` on a 12-core Xeon Gold 6136.
Machine-readable: `../data/corpus-metrics.tsv` and `../data/corpus-catalog.json`;
per-corpus `METADATA.json` lives in each `corpusN/` alongside its `manifest.txt`.

All byte counts are **logical** (`stat -c%s` / `wc -c` / `wc -l`). `/tanksmall` is a
compressed filesystem, so `du` under-reports and is never used here.

## Method

* `.ii` set = exactly the files listed in that corpus's `manifest.txt`.
* raw source set = every `*.{c,cc,cpp,cxx,c++,C,h,hh,hpp,hxx,h++,tcc,inl,inc,ipp,ixx}`
  under the project's own checkout(s), excluding `/.git/`, `/build/`, `/CMakeFiles/`
  and any `*.ii`. Vendored / checked-in `third_party` **is** included: it is real
  shipped source.
* Compressed sizes are over the **pure concatenated content** of the file set
  (`cat file1 file2 ... | zstd -19 --long=31 | wc -c`) — no tar, so no per-file header
  overhead, and nothing is written to disk. Single-threaded, so the numbers are
  reproducible (multi-threaded zstd changes framing and shifts sizes slightly).

## The comparison this table does and does not make

**The raw-source column excludes system and toolchain headers** — libstdc++, glibc,
and the compiler's own intrinsic headers — which the `.ii` expand inline, over and
over, once per TU. So:

* `z19long src` = *"ship the project's source; the remote already has the toolchain."*
* `z19long .ii` = *"ship all preprocessed output."*

That is the meaningful icecream comparison — what a distributed-compile transport
actually has to move, versus what it would move if the far end could be trusted to
have an identical toolchain and only needed project source. It is **not** a
like-for-like compression benchmark of identical bytes: the two columns describe
different content. The `z19long ii/src` ratio is therefore a transport-cost ratio,
not a compression-efficiency ratio.

## Table

| corpus | project | TU | LOC src | LOC .ii | src MiB | .ii MiB | z19long src MiB | z19long .ii MiB | z19long ii/src |
|---|---|---|---|---|---|---|---|---|---|
| corpus † | LLVM | 1,238 | 17,233,314 | 137,913,644 | 747.5 | 3452.6 | 70.7 | 7.3 | 0.10 |
| corpus2 † | RocksDB | 622 | 879,340 | 110,485,751 | 31.3 | 2970.0 | 4.0 | 6.0 | 1.52 |
| corpus3 | DuckDB | 689 | 1,253,161 | 69,094,918 | 61.0 | 1893.7 | 6.9 | 6.8 | 0.99 |
| corpus4 † | abseil+protobuf | 700 | 918,167 | 90,359,789 | 33.1 | 2461.4 | 3.6 | 3.9 | 1.09 |
| corpus5 † | OpenCV | 1,506 | 2,077,209 | 159,014,620 | 74.4 | 4416.5 | 8.6 | 6.3 | 0.74 |
| corpus6 † | Godot | 2,207 | 6,288,358 | 175,636,352 | 348.0 | 5657.9 | 62.1 | 55.6 | 0.89 |
| corpus7 | fmt | 50 | 69,231 | 4,931,972 | 2.4 | 130.0 | 0.4 | 0.7 | 1.70 |
| corpus8 | spdlog | 34 | 32,543 | 3,516,541 | 1.1 | 93.8 | 0.2 | 0.5 | 2.38 |
| corpus9 | Catch2 | 857 | 74,985 | 36,035,922 | 2.6 | 903.4 | 0.2 | 0.8 | 3.39 |
| corpus10 | nlohmann-json | 99 | 150,463 | 10,071,362 | 5.5 | 280.3 | 0.4 | 0.8 | 2.00 |
| corpus11 | range-v3 | 259 | 101,916 | 22,769,868 | 3.4 | 602.8 | 0.3 | 0.7 | 2.48 |
| corpus12 † | Eigen | 650 | 384,476 | 108,282,245 | 14.8 | 3368.6 | 1.8 | 1.3 | 0.75 |
| corpus13 | re2 | 72 | 37,814 | 4,229,003 | 1.1 | 105.1 | 0.2 | 0.4 | 2.00 |
| corpus14 | LevelDB | 72 | 136,341 | 5,410,057 | 4.6 | 137.2 | 0.7 | 0.5 | 0.77 |
| corpus15 | simdjson | 153 | 398,960 | 16,636,934 | 15.3 | 446.7 | 0.5 | 1.0 | 1.81 |
| corpus16 | cereal | 84 | 48,681 | 11,297,707 | 1.9 | 311.8 | 0.2 | 0.5 | 2.01 |
| **TOTAL** | 16 projects | 9,292 | 30,084,959 | 965,686,685 | 1348.0 | 27231.9 | 160.9 | 93.1 | 0.58 |

**†** — `--long=31` (2 GiB) is zstd's **maximum** window; 32 is rejected even with
`--ultra`. The 6 daggered corpora have more than 2 GiB of `.ii`, so the
window cannot span the whole corpus and zstd never gets to see the duplication between
the earliest and latest TUs. Their `z19long .ii` is therefore a **lower bound** on the
cross-TU redundancy present — the true figure is better than shown, and a codec with
its own unbounded line dictionary is not subject to this ceiling at all. Affected:
corpus (LLVM), corpus2 (RocksDB), corpus4 (abseil+protobuf), corpus5 (OpenCV), corpus6 (Godot), corpus12 (Eigen).

### Reading the table

* **ii/src bytes** (`.ii MiB` ÷ `src MiB`) — raw expansion factor of preprocessing.
  Header-only libraries blow up hardest: Catch2 346x, Eigen 227x, range-v3 178x,
  cereal 165x. A few KB of test source pulls in megabytes of templates every time.
* **z19long ii/src** — the headline number: compressed preprocessed footprint over
  compressed project-source footprint. Under 1.0 means the compressed `.ii` stream is
  *smaller* than the compressed project source, which happens whenever the corpus is
  many near-identical expansions of a small amount of source.

### LLVM footnote: monorepo vs `llvm/` only

`corpus` is the one row where the source column is badly mismatched to the `.ii`
column. The checkout is the whole llvm-project monorepo — clang, mlir, libcxx, lldb,
all the test inputs — but only `llvm/` for the X86 target was preprocessed. Restricting
the source set to the `llvm/` subtree that was actually compiled:

| basis | files | src MiB | z19long src MiB | z19long ii/src |
|---|---|---|---|---|
| whole monorepo (table above) | 71,983 | 747.5 | 70.7 | 0.10 |
| `llvm/` subtree only | 8,127 | 151.9 | 22.3 | 0.33 |

The `llvm/`-only basis is the honest one for this corpus, and it is the row to quote:
`z19long ii/src = 0.33, not 0.10`. The other 15 corpora do not have this problem — their checkouts
are the project that was preprocessed. The **TOTAL** row inherits the same distortion,
so read the per-corpus rows, not the total, when the source column matters.

## Caveats

* **corpus (LLVM)** TU count comes from a *time-bounded* build harvest
  (`-save-temps=obj`, 720 s at -j12), not from a full replay of a compile database,
  so it is only approximately reproducible.
* **corpus12 (Eigen)** is capped at 650 of 1542 available TUs; Eigen's ~5.4 MB per-TU
  expansion would otherwise make it ~8 GB on its own.
* **corpus15 (simdjson)** is 153 of 156 TUs: 3 developer-mode targets fail to
  preprocess and are dropped.
* **corpus4** is two projects (abseil + protobuf) sharing one manifest; its source
  column sums both checkouts, and `git_url`/`git_commit` carry both.
* `.ii` size depends on where the checkout lives: `-E` emits a `# <line> "<abs path>"`
  marker at every include transition, so a longer source path inflates every corpus
  (measured: +1.3% for a path 31 characters longer, with the `.ii` byte-identical once
  both roots are normalized).

## Regenerating

`corpus-infra/regenerate_corpuses.sh [corpusN ...]` rebuilds corpora from scratch
(clone -> configure -> `-E` replay -> manifest -> TU verify);
`corpus-infra/snapshot_corpuses.sh` rebuilds the transfer archives;
`collect_cheap.sh` + `collect_zstd.sh` + `build_metadata.py` rebuild this table.
See `corpus-infra/README.md`.
