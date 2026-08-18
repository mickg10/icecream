# Per-corpus panels: codec (colour) x docker env (dash), 4 passes

One file per **(corpus, docker env)**, since the panel is per corpus with colour = codec and
dash = env. Each project is run **4 times back-to-back** — cold pass 1, warm rebuilds 2-4 —
off one shared 4x manifest, so every codec sees literally the same TU sequence.

```
tu  pass  project  env  raw  cumulative_raw
p29_cum_wire         P29+BSC line-interning (codec50, batch basis)
grz2_cum_wire        GRZ2, binding --gtu 112; step function, forward-filled
fastintern_cum_wire  ultra-fast interner (production-fused, dedup + z3)
zstd3_cum_wire       zstd-3 per TU, independent -- the no-cross-TU-memory baseline
pred_cum_wire        online prediction (empty-online-k2, cold, no seed)
grz2_group_closes    1 where a GRZ2 group closed on this TU
```

## Lead batch: 3 projects x 4 envs x 5 codecs x 4 passes

Total cumulative wire at the end of pass 4:

| project | env | P29+BSC | GRZ2 | fast interner | zstd-3/TU | prediction |
|---|---|---:|---:|---:|---:|---:|
| re2 | debian-gcc | 458,487 | 364,291 | 17,144,273 | 53,009,204 | 530,457 |
| re2 | fedora-clang-libcxx | 506,829 | 398,482 | 26,479,246 | 92,332,764 | 2,174,715 |
| re2 | linuxbrew | 439,415 | 357,822 | 16,974,236 | 53,103,908 | 479,428 |
| re2 | conan-gcc | 467,706 | 369,978 | 17,587,213 | 54,456,092 | 595,454 |
| fmt | debian-gcc | 1,009,483 | 667,109 | 24,720,348 | 81,274,928 | 2,105,829 |
| fmt | fedora-clang-libcxx | 786,478 | 602,443 | 25,335,217 | 91,344,900 | 1,987,491 |
| fmt | linuxbrew | 764,113 | 596,557 | 24,026,535 | 80,690,796 | 605,369 |
| fmt | conan-gcc | 1,023,335 | 677,634 | 25,207,560 | 82,868,976 | 2,128,917 |
| leveldb | debian-gcc | 919,480 | 615,248 | 33,323,594 | 107,494,448 | 2,260,375 |
| leveldb | fedora-clang-libcxx | 953,229 | 624,283 | 39,721,489 | 141,131,588 | 3,496,813 |
| leveldb | linuxbrew | 792,905 | 578,380 | 32,921,374 | 107,381,768 | 1,539,131 |
| leveldb | conan-gcc | 932,599 | 624,216 | 33,955,781 | 109,826,688 | 2,099,375 |

The env spread is real and worth the dash dimension: on re2 the fedora-clang-libcxx build
costs **1.74x** the linuxbrew build in raw zstd-3 terms (92.3 MB vs 53.1 MB) and **4.5x** in
prediction wire, because libc++ preprocesses to different — and more — text. The structural
codecs absorb most of that (P29+BSC varies only 1.15x across the four envs), which is itself
the point: the dash spread narrows as the codec gets more structural.

## Env coverage (`env-coverage.tsv`)

**11 projects have complete 4-env cells** — `corpus.json` present, payload sha256 verified,
TU count matching: catch2, cereal, eigen, fmt, leveldb, nlohmann-json, opencv, range-v3,
re2, rocksdb, spdlog. That is the verified 44-cell matrix.

**24 projects have a single `native-gcc11` env** (`~/grouprlz/corpora.tsv`), including the
giants — llvm, llvm-full and firefox. Several names appear in both sets; those are genuinely
different builds, so for those projects `native-gcc11` is effectively a **fifth** env rather
than a duplicate.

So 11 four-env panels + 24 single-env panels = **35 panels**, clearing the 30 target with
firefox included.

**Not usable, and why** — these are the older cell generation, carrying
`<project>-<profile>.ii.tar.zst` plus `raw-manifest.txt` and **no `corpus.json`**:

- **abseil** — 4 profile dirs, old-style package only. Usable only by trusting the
  superseded package's own internal manifest, with no sha or TU-count binding to check it
  against. Excluded rather than silently trusted.
- **draco, magnum, taglib** — 1 env (debian-gcc), same old-style packaging.
- **simdjson** — profile dirs exist but are **empty**.

This matters because a cell can hold two archives and the superseded one carries its own
manifest, so globbing `*.ii.tar.zst` fails silently with the wrong TU set. Every cell here
is resolved from `corpus.json` -> `payload.path` with its sha256 verified.

## Gates

Per cell, all asserted before the join is written:

- payload sha256 verified against `corpus.json`; extracted TU count checked against
  `tu_count`.
- `zstd3` self-check: per-TU sums equal independently accumulated totals (`sum_ok=1`).
- `fastintern`: per-TU byte-exact reconstruction, `verify=PASS`.
- `p29`: codec50's curve endpoint equals the run's `TOTAL=` — **YES on all 12 cells**.
- `grz2`: group sums plus stream framing equal the reported wire; every group boundary
  asserted against codec50's `cumulative_raw`.
- axis: every codec's per-TU `raw` must match codec50's, TU for TU; a disagreeing source is
  dropped to `NA` rather than misaligned. No cell required a drop.

## Codec caveats

- **GRZ2 is a step function** (groups close every 112 TUs); `grz2_group_closes` marks the
  real points, and `NA` runs until the first group closes — on these small projects that is
  inside pass 2, so pass 1 is `NA`.
- **The payload is `zstd-3 --long=31`**, so extraction needs the long window explicitly;
  plain `tar --zstd` fails with "Window size larger than maximum".
- **The prediction line is the expensive one**: single-threaded Python at roughly 30 MB/s
  against seconds per cell for the other four. It handles 4 passes and full corpora
  (`--max-tus` is unbounded; the earlier 200-TU curves were a `--max-tus 200` choice), and
  reports `exact: true` on every cell here. Its cost is what will bound how many of the 35
  panels carry it; any cell without it shows `NA` and is named, never quietly dropped.
