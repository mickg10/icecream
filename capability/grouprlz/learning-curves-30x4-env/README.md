# Per-corpus panels: codec (colour) x docker env (dash), 4 passes

> **PROVISIONAL — superseded grouping.** These cells were produced by the single-4x-stream
> harness, in which a GRZ2 group can span the cold build into the first warm rebuild, so the
> per-build boundaries here are **not independently closed** (only 48 of 176 pass endpoints
> were GRZ2 close points). The 4x endpoints are correct and unchanged, but the intermediate
> builds are not. Regenerate with `selector_passcell.sh`, whose prefix method closes and
> independently decodes every build boundary — see `../pass-closure-proof/`.

One file per **(corpus, docker env)**, since the panel is per corpus with colour = codec and
dash = env. Each project is run **4 times back-to-back** — cold pass 1, warm rebuilds 2-4 —
off one shared 4x manifest, so every codec sees literally the same TU sequence.

```
tu  pass  project  env  raw  cumulative_raw
p29_cum_wire         P29+BSC line-interning (codec50, batch basis)
grz2_cum_wire        GRZ2, binding --gtu 112; step function, forward-filled
fastintern_cum_wire  ultra-fast interner (production-fused, dedup + z3)
zstd3_cum_wire       zstd-3 per TU, independent -- the no-cross-TU-memory baseline
pred_cum_wire        Region-sequence ORACLE (empty-online-k2, cold, no seed)
                     -- NOT a full-.ii wire; see the note below
grz2_group_closes    1 where a GRZ2 group closed on this TU
```

> **Prediction line — read this before comparing totals.** `pred_cum_wire` is a
> **Region-sequence oracle** (`empty-online-k2`, cold, no pre-shared seed), **not a
> full-`.ii` wire** like the other four columns. It predicts over the Region stream rather
> than encoding the complete preprocessed input, so **its totals are NOT comparable to the
> P29+BSC / GRZ2 / fast-interner / zstd-3 totals — only the SHAPE of its curve is.** Any
> ratio taken against it is meaningless. This matches how the dashboard labels it.

## Coverage: 11 projects x 4 envs = 44 cells

All 11 verified four-profile projects are complete: re2, fmt, leveldb, spdlog, cereal,
nlohmann-json, range-v3, catch2, rocksdb, opencv, eigen. Every cell passes its gates
(payload sha verified, TU count matched, `sum_ok=1`, `verify=PASS`, **codec50 curve endpoint
== `TOTAL=` on all 44**, GRZ2 boundaries asserted against `cumulative_raw`, per-TU raw
asserted TU-for-TU).

Note some projects differ in TU count BETWEEN profiles — opencv is 1500/1500/1485/1500 and
range-v3 is 537/575/575/537 — because the profiles genuinely compile different file sets.
Each cell is joined independently, so this is handled, but it means the dashes within one
panel do not all end at the same x.

## ENV SENSITIVITY IS PER CODEC, AND IT IS NOT MONOTONE IN "HOW STRUCTURAL"

`env-spread.tsv`, max/min across the four profiles per project, at 4-pass totals:

| project | raw | P29+BSC | GRZ2 | fast interner | zstd-3/TU |
|---|---:|---:|---:|---:|---:|
| catch2 | 3.05x | 1.21x | **2.55x** | 1.90x | 2.12x |
| cereal | 1.37x | 1.07x | 1.02x | 1.05x | 1.04x |
| eigen | 1.25x | 1.03x | 1.21x | 1.05x | 1.06x |
| fmt | 1.50x | 1.34x | 1.14x | 1.05x | 1.13x |
| leveldb | 1.84x | 1.20x | 1.08x | 1.21x | 1.31x |
| nlohmann-json | 1.48x | 1.27x | **1.93x** | 1.07x | 1.14x |
| opencv | 1.49x | 1.12x | 1.10x | 1.11x | 1.18x |
| range-v3 | 2.31x | 1.11x | **2.68x** | 1.58x | 1.69x |
| re2 | 2.57x | 1.15x | 1.11x | 1.56x | 1.74x |
| rocksdb | 1.33x | 1.12x | 1.10x | 1.19x | 1.15x |
| spdlog | 2.10x | 1.11x | **1.89x** | 1.39x | 1.49x |
| **MEDIAN** | **1.50x** | **1.12x** | **1.14x** | **1.19x** | **1.18x** |

**Correction to the three-project reading.** On the lead batch it looked like the dash
spread simply narrows as a codec gets more structural. With all 11 projects that is **true
for P29+BSC and false as a general rule.**

- The **raw input itself** varies 1.25x-3.05x across profiles (median 1.50x). The docker
  profile really does change how much preprocessed text exists.
- **P29+BSC is the env-stable one**: median 1.12x, worst case 1.34x. It absorbs the profile
  difference on every project.
- **GRZ2 has the same median (1.14x) but is bimodal**: usually flat, then 1.89x-2.68x on
  catch2, range-v3, nlohmann-json and spdlog. On range-v3 its spread (2.68x) **exceeds the
  raw spread (2.31x)** — it amplifies the profile difference rather than absorbing it.

So the panel should not be read as "structural codecs have tight dashes". P29+BSC has tight
dashes; GRZ2's dashes fan out on exactly the projects where long-range matching is
profile-sensitive. That contrast between the two structural codecs is the more interesting
story, and it is only visible because env is its own axis.

## Lead batch detail: 3 projects x 4 envs x 5 codecs x 4 passes

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
prediction wire, because libc++ preprocesses to different — and more — text, while P29+BSC
varies only 1.15x. See the env-sensitivity table above for why that does **not** generalise
to "structural codecs absorb env": GRZ2 is structural and fans out to 2.68x on range-v3.

## Env coverage (`env-coverage.tsv`)

**11 projects have complete 4-env cells, and all 44 are now measured** — `corpus.json` present, payload sha256 verified,
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
