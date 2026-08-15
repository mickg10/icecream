# Corpus metrics: preprocessed (.ii) vs raw source, z19 and z19+LDM

Generated 2026-08-15T14:34:42Z with `zstd v1.5.7` on a 12-core Xeon Gold 6136.
Source data: `corpus-metrics.tsv`; per-corpus `METADATA.json` lives in each
`/tanksmall/scratch/ictmp/corpusN/`.

All byte counts are **logical** (`stat -c%s` / `wc -c` / `wc -l`). `/tanksmall` is a
compressed filesystem, so `du` under-reports and is never used here.

## Method

* `.ii` set = exactly the files listed in that corpus's `manifest.txt`.
* raw source set = every `*.{c,cc,cpp,cxx,c++,C,h,hh,hpp,hxx,h++,tcc,inl,inc,ipp,ixx}`
  under the project's own checkout(s), excluding `/.git/`, `/build/`, `/CMakeFiles/`
  and any `*.ii`. Vendored / checked-in `third_party` **is** included: it is real
  shipped source.
* Compressed sizes are over the **pure concatenated content** of the file set
  (`cat file1 file2 ... | zstd ... | wc -c`) — no tar, so no per-file header overhead.
* `z19` = `zstd -19 -T0` (8 MB window). `z19long` = `zstd -19 --long=31 -T0`
  (2 GiB long-distance-matching window, i.e. the whole corpus is in reach).

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

| corpus | project | TU | LOC src | LOC .ii | src MiB | .ii MiB | z19 src MiB | z19long src MiB | z19 .ii MiB | z19long .ii MiB | ii/src bytes | z19long ii/src | long gain (z19/z19long .ii) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| corpus | LLVM | 1,238 | 17,233,314 | 137,913,644 | 747.5 | 3452.6 | 72.9 | 70.7 | 50.3 | 7.3 | 4.62 | 0.10 | 6.93 |
| corpus2 | RocksDB | 622 | 879,340 | 110,485,751 | 31.3 | 2970.0 | 4.1 | 4.0 | 65.5 | 6.0 | 94.92 | 1.52 | 10.83 |
| corpus3 | DuckDB | 689 | 1,253,161 | 69,094,918 | 61.0 | 1893.7 | 7.0 | 6.9 | 30.5 | 6.8 | 31.03 | 0.99 | 4.47 |
| corpus4 | abseil+protobuf | 700 | 918,167 | 90,359,789 | 33.1 | 2461.4 | 3.8 | 3.6 | 33.9 | 3.9 | 74.26 | 1.09 | 8.63 |
| corpus5 | OpenCV | 1,506 | 2,077,209 | 159,014,620 | 74.4 | 4416.5 | 8.7 | 8.6 | 20.7 | 6.3 | 59.40 | 0.74 | 3.30 |
| corpus6 | Godot | 2,207 | 6,288,358 | 175,636,352 | 348.0 | 5657.9 | 62.7 | 62.1 | 102.6 | 55.6 | 16.26 | 0.89 | 1.85 |
| corpus7 | fmt | 50 | 69,231 | 4,931,972 | 2.4 | 130.0 | 0.4 | 0.4 | 1.9 | 0.7 | 53.96 | 1.70 | 2.65 |
| corpus8 | spdlog | 34 | 32,543 | 3,516,541 | 1.1 | 93.8 | 0.2 | 0.2 | 0.6 | 0.5 | 85.58 | 2.38 | 1.44 |
| corpus9 | Catch2 | 857 | 74,985 | 36,035,922 | 2.6 | 903.4 | 0.2 | 0.2 | 8.5 | 0.8 | 345.67 | 3.39 | 10.45 |
| corpus10 | nlohmann-json | 99 | 150,463 | 10,071,362 | 5.5 | 280.3 | 0.4 | 0.4 | 3.9 | 0.8 | 50.54 | 2.00 | 5.05 |
| corpus11 | range-v3 | 259 | 101,916 | 22,769,868 | 3.4 | 602.8 | 0.3 | 0.3 | 5.0 | 0.7 | 178.34 | 2.48 | 6.88 |
| corpus12 | Eigen | 650 | 384,476 | 108,282,245 | 14.8 | 3368.6 | 1.8 | 1.8 | 4.4 | 1.3 | 227.09 | 0.75 | 3.31 |
| corpus13 | re2 | 72 | 37,814 | 4,229,003 | 1.1 | 105.1 | 0.2 | 0.2 | 0.8 | 0.4 | 98.14 | 2.00 | 2.05 |
| corpus14 | LevelDB | 72 | 136,341 | 5,410,057 | 4.6 | 137.2 | 0.7 | 0.7 | 1.4 | 0.5 | 30.15 | 0.77 | 2.56 |
| corpus15 | simdjson | 153 | 398,960 | 16,636,934 | 15.3 | 446.7 | 0.6 | 0.5 | 3.2 | 1.0 | 29.18 | 1.81 | 3.25 |
| corpus16 | cereal | 84 | 48,681 | 11,297,707 | 1.9 | 311.8 | 0.2 | 0.2 | 0.9 | 0.5 | 165.46 | 2.01 | 1.81 |
| **TOTAL** | 16 projects | 9,292 | 30,084,959 | 965,686,685 | 1348.0 | 27231.9 | 164.2 | 160.9 | 334.2 | 93.1 | 20.20 | 0.58 | 3.59 |

