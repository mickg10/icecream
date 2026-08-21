# Policy-B corrected-P29 replacement rows: verified-44 docker family

Date: 2026-08-18 UTC. Host `tt-quietbox2`.

**This is a measurement ledger, not a fitted selector.** No selector is fitted, no
selected total is claimed, and `selected_at_probe` below is only the argmin of the two
TU112 probe censuses, recorded as an observation.

## Contract as implemented

- **Corpus**: the 44 verified `ice-ii-corpus-v1` cells, 11 projects x 4 profiles, from the
  **current** `ii.tar.zst` payload named by each cell's `corpus.json`. 74.54 GB raw,
  21,657 TUs. Each cell's TU count was re-verified against `corpus.json` `tu_count`
  before measuring.
- **Decision point**: `probe_tus = min(112, total_tus)`. Both candidates advance only
  through it. P29 is **never** truncated to GRZ2's group-close TU; GRZ2's group-1 close
  TU, close reason and ADD bytes are recorded as separate features.
- **P29**: rebuilt from local-oracle commit `56c1744`, `--stable-root-tags`
  (with `--direct-ordinals`); the suffix-blind probe adds `--open-final-entropy`.
- **Identity gate**: local-oracle's `run_p29_prefix_identity.sh` +
  `verify_p29_prefix_identity.py`, requiring byte-identical per-TU curve, component
  curve, raw-plane stream prefixes and literal frames through the probe.
- **GRZ2**: deployed demand-ring anchor-census binary only. Complete GRZ2 wires,
  exactness, decode rates and whole-program zstd references are **reused** from
  local-oracle's retained `matrix44-20260818T0230Z` replay, not recomputed.
- **Rate**: one fixed total core budget of 16, split **disjoint 8 P29 (cores 16-23) /
  8 GRZ (cores 24-31)**, both bounded probes launched together. No two independently
  measured full-core rates are ever added. No stage time is derived by subtracting an
  inferred decode time from a wall clock.

## Result

**Identity: 44/44 PASS. Corrected complete P29 byte-exact: 44/44. GRZ complete rows
reused: 44/44, all `exact=true`.**

| profile | cells | raw | corrected P29 complete | / z19 | probe P29 | probe GRZ2 | GRZ/P29 @ probe | argmin @ probe | GRZ g1 close |
|---|---:|---:|---:|---:|---:|---:|---:|:---:|---|
| debian-gcc | 11 | 16.37 GB | 18,122,190 | 1.1380 | 8,103,699 | 5,929,553 | 0.7317 | GRZ2 11/11 | tu 9, raw 2 |
| conan-gcc | 11 | 16.88 GB | 18,380,816 | 1.1364 | 8,366,730 | 6,111,000 | 0.7304 | GRZ2 11/11 | tu 9, raw 2 |
| linuxbrew | 11 | 16.80 GB | 16,896,087 | 1.0908 | 7,728,594 | 5,788,724 | 0.7490 | GRZ2 11/11 | tu 9, raw 2 |
| fedora-clang-libcxx | 11 | 24.50 GB | 17,770,811 | 1.0514 | 8,642,965 | 6,275,655 | 0.7261 | GRZ2 11/11 | tu 9, raw 2 |
| **ALL** | **44** | **74.54 GB** | **71,169,904** | **1.1036** | **32,841,988** | **24,104,932** | **0.7340** | **GRZ2 44/44** | **tu 36, raw 8** |

### The stable-Root correction is nearly size-neutral

Against local-oracle's legacy `00f83f8` rows over the same 44 cells:

```
legacy total     71,167,315
corrected total  71,169,904
delta                +2,589   (+0.0036%)
```

Largest gains fmt/conan-gcc -8,787 (-0.854%) and fmt/debian-gcc -4,347; largest
regressions leveldb/fedora +2,913 and spdlog/fedora +2,892. **The correction matters for
causality and prefix identity, not for size** -- which is the point: the legacy rows were
not wrong by much in bytes, they were wrong in what they were measuring.

### Probe observations

At TU112 the GRZ2 census is smaller than the P29 census on **44/44 cells**, ratio 0.726
to 0.749 and remarkably stable across profiles. GRZ2 closed group 1 early on **8 cells**
(all four eigen profiles at TU83-105 and four others), by the `raw` cap in every case;
on the other 36 it closed exactly at the boundary. Those 8 are precisely the rows the
legacy runner compared over unequal extents.

