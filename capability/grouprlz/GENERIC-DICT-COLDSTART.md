# Does a shipped generic C++ dictionary avoid the P29+BSC cold start?

**Answer: no — except on tiny libraries.** A content-addressed generic dictionary trained on
21.4 GiB of disjoint C++ (12 projects, 4.75 M distinct lines, **23.6 MB** shipped) removes
**5.0 %** of RocksDB's build-#1 bootstrap penalty, **10.0 %** of Abseil's, **1.8 %** of Firefox's
and **0.7 %** of LLVM-full's. It removes **72.1 %** on cereal. The bootstrap transfer is still
required for every project that matters.

The mechanism is not broken — the *content* is. Run the identical pre-seed machinery with the
project's **own** prior instead of the generic one and it removes 99.2–99.7 % of the same wire
(that is the `warm` column, measured with the same code path). What a generic dictionary cannot
supply is the only thing that matters: **the target project's own lines.**

## Result

All sizes are bytes on the wire, byte-exact, decode-verified. `gap_closed` =
`(cold_nodict − cold_withdict) / (cold_nodict − warm)`.

| corpus | dict | cold_nodict | cold_withdict | warm | **gap_closed** | dict covers N% of the corpus's distinct lines |
|---|---|---:|---:|---:|---:|---:|
| cereal (84 TU, 0.33 GB) | DICT-A | 459,374 | 129,344 | 1,503 | **72.08 %** | 84.9 % |
| abseil (700 TU, 2.58 GB) | DICT-A | 5,058,543 | 4,555,861 | 30,989 | **10.00 %** | 21.7 % |
| rocksdb (622 TU, 3.11 GB) | DICT-A | 8,537,705 | 8,110,350 | 31,463 | **5.02 %** | 12.2 % |
| rocksdb | DICT-B (+57 % bigger) | 8,537,705 | 8,082,528 | 31,463 | **5.35 %** | 12.4 % |
| firefox (10,010 TU, 109.6 GB) | DICT-A | 37,391,954 | 36,723,636 | 213,148 | **1.80 %** | 5.5 % |
| llvm-full (8,141 TU, 43.1 GB) | DICT-A1 | 58,526,788 | 58,118,781 | 445,619 | **0.70 %** | 1.0 % |

`gap_closed` tracks distinct-line coverage almost exactly. That is the whole story: the wire is
dominated by *new line definitions*, and a generic dictionary only pays for lines it happens to
contain.

### Firefox specifically

**gap_closed = 1.80 %.** A generic C++ prior does not transfer to a giant. Two candidate causes,
and the experiment separates them:

* **Toolchain mismatch.** Firefox's `.ii` were preprocessed by clang against **gcc-10** libstdc++
  headers (`/usr/lib/gcc/x86_64-linux-gnu/10/…`); every training corpus is gcc-11 native
  (`/usr/include/c++/11`). No disjoint gcc-10 corpus exists on the box (the only one is
  gecko-dev, which *is* Firefox), so this arm cannot be made toolchain-matched.
* **Project size.** Large projects' distinct lines are overwhelmingly their own.

**LLVM-full is the control that decides it.** It is a giant (8,141 TU, 43.1 GB), it is
gcc-11 native — *the same toolchain as the dictionary* — and it is disjoint from DICT-A1
(DICT-A with LLVM removed). It scores **0.70 %**, worse than Firefox. So Firefox's poor transfer
is **not** primarily a toolchain artifact; scale is the dominant term, and the toolchain mismatch
if anything makes Firefox look slightly *better* than a matched giant would.

### Growing the dictionary does not help

DICT-B adds gcc, Qt/qtbase and folly: **+49 % raw training bytes, +44 % distinct lines, +57 %
shipped size (23.6 → 37.2 MB)**. RocksDB's gap_closed moves **5.02 % → 5.35 %** and line
coverage **12.20 % → 12.41 %**. Extrapolating that slope, closing even half of RocksDB's
bootstrap would need a dictionary many orders of magnitude larger than the program it is
compressing. C++ preprocessed output is project-specific in the tail that costs bytes.

### Rate

Every condition clears the ≥1 GB/s C-encode gate. `cold_withdict_GBps` in the TSV is the
**whole-run** C-encode proxy reported by the codec, which *includes re-encoding the pre-installed
prefix* — this harness has no state-load path, so it re-derives the dictionary state by running
the training TUs. `cold_nodict_GBps` (3.4–23.7 GB/s) is the clean per-corpus figure. A
differential (prefix+test minus prefix-only) was attempted and is **not reported**: on the small
corpora the difference is inside the run-to-run noise and came out negative for cereal.

## Method

**Codec.** `codec50` P29 + BSC literal groups — local-oracle's
`/tmp/issue16-p29-bsc-integration-20260817/src/codec50.cpp`, flags exactly as in the published
fixed-16-native runs (`--z 3 --mixed-regions --byte-array-lines --direct-ordinals
--compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024
--blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3
--literal-group-tus 112 --literal-group-workers 16 --literal-group-skip-zstd10`), two-phase
(`--mixed-dump-prefix` then `--literal-group-prefix`).

**Anchor.** The unmodified binary reproduces the published RocksDB P29+BSC cold wire
**8,537,705** exactly. My re-runs of abseil (**5,058,543**) and cereal (**459,374**) also match
the published fixed-16-native values byte for byte.