### Reading the derived columns

* **ii/src bytes** — raw expansion factor of preprocessing. Header-only libraries
  (Catch2 346x, Eigen 227x, cereal 165x) blow up hardest: a few KB of test source
  pulls in megabytes of templates every time.
* **z19long ii/src** — the headline number: compressed preprocessed footprint over
  compressed project-source footprint. Under 1.0 means the compressed `.ii` stream is
  *smaller* than the compressed project source, which happens whenever the corpus is
  many near-identical expansions of a small amount of source.
* **long gain** — how much the 2 GiB LDM window buys over the default 8 MB one on
  the `.ii` side. This is the cross-TU redundancy that an 8 MB window cannot see, and
  it is the entire premise of a cross-TU line-dedup transport. On the raw-source side
  the same window buys almost nothing — 1.00x for ten of the sixteen, 1.13x at the very
  most (simdjson, which checks in generated amalgamations) — because ordinary source
  has no cross-file duplication at that scale. That gap between the two columns *is*
  the opportunity.

### LLVM footnote: monorepo vs `llvm/` only

`corpus` is the one row where the source column is badly mismatched to the `.ii`
column. The checkout is the whole llvm-project monorepo — clang, mlir, libcxx, lldb,
all the test inputs — but only `llvm/` for the X86 target was preprocessed. Restricting
the source set to the `llvm/` subtree that was actually compiled:

| basis | files | src MiB | z19 src MiB | z19long src MiB | z19long ii/src |
|---|---|---|---|---|---|
| whole monorepo (table above) | 71,983 | 747.5 | 72.9 | 70.7 | 0.10 |
| `llvm/` subtree only | 8,127 | 151.9 | 22.9 | 22.3 | 0.33 |

The `llvm/`-only basis is the honest one for this corpus, and it is the row to quote:
`z19long ii/src = 0.33, not 0.10`. Even so, LLVM stays the least redundant corpus in the set by this
measure: 1,238 TUs of genuinely different compiler source, not 1,238 re-expansions of the
same headers. The other 15 corpora do not have this problem — their checkouts are the
project that was preprocessed.

## Snapshots