### Fixed-budget rate

| profile | makespan sum | core-seconds / probe GiB |
|---|---:|---:|
| debian-gcc | 7.83 s | 4.54 |
| conan-gcc | 8.18 s | 4.53 |
| linuxbrew | 8.03 s | 4.51 |
| fedora-clang-libcxx | 10.19 s | 3.70 |
| **ALL** | **34.22 s** | **4.24** |

## Honest labels

- **P29 used the research interner** (`load_corpus` + `Interner::process`, the
  file-open/re-intern pass), not the fast M5 path. Every P29 C-stage number here is
  **research-interner and is not the product limit.** M5 integration remains a separate,
  unstarted step and is not blessed by these rows.
- **One contract stage is still combined**: P29 material construction and serialization
  share a single accumulator (`enc_s`, codec50.cpp:1104-1653) with no existing bracket
  between them. Splitting them requires instrumenting 56c1744. Every other stage --
  producer extract/concat/TU-map, P29 intern, P29 structure plan, P29 literal entropy,
  GRZ match, GRZ entropy, GRZ total -- is a direct clock, per row.
- **Decode rates are local-oracle's**, measured decoding to a file. On the same wire,
  decoding to `/dev/null` measures roughly 1.7x faster, so these are the conservative
  figure. 25 of 44 clear 500 MB/s on the to-file basis.
- The 1.10x whole-program-z19 size gate is **not** met by corrected P29 alone
  (1.1036x overall; only fedora-clang-libcxx at 1.0514 and linuxbrew at 1.0908 are
  inside it). That is a statement about P29, not about any selector.

## Families

This document covers the **verified-v2-docker** family only. The **fixed-16** and
**native-25** replays under this contract are not yet run.

## Pins (identical on all 44 rows)

```
P29 source  89ed3b299332f90898b0bd86338052d67d686e93797d2e332080f7ed33c7c753  (= 56c1744:linecache/codec50.cpp)
P29 binary  3967d6ca12b034c0752485a81f3c0dd64f2196c52d1ab32b18f82fadf678418e
GRZ source  e997b612c3445c951fa8bfc4abd2942fbad532fac85ebae5aa5fe753fe64a6b9
GRZ binary  647883b7a346ae48d76ea2ac542240e5c78b4a56049372ee6067f5ec94276af9
gate        gate-anchor-census-retained-20260818T0230Z (12/12)
libbsc      baffa62c70b6ebbecc9af14ce550e965ea247680
zstd        v1.4.8
```

P29 was rebuilt independently rather than reusing local-oracle's binary:

```
g++ -O3 -march=znver3 -std=c++23 -DWITH_BSC_GROUPS -I /home/ttuser/libbsc/libbsc \
    -o codec50-56c1744 codec50.cpp /home/ttuser/grouprlz/libbsc.a -lzstd -lz -lpthread
```

On catch2/debian-gcc this build reproduces local-oracle's `codec50-stable-root-final-l23`
(`6cb0d9db...`, different flags, 2.2x the file size) byte-for-byte: identical `curve.tsv`
`8453551d...`, `components.tsv` `31f280cb...`, `literal.wire` `9e0e9f9f...`, same
TOTAL=980,640, both `byte-exact=OK`.

## Reproducing

- `selector_pb_cell.sh <project> <profile>` -- one cell end to end.
- `selector_pb_sweep.sh <project>...` -- all four profiles per project.
- `selector_pb_summarize.py` -> `selector-binding-dockermatrix-policyB.tsv` (40 columns
  per cell) + `.notes`.

Per-cell evidence under `/home/ttuser/selbind/pb/<project>-<profile>/`, including the
full identity run (`id/`), both probe censuses, the GRZ group curve with the anchor
census, and all stage clocks.

Contention note: local-oracle's own 44-cell replay was running on cores 0-15 throughout,
which are the SMT siblings of the physical cores under 16-31. Load average 3.4-4.8. The
rate figures are therefore conservative; sizes and identity are unaffected.

---

# SIZE-CENSUS (size-only, NOT rate-bound)

Per-cell `selected = min(corrected P29+BSC complete, GRZ2 complete)`. **No rate gate is
applied and no selector is fitted.** This is a size observation over the 44 verified-v2
docker cells; the rate-binding resource run is separate and gated.