**Rebuild.** The stock binary aborts (`short`) on >131 k distinct short lines — a fixed
`SHORT_CAP=1u<<17` interner table — which every dictionary-prefixed giant exceeds. I made
`TINY_CAP`/`SHORT_CAP` overridable and rebuilt at `-DICE_LINE_CAP_LOG2=25
-DICE_SHORT_CAP_LOG2=23 -DICE_TINY_CAP_LOG2=16`, statically linked against **zstd 1.4.8 built
with `ZSTD_MULTITHREAD=1`** (Ubuntu's system libzstd 1.4.8 is built without MT and rejects
`ZSTD_c_nbWorkers`). Both rebuilds reproduce the 8,537,705 anchor **byte-identically**, and the
MT build reproduces Firefox cold **37,391,954** byte-identically against the stock binary. The
capacity change is output-neutral: ids are assigned in first-seen order, independent of table
geometry.

**Pre-seed mechanism.** The codec has no dictionary-load flag, so the shipped package is modelled
as *the P29 state after ingesting the training TUs*: manifest = `[training TUs] ++ [test TUs]`,
one chronological pass with retained state. **The dictionary never appears on the wire** — the
reported `cold_withdict` is `cumulative_wire(last TU) − cumulative_wire(last training TU)` taken
from the run's own per-TU curve, so not one byte of the prefix is charged. `warm` is the same
construction with the corpus itself as the prefix. Because BSC literal groups are indexed
`TU_index / 112`, every prefix is **padded to a multiple of 112** by repeating leading (already
interned, ~0-wire) TUs, so no group ever straddles the train/test boundary and no group wire is
mis-attributed.

**Byte-exactness.** `byte-exact=OK` on every run, and `exact=true` on every row of every per-TU
curve — checked separately for the test segment. `decode_exact=true` in the TSV means: the whole
run round-tripped, *and* every test TU round-tripped, decoding against the pre-installed state
plus the test wire only.

**z19 reference.** `zstd -19 --long=31` over the ordered whole-program `.ii`, from
`capability/grouprlz/corpora.tsv` (built by `prep.sh`/`baseref.sh`).

## Train/test disjointness

**DICT-A (12 projects, 7,840 padded TU, 22,477,963,409 B raw, 4,745,129 distinct lines, 23,628,157 B shipped):**
llvm, duckdb, opencv, godot, fmt, spdlog, catch2, nlohmann-json, range-v3, eigen, re2, simdjson
(corpus, corpus3, corpus5–corpus13, corpus15).

**DICT-B** = DICT-A + gcc, qtbase, folly (corpus17, corpus19, corpus22) — 10,192 TU, 33.6 GB,
6,851,499 distinct lines, 37,197,391 B shipped.

**DICT-A1** = DICT-A **minus llvm**, used only for the LLVM-full control — 6,608 TU, 18.9 GB,
4,018,421 distinct lines, 18,918,151 B shipped.

No test target (rocksdb, abseil, cereal, firefox, llvm-full) is in any training set. Beyond
checking manifest paths, every candidate training corpus was scanned **at the content level**
(`grep -a` over the concatenated `.ii`) for `absl/`, `rocksdb/`, `leveldb/`, `cereal/`: all
twelve DICT-A projects returned **zero** hits.

Deliberately excluded from training:

| corpus | why |
|---|---|
| leveldb (corpus14) | RocksDB is a fork of LevelDB — ancestor-code leakage into a test target |
| gecko-dev (corpus18) | same project as the firefox target |
| clickhouse (corpus20) | libc++/clang toolchain; vendors rocksdb and abseil |
| pytorch (corpus21) | content scan hit `absl/` |
| arrow (corpus23) | content scan hit `absl/` |
| bitcoin (corpus24) | content scan hit `leveldb/` |
| chromium (corpus25) | vendors abseil |

Firefox's own manifest contains 222 paths matching `absl|abseil` (it vendors abseil-cpp). That is
irrelevant here — both are *test* targets, neither is training data.

## Caveats

* The modelled package is the full P29 state (lines, regions, S1 blocks, paths), not a bare line
  table — i.e. **more** generous than the line-hash dictionary the claim proposes. `dict_bytes`
  is the shippable payload: `zstd -19 --long=31` of the deduplicated distinct-line text
  (`p.literal.raw`) from the training-only run.
* Ordinals are assigned in first-seen order, so a large prior pushes reference ordinals higher.
  This is charged honestly (it is inside `cold_withdict`) and is part of why bigger dictionaries
  barely pay.
* `warm` is an *identical* rebuild — the true floor. A realistic incremental rebuild sits above
  it, which would make `gap_closed` marginally larger for every row; it does not change any
  conclusion.
* All runs on quietbox2 (EPYC 8124P, 503 GB); gcc 11.4.0; artifacts under `~/gdict`
  (`run_alt.sh`, `mkman.py`, `wire.py`, `runs/*.curve.tsv`).

## Artifacts in this directory

* `generic-dict-coldstart.tsv` — the ledger.
* `gdict-harness/` — `run_alt.sh` (two-phase P29+BSC driver), `mkman.py` (padded prefix +
  test manifests), `wire.py` (boundary subtraction), `collect.py`, `codec50-bigcap.patch`
  (the two-constant capacity change), `BUILD.md` (exact rebuild recipe + anchor).
* `gdict-runs/` — every run's `.out` (each carries `byte-exact=OK` and its `TOTAL=`), plus
  `curve-boundaries.tsv`: prefix TU count, prefix cumulative wire, total cumulative wire, the
  subtracted test wire and the two `exact` flags for all sixteen measurement runs. Every row of
  the TSV can be re-derived from that file alone.

## What this means for deployment

Shipping a generic dictionary with the package is cheap (23.6 MB) and it is **not useless** — it
is close to free and it genuinely helps small header-dominated libraries. But it does not remove
the cold-start transfer for RocksDB, Abseil, Firefox or LLVM: those still need their own history
shared before the online tier reaches its ceiling. The bootstrap transfer stays on the roadmap.
