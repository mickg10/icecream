# Binding the causal GRZ2 / P29+BSC selector on fixed-16

Measured on `ttuser@tt-quietbox2` (AMD EPYC 8124P, 16 physical cores, 32 threads,
503 GiB RAM), 2026-08-18. Every encode and decode in this document was re-run from
scratch; nothing is carried over from the earlier ledgers.

## Answer in one line

The causal, rate-legal selector reaches **389.18x raw** with the frozen codec policies,
against the **402.01x** hindsight minimum. It is **10.8x short of the 400x target**, and
the entire 2,342,086-byte gap is corpora where GRZ2 is smaller but misses its own encode
deadline. Adding a second, faster GRZ2 candidate recovers most of it (**400.03x**, a
5,264-byte margin) but only on the codec-internal rate basis and only by moving six small
libraries onto a decoder that runs below the 500 MB/s floor.

Separately, and more important than any of the above: **P29+BSC does not hold the 1 GB/s
C floor on any of the 16 corpora once its interning pass is charged to the encoder.**
Since P29+BSC is the always-on baseline, that is the floor the whole scheme rests on.

## What reproduced exactly

Both size ledgers reproduce byte-for-byte, independently re-encoded:

- **GRZ2** (`grz2g`, fixed-112 policy `-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 -j 8
  --gtu 112 --graw 512 --gadd 128 --hist 1024`): all 16 `out` byte counts and all 16
  wire SHA-256s match `g2-fixed112-fixed16.tsv`. Wires are also identical between
  `-j 8` and `-j 1`, so GRZ2's thread count does not change its output.
- **P29+BSC** (`codec50-bsc-znver3`, 112-TU literal groups, BSC): all 16
  `complete_wire_bytes` match `p29-bsc-group-fixed16.tsv`, `byte-exact=OK` on all 16.
- **Hindsight minimum**: 71,028,967 B = 0.7409x whole-program z19 = **402.01x raw**,
  exactly local-oracle's number. The Godot+Eigen construction reproduces at 400.01x
  with a 1,981-byte margin.

Round-trip: GRZ2's wire is decoded by a separate process invocation and `cmp`-ed against
the input -- exact on 16/16. P29+BSC's complete wire is never materialised as one file;
its exactness is the codec's own in-process encode/decode/verify. That asymmetry is worth
closing before either is called deployable.

## The rate accounting, and why it decides everything

Both codecs were measured on a **warm page cache**, so neither pays for the NVMe read of
its own input. Decode is single-thread for both.

GRZ2 encodes in one process; its timer starts before `mmap` and ends after the wire is
written, so its reported rate is the whole encoder.

P29+BSC's codec prints a `split (2-proc per-stream proxy)` C rate. That proxy **excludes
the `loaded+interned` phase** -- `load_corpus()` plus `Interner::process()`, the line
interning that produces the region ids the encoder then consumes (`codec50.cpp:875`; the
`pass 0` timer starts after it). Interning is encoder work: without it there is nothing
to encode. So the table reports both bases.

| basis | what it counts | rocksdb |
|---|---|---:|
| `P29BSC_Cgbps_codec` | published proxy, no interning charge | 3.536 GB/s |
| `P29BSC_Cgbps_full` | whole process minus the decode share | 0.647 GB/s |
| `P29BSC_Cgbps_full_noread` | also forgiving the in-memory corpus copy | 0.691 GB/s |
| `P29BSC_intern_gbps` | the interner alone | 1.038 GB/s |

The interner is a serial loop over TUs, so its own throughput (0.49-1.31 GB/s, median
~1.05 on the >=100 TU corpora) is a hard ceiling on P29+BSC's C side no matter how many
cores the rest of the encoder gets.

### P29+BSC 1 GB/s floor: 16/16 miss

| corpus | full C | full C, read forgiven | published proxy |
|---|---:|---:|---:|
| llvm | 0.742 | 0.813 | 4.056 |
| rocksdb | 0.647 | 0.691 | 3.536 |
| duckdb | 0.524 | 0.556 | 1.809 |
| abseil | 0.701 | 0.765 | 4.323 |
| opencv | 0.824 | 0.928 | 6.611 |
| godot | 0.417 | 0.439 | 0.988 |
| catch2 | 0.735 | 0.832 | 5.698 |
| range-v3 | 0.694 | 0.752 | 4.375 |
| eigen | 0.937 | **1.027** | 15.124 |
| simdjson | 0.556 | 0.591 | 2.577 |
| fmt / spdlog / json / re2 / leveldb / cereal | 0.307-0.617 | 0.322-0.668 | 0.995-3.407 |