| profile | cells | raw | selected | **x = raw/selected** | / z19 | GRZ2 wins | P29+BSC wins |
|---|---:|---:|---:|---:|---:|:---:|:---:|
| debian-gcc | 11 | 16.37 GB | 15,605,707 | **1048.94** | 0.9800 | 9 | 2 |
| conan-gcc | 11 | 16.88 GB | 15,807,931 | **1067.52** | 0.9773 | 9 | 2 |
| linuxbrew | 11 | 16.80 GB | 15,010,556 | **1119.09** | 0.9691 | 9 | 2 |
| fedora-clang-libcxx | 11 | 24.50 GB | 16,163,459 | **1515.55** | 0.9563 | 7 | 4 |
| **ALL** | **44** | **74.54 GB** | **62,587,653** | **1190.96** | **0.9705** | **34** | **10** |

Single-codec references on the same 44 cells:

| | x raw | / z19 |
|---|---:|---:|
| GRZ2 alone | 1044.03 | 1.1071 |
| corrected P29+BSC alone | 1047.34 | 1.1036 |
| whole-program `zstd -19 --long=31` alone | 1155.83 | 1.0000 |
| **per-cell min (this census)** | **1190.96** | **0.9705** |

The complementarity is the finding: **neither codec alone beats whole-program zstd-19**
(both about 1.10x z19), but the per-cell minimum does, at 0.9705x. The two codecs win on
different cells -- 34/10 overall -- and fedora-clang-libcxx shifts hardest toward P29+BSC
(7/4 versus 9/2 on the three gcc-family profiles).

Because this is size-only, it is an upper bound on any rate-legal selector: applying a
C-rate deadline can only remove candidates, never add them.

## Corpus binding rule: verified on all 44

`selector_pb_provenance.py` re-derives, per cell, that the payload was resolved from
`corpus.json` `payload.path` (never globbed, never the older project-named package), that
its SHA-256 and byte count match `payload.sha256`/`payload.bytes`, that the `manifest.tsv`
used is the on-disk sibling of that declared payload, and that
`probe_tus == min(112, tu_count)` with the measured TU count equal to `tu_count`.

