# material_lab Phase 1 — MATERIAL_BLOCK O0→O1: **NO-GO** (representation screened out)

The single-process byte-exact research engine screens the static MATERIAL_BLOCK slot representation
against the bake-off gates. Result: **the O0 impossibility screen FAILS on all 16 fixed corpora → the
representation is impossible → STOP; do not start GPU/ranker work.** Independently re-verified.

## Trust anchor (single process, byte-exact)
`material_lab ledger --mode m3` reproduces the committed M3 ledger to the byte — DuckDB **9,802,066**,
RocksDB **10,111,855** (all 7 categories). `--mode m4` reproduces cap_main's CAP-M4 byte-for-byte.
Independent reconstruction via cap_main: 689/689 + 622/622 byte-exact, system-header reads = 0.
The 5 `cap_codec.cpp` `-Wmisleading-indentation` warnings are fixed (behavior-preserving; ledger unchanged; diff = 6 lines incl. one null-guarded census hook).

## The decisive census (DuckDB — C = 9,802,066; 0.80·C = 7,841,653; raw/400 = 4,964,288)
Literal residual: **28% fixed skeleton, 72% typed slots; 85% of slot bytes are identifiers** (id 20.6 MB of 24 MB).
Near-repeat lines (shared skeleton ≥2) = **83.5% of raw** — templatable structure exists, but the slot *content* is the entropy.

| ceiling (z3) | bytes | % of C | ≤ 0.80·C? |
|---|---:|---:|---|
| O0 `oracle_free_definitions` (impossibility screen) | 8,837,728 | 90.2% | **NO** |
| structure FULLY free (skeleton + selection free, slot values only) | 8,046,637 | 82.1% | **NO** |
| **O1 `fully_charged` (decisive gate)** | 9,645,571 | **98.4%** | **NO** |

Fixed-16 aggregate: **O0 = 93.1% of C0, O1 = 100.1%** (costs more than doing nothing); per-corpus O0 86.0–95.1%, O1 98.4–109.7% — every corpus fails.

## Why it's dead (honest)
- Even with the skeleton dictionary AND selection FREE, the identifier-slot entropy alone is **82.1% of C** — above the 80% gate. There is not enough removable structure.
- The static structural gain over plain zstd is 16.4% at z3 but **collapses to ~1% at z19+long** — raising the zstd level already captures almost everything MATERIAL_BLOCK templating would.
- The residual is genuine **identifier-sequence entropy** (~1 B/occurrence after zstd). Only a *sequence predictor* could reduce it, and reaching raw/400 (63.8% of C) would require near-oracle identifier prediction.

## Decision
O0 screen FAILED → **static MATERIAL_BLOCK representation impossible → STOP / redesign.** No GPU/ranker.
The one remaining lever on the residual is identifier-sequence prediction — a much larger, speculative ML
bet — and whether that's worth pursuing is a goal-level question (the transfer-bound-vs-compile-bound
grounding of cold-400× itself is still unmeasured).

## Deliverables (`~/capability/`, committed)
`material_lab.cpp` (`ledger` `--mode m3|m4` · `residual-census` `[--z19]` · `curve` `--test-mode o0|o1` · `export-events` · `rbase-p29` hook),
fixed `cap_codec.{h,cpp}`, `curve.{duckdb,rocksdb}.{o0,o1}.tsv` (13-column schema), `events.duckdb.zst`, `coverage_manifest.tsv`.
Z19 pinned: libzstd 1.4.8, level 19, LDM on, windowLog 27, per-TU session reset (`zstd -19 --long=27` equiv), gcc 11.4.0.
RBASE-P29 = **pending local-oracle's per-TU export** (not approximated). Coverage this phase = fixed-16 only; 25-native + 44-pilot readers are secondary follow-ups.