| corpus | snapshot | bytes | MiB | vs z19long .ii |
|---|---|---|---|---|
| corpus | corpus.tar.zst | 7,669,786 | 7.3 | 1.01 |
| corpus2 | corpus2.tar.zst | 6,348,317 | 6.1 | 1.00 |
| corpus3 | corpus3.tar.zst | 7,194,713 | 6.9 | 1.00 |
| corpus4 | corpus4.tar.zst | 4,149,654 | 4.0 | 1.01 |
| corpus5 | corpus5.tar.zst | 6,670,767 | 6.4 | 1.01 |
| corpus6 | corpus6.tar.zst | 58,467,532 | 55.8 | 1.00 |
| corpus7 | corpus7.tar.zst | 742,847 | 0.7 | 1.00 |
| corpus8 | corpus8.tar.zst | 474,598 | 0.5 | 1.01 |
| corpus9 | corpus9.tar.zst | 897,463 | 0.9 | 1.06 |
| corpus10 | corpus10.tar.zst | 816,815 | 0.8 | 1.00 |
| corpus11 | corpus11.tar.zst | 777,347 | 0.7 | 1.01 |
| corpus12 | corpus12.tar.zst | 1,412,952 | 1.3 | 1.01 |
| corpus13 | corpus13.tar.zst | 433,325 | 0.4 | 1.01 |
| corpus14 | corpus14.tar.zst | 557,686 | 0.5 | 1.01 |
| corpus15 | corpus15.tar.zst | 1,044,085 | 1.0 | 1.01 |
| corpus16 | corpus16.tar.zst | 495,266 | 0.5 | 1.01 |
| **TOTAL** | | 98,153,153 | 93.6 | 1.00 |

Each snapshot is `tar` of that corpus's `.ii` files (relative paths) plus
`manifest.txt` and `METADATA.json`, compressed with `zstd -19 --long=31 -T0`.
Snapshot bytes run slightly above `z19long .ii` because of tar's 512-byte
per-member headers and padding (thousands of members per corpus) plus the extra
metadata files. Checksums: `snapshots/SHA256SUMS`.

**Unpacking requires `--long=31`.** zstd refuses a 2 GiB decompression window unless
asked, so a plain `zstd -d` fails with *"Frame requires too much memory for decoding"*.
Use:

```bash
zstd -dc --long=31 snapshots/corpusN.tar.zst | tar -x -C <dest>
```

The `METADATA.json` *inside* an archive is the copy that existed when the archive was
built, so it has no `snapshot_bytes`/`snapshot_sha256` keys — an archive cannot carry
its own size. The live `corpusN/METADATA.json` does.

## Caveats

* **corpus (LLVM)** — the source column covers the *entire* llvm-project monorepo
  (clang, mlir, libcxx, all of `llvm/test/`, ...), because that is what the checkout
  contains, while only `llvm/` for the X86 target was preprocessed. Its `ii/src` and
  `z19long ii/src` ratios are therefore pessimistic by a large factor; see the LLVM
  footnote above for `llvm/`-subtree-only numbers. The **TOTAL** row inherits the same
  distortion — its 0.58 would be ~0.75 on the `llvm/`-only basis — so read the per-corpus
  rows, not the total, when the source column matters.
* **corpus (LLVM)** TU count comes from a *time-bounded* build harvest
  (`-save-temps=obj`, 720 s at -j12), not from a full replay of a compile database,
  so it is only approximately reproducible.
* **corpus12 (Eigen)** is capped at 650 of 1542 available TUs; Eigen's ~5.4 MB per-TU
  expansion would otherwise make it ~8 GB on its own.
* **corpus15 (simdjson)** is 153 of 156 TUs: 3 developer-mode targets fail to
  preprocess and are dropped.
* **corpus4** is two projects (abseil + protobuf) sharing one manifest; its source
  column sums both checkouts, and `git_url`/`git_commit` carry both.
* Compressed sizes come from multi-threaded zstd (`-T0`, 12 workers). MT output can
  differ by a fraction of a percent from single-threaded output at the same level;
  every number here was produced the same way, so they are comparable to each other.

## Regenerating

`./regenerate_corpuses.sh [corpusN ...]` rebuilds corpora from scratch
(clone -> configure -> `-E` replay -> manifest -> TU verify); `./snapshot_corpuses.sh`
rebuilds the archives; `collect_cheap.sh` + `collect_zstd.sh` + `build_metadata.py`
rebuild this table. See `README.md`.