Even under the most generous reading available to it -- warm cache, corpus copy
forgiven, all 16 cores -- P29+BSC clears 1 GB/s on **1 of 16** corpora (Eigen, 1.027).
On the published proxy basis it clears 14/16, missing only fmt (0.995) and Godot (0.988).

The one architectural defence of the published number is that interning could be
overlapped with preprocessing, since `cpp` is far slower than 1 GB/s. That is a real
argument, but it re-frames the gate for GRZ2 identically, and it is an owner call, not
a measurement. As measured, all C-side CPU work counts.

## The causal selector

Rule as specified: P29+BSC is the always-on, rate-safe baseline and always ships; GRZ2 is
kept only when its own encode beat the `raw/1e9` deadline (a straggler is killed at the
deadline, so the decision is causal); selected = smaller of the legal candidates.

| selector | bytes | / z19 | x raw | vs 400x |
|---|---:|---:|---:|---|
| GRZ2 only, all 16 | 90,616,449 | 0.9452 | 315.12 | |
| P29+BSC only, all 16 | 78,568,007 | 0.8195 | 363.44 | |
| **hindsight minimum** | 71,028,967 | 0.7409 | **402.01** | clears |
| **causal, frozen policies, C gate** | **73,371,053** | **0.7653** | **389.18** | **short 1,984,374 B** |
| causal, frozen, C and F>=500 MB/s | 73,516,663 | 0.7668 | 388.41 | short 2,129,984 B |
| causal + tuned GRZ2, C gate, codec timer median | 71,381,415 | 0.7445 | **400.03** | clears by 5,264 B |
| causal + tuned GRZ2, C gate, min-of-reps / wall clock | 71,624,963 / 71,619,235 | 0.747 | 398.67 / 398.70 | short ~235 kB |
| causal + tuned GRZ2, C and F gates | 72,374,125 | 0.7549 | 394.54 | short 987,446 B |

Restricted to the 10 corpora with >=100 TUs: causal 69,472,902 B = 0.7518x z19 =
**395.05x**, hindsight 67,872,622 B = 0.7344x z19 = 404.36x.

Frozen-policy win count: **GRZ2 6, P29+BSC 10.** GRZ2 wins rocksdb, abseil, opencv,
catch2, range-v3, cereal. P29+BSC wins llvm, godot, eigen on size, and takes the other
seven only because GRZ2 was rate-illegal there.

### Where the causal selector loses ground

Every byte of the 2,342,086-byte gap to hindsight is one of these seven -- GRZ2 was
smaller but did not meet its deadline, so the selector was forced onto P29+BSC:

| corpus | GRZ2 C (wall / codec) | cost of being forced to P29+BSC |
|---|---:|---:|
| duckdb | 0.807 / 0.838 | +1,297,448 B |
| simdjson | 0.918 / 0.974 | +302,832 B |
| fmt | 0.426 / 0.446 | +269,961 B |
| nlohmann-json | 0.773 / 0.815 | +266,299 B |
| leveldb | 0.553 / 0.582 | +128,824 B |
| spdlog | 0.518 / 0.534 | +41,806 B |
| re2 | 0.580 / 0.630 | +34,916 B |

DuckDB is arithmetically mandatory: without it, the other six together are worth only
1,044,638 B, less than the 1,984,374 B needed to reach 400x.

## Secondary: can GRZ2's encode be pushed over the deadline?

Yes, on all seven, by trading a little size for parallelism and lower fixed cost. Every
variant below was decoded and `cmp`-ed byte-for-byte against its input: **exact on all**.
The lever that matters is the literal block size (`-b`), which controls how much of the
entropy stage can be spread across threads; for the small libraries the 16 MiB index
(`-t 21`) and 1 GiB history ring (`--hist 1024`) are also material fixed costs.

Size-keyed second candidate:

- `raw > 256 MB`: `-b 2 -j 16`
- `raw <= 256 MB`: `-b 1 -t 16 -s 8 --hist 32 -j 16`