**44/44 cells: payload `ii.tar.zst`, SHA match True, counts_ok True. Zero violations.**
Sanity cells as requested: **re2 = 72 TUs** (active corpus, not the old package's 22) and
**rocksdb = 418** (not 367). Per-cell pins -- payload path, payload byte count,
`corpus.json` SHA-256 and `manifest.tsv` SHA-256 -- are in
`selector-policyB-provenance.tsv`.

---

# Policy-B classifier bracket (size-only, no rates)

Two corrections to how the census has been read:

- GRZ2 wins the **TU112 census** on 44/44, but wins the **complete** size on **34/44**.
  P29+BSC wins the complete size on 10. `selected != sum(GRZ2 complete)`.
- The **naive** policy-B rule -- pick whichever TU112 census is smaller, then ship that
  codec -- degenerates to GRZ2-always here, and is the **worst** of the three fixed
  policies: 1.1071x z19 versus 1.1036x for P29+BSC-always.

| policy | bytes | x raw | / z19 |
|---|---:|---:|---:|
| **ORACLE** per-cell min(complete) | 62,587,653 | 1190.96 | **0.9705** |
| whole-program `zstd -19 --long=31` | 64,489,971 | 1155.83 | 1.0000 |
| P29+BSC always | 71,169,904 | 1047.34 | 1.1036 |
| **NAIVE** smaller TU112 census (= GRZ2 always) | 71,395,383 | 1044.03 | **1.1071** |

**Bracket width = 8,807,730 B = 0.1366x z19.** That is the headroom a decision-point
classifier can recover.

## The 10 tail overtakes

GRZ2 is smaller at TU112 on all 44, so every P29+BSC complete win is a pure tail
overtake:

| project | profile | GRZ complete | P29 complete | margin | TU112 ratio | g1 close |
|---|---|---:|---:|---:|---:|---|
| range-v3 | fedora-clang-libcxx | 1,036,737 | 936,114 | -100,623 | 0.6945 | tu |
| opencv | fedora-clang-libcxx | 6,609,636 | 6,144,420 | -465,216 | 0.7319 | tu |
| eigen | fedora-clang-libcxx | 4,864,106 | 2,320,794 | -2,543,312 | 0.7639 | **raw** |
| eigen | conan-gcc | 4,329,139 | 2,379,861 | -1,949,278 | 0.7691 | **raw** |
| eigen | debian-gcc | 3,932,160 | 2,376,519 | -1,555,641 | 0.7711 | **raw** |
| eigen | linuxbrew | 3,874,615 | 2,309,461 | -1,565,154 | 0.7822 | **raw** |
| rocksdb | debian-gcc | 2,535,516 | 2,468,928 | -66,588 | 0.7959 | **raw** |
| rocksdb | conan-gcc | 2,679,877 | 2,555,041 | -124,836 | 0.7975 | **raw** |
| rocksdb | linuxbrew | 2,444,715 | 2,278,907 | -165,808 | 0.8193 | **raw** |
| rocksdb | fedora-clang-libcxx | 2,553,438 | 2,282,164 | -271,274 | 0.8391 | **raw** |

## The separating feature is the group-1 close reason, not the size ratio

The GRZ2 group-1 close reason -- already retained at the decision point -- is a perfect
one-sided separator on this matrix:

```
                  closed_by=raw   closed_by=tu
  P29+BSC wins          8              2
  GRZ2 wins             0             34
```

| rule (decision-point features only) | bytes | x raw | / z19 | TP | FP | FN | recovers |
|---|---:|---:|---:|:---:|:---:|:---:|---:|
| **A: g1 `closed_by == raw` -> P29+BSC** | 63,153,492 | 1180.29 | **0.9793** | 8 | **0** | 2 | **93.6%** |
| B: A or (fedora and ratio >= 0.69) | 63,061,295 | 1182.01 | 0.9778 | 10 | 4 | 0 | 94.6% |
| C: TU112 ratio >= 0.7639 | 67,495,680 | 1104.36 | 1.0466 | 7 | 12 | 3 | 44.3% |
| D: A or ratio >= 0.7639 | 64,952,368 | 1147.60 | 1.0072 | 8 | 12 | 2 | 73.2% |

**Rule A is a single binary feature, already observed, with zero false positives, and it
recovers 93.6% of the bracket** -- landing at 0.9793x z19, inside the goal and below plain
zstd-19. The TU112 size ratio, which was the obvious candidate, is much weaker (44.3%,
12 false positives): the P29-win and GRZ-win ratio ranges overlap almost completely
(0.6945-0.8391 versus 0.6137-0.8194). Rule B buys 1 further point of bracket for 4 false
positives and is not worth it.

Mechanistically this is plausible: an early `raw`-cap close means the cell has large
per-TU raw volume, so GRZ2's bounded history saturates and its long-range advantage
decays through the tail, while P29+BSC's structural model keeps paying.

## Do not over-read this yet

Held out **by project lineage**, as the contract requires, the positives are only **four
lineages**: eigen 4/4, rocksdb 4/4, opencv 1/4, range-v3 1/4. Rule A's 8 true positives
are **eigen and rocksdb only -- two lineages**. A 2-lineage, 44-cell result is a strong
hypothesis, not a validated classifier. The honest reading is: the close-reason feature
is the thing to test next on the fixed-16 and native-25 families, where the large corpora
(LLVM, Godot, Eigen) are exactly where the selection is expected to bite.

Everything above is **size-only**. Rate-legality is not applied and would only remove
candidates, so each row is an upper bound on its rate-bound counterpart.

---

# INTERIM rate binding (conservative two-pass P29 charge)

Measured isolated, box quiescent (load 1.2-1.7 throughout), fixed 16-core budget split
disjoint **8 P29 (cores 16-23) / 8 GRZ (cores 24-31)**. Every P29 C rate is a
**pessimistic floor**: both the raw-plane plan pass and the grouped encode are inside the
candidate clock, so a one-pass encoder can only be faster. Each candidate is charged its
own input conversion -- GRZ pays concat + TU map, P29 pays its own open + intern
in-process. Nothing is derived by subtracting a stage. Probe reps 3, complete-encode and
decode reps 2, **every rep retained in the TSV, none discarded**.

## Headline: the decode floor is met everywhere, the encode floor nowhere

| gate | cells clearing |
|---|---|
| F >= 500 MB/s, GRZ2 (real single-thread decode) | **44/44** |
| F >= 500 MB/s, P29+BSC (codec in-process proxy) | **44/44** |
| C >= 1 GB/s, GRZ2 (charged) | **0/44** |
| C >= 1 GB/s, P29+BSC (two-pass, charged) | **0/44** |
| full gate, either codec | **0/44** |

Distribution (median of reps): P29 C 0.128-0.444 (med 0.286) · GRZ C 0.269-0.589
(med 0.470) · GRZ F 0.531-2.059 (med 0.875) · P29 F proxy 0.653-11.308 (med 2.486).

## What each proposed fix actually buys

Aggregate over all 44 cells, 74.54 GB raw:

| configuration | C GB/s | gate |
|---|---:|---|
| P29 as measured (plan + grouped, research interner) | 0.370 | FAIL 0/44 |
| **P29 if one-pass** (grouped only, research interner) | **0.693** | **FAIL 0/44** |
| GRZ as measured (concat + TU map + encode) | 0.516 | FAIL 0/44 |
| **GRZ if one-producer fanout** (encode only) | **1.304** | **PASS 25/44** |

The plan pass is 46.6% of P29's candidate time; the concat is 60.4% of GRZ's.

This answers the open question empirically, and the two codecs answer it differently:

- **For GRZ2 the binding cost is the harness, not the codec.** Materializing a
  concatenated `cell.ii` on disk is a measurement artifact -- production holds the TU
  stream in memory. A one-producer fanout moves GRZ from 0.516 to **1.304 GB/s**, clearing
  the gate on 25/44 and on **15/15 of the cells at or above 1 GB raw**. The small cells
  that still miss are fixed-overhead-bound, as they have been throughout.
- **For P29+BSC one-pass is necessary but nowhere near sufficient.** Removing the plan
  pass entirely still leaves 0.693 GB/s and **0/44** cells clearing. P29 needs the
  one-pass refinement *and* fast M5 interning *and* more cores before this gate is in
  reach. Its C-stage numbers remain research-interner labelled.

## The tension the census exposed, now answered

On the 10 cells where P29+BSC wins the complete size -- the cells a selector actually
needs P29+BSC to be rate-legal on -- **P29 clears the gate on 0 of 10**:

| project | profile | P29 C | P29 F | GRZ C | P29 gate |
|---|---|---:|---:|---:|:---:|
| eigen | linuxbrew | 0.444 | 11.308 | 0.580 | FAIL |
| eigen | debian-gcc | 0.441 | 10.882 | 0.586 | FAIL |
| eigen | conan-gcc | 0.435 | 10.630 | 0.578 | FAIL |
| eigen | fedora-clang-libcxx | 0.429 | 10.643 | 0.589 | FAIL |
| range-v3 | fedora-clang-libcxx | 0.407 | 8.760 | 0.574 | FAIL |
| opencv | fedora-clang-libcxx | 0.367 | 6.702 | 0.499 | FAIL |
| rocksdb | fedora-clang-libcxx | 0.361 | 4.261 | 0.512 | FAIL |
| rocksdb | linuxbrew | 0.346 | 3.753 | 0.484 | FAIL |
| rocksdb | conan-gcc | 0.345 | 3.752 | 0.491 | FAIL |
| rocksdb | debian-gcc | 0.341 | 3.703 | 0.483 | FAIL |

These are the *fastest* P29 cells in the matrix -- they top the P29 C distribution -- and
they still sit at roughly a third of the deadline. Even a perfect one-pass would land them
near 0.68-0.89 GB/s. **The 0.9705x oracle is therefore not rate-reachable on this matrix
with P29 as it stands**: a rate-legal selector would be forced onto GRZ2 everywhere, which
is the 1.1071x naive result. Closing that gap is a P29 encode-throughput problem, not a
selector problem.

## Carried-through selection

Size-census selection is unchanged and size-consistent: 62,587,653 B, 1190.96x,
**0.9705x z19**. The rate-legal-restricted selection is identical only because no cell has
a legal candidate under this charge, so every cell falls back to the size minimum; that
row is a placeholder, not a rate-bound result.

## Status of this table

**INTERIM / conservative-two-pass**, pending bigoracle's ruling. Two labels stay attached:
P29's C uses the research interner and is not the product limit, and P29's F is the
codec's in-process per-stream proxy because there is no standalone P29 decoder -- a direct
codec report, but not the same measurement as GRZ's real standalone decode.