| corpus | frozen bytes | tuned bytes | size cost | C median (codec / wall) | F 1T |
|---|---:|---:|---:|---:|---:|
| duckdb | 6,576,037 | 6,730,947 | +154,910 | 1.204 / 1.155 | 0.719 |
| simdjson | 955,208 | 993,865 | +38,657 | 1.435 / 1.301 | 0.344 |
| nlohmann-json | 743,198 | 780,245 | +37,047 | 1.207 / 1.131 | 0.197 |
| leveldb | 489,989 | 517,256 | +27,267 | 1.170 / 1.107 | 0.205 |
| re2 | 401,210 | 421,830 | +20,620 | 1.091 / 1.002 | 0.167 |
| fmt | 662,261 | 712,090 | +49,829 | 1.068 / 0.974 | 0.194 |
| spdlog | 436,106 | 460,224 | +24,118 | 1.074 / 0.984 | 0.149 |

Every one of these is still far smaller than the P29+BSC wire it replaces, so all seven
flip, and the causal total lands at **71,381,415 B = 400.03x** -- clearing 400x by
**5,264 bytes**, a 0.007% margin.

Three qualifications, all of which the owner needs before that number is used:

1. **The margin is knife-edge and basis-dependent.** On the process wall clock (which
   adds ~10-20 ms of process startup, immaterial for the big corpora and decisive for the
   <150 MB ones) fmt and spdlog measure 0.974 and 0.984 GB/s and drop out, leaving
   **398.70x**. The min-of-reps codec basis gives 398.67x. Only the median-of-reps
   codec-internal timer -- the same basis `g2-fixed112-fixed16.tsv` gates on -- clears.
2. **Six of the seven flips break the decode floor.** The tuned wires decode at
   0.149-0.344 GB/s single-thread, well under 500 MB/s. Only DuckDB (0.719) clears.
   Requiring C >= 1 GB/s **and** F >= 500 MB/s on the selected codec puts the causal
   selector at **394.54x**. (The frozen GRZ2 already failed F on those same libraries;
   the tuning improved F, it did not create the problem.)
3. **The tuned rates assume a whole 16-core box per codec.** At one thread the same
   tuned policy gives 0.675 (duckdb), 0.918 (simdjson), 0.754 (json) -- all illegal. The
   "C-side runs both codecs" model needs enough cores that each concurrent candidate
   still meets its own deadline; measured alone, each candidate had all 16.

## The gate as a whole, not just the C rule

The selected codec fails F >= 500 MB/s on three corpora under the frozen policies:
Godot (P29+BSC, 0.436), range-v3 (GRZ2, 0.427), cereal (GRZ2, 0.246). **Godot has no
admissible codec at all**: P29+BSC decodes at 0.436 and GRZ2 encodes at 0.349. Godot is
simultaneously the corpus that makes the size result work -- P29+BSC saves 18,333,398 B
there, more than the entire distance between the GRZ2-only and hindsight totals -- and
the corpus where both codecs are furthest from the rate gate.

## Reproducing

- `selector_cell.sh <corpus>` -- one corpus, both codecs, both thread configurations,
  warm cache, 3 reps each, byte-exact round trip. Evidence under `~/selbind/runs/<corpus>`
  on quietbox2.
- `selector_summarize.py` -> `selector-binding-fixed16.tsv` (per-corpus rows) and
  `selector-binding-fixed16.notes` (aggregates and every gate miss).
- `selector_tune1.sh`, `selector_tune2.sh` -> `selector-grz2-tune-round{1,2,3}.tsv`;
  the chosen second candidate is `selector-tuned-grz2.tsv`.
- References: `wp_z19` is `zstd -19 --long=31 -T0` over the whole ordered `.ii`, taken
  from `corpora.tsv` (built by `prep.sh`, round-trip-verified by `baseref.sh`); sizes via
  `stat -Lc %s` on the real concatenated file, not a symlink.

Caveat on the measurements: a sibling job was active on the same 16 cores throughout.
Rate spread across reps was under 3% on the corpora above 900 MB and up to 10.5% on
spdlog (0.4 s wall); see `C_spread_pct`. The tune rounds ran under load average 4-7, so
their rates are, if anything, pessimistic.
