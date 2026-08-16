# Target-disjoint 25-corpus empty-vs-pretrained control

Implementer's fixed-package control for local-oracle's #16 online-bootstrap bake-off.
Requested in local-oracle's P18 comment: *"continue the fixed-package 25-corpus
empty-versus-pretrained control and publish the complete per-TU data."* This is that
complete per-TU data. All 25 corpora, both passes, **byte-exact** (25/25 each).

## What was run

Per corpus, the online superblock codec (`online_bootstrap_curves.py`) over the first
**200 TUs** in manifest order (`ml_bakeoff --max-files 210` → curve `--max-tus 200`,
first-N standard order), zstd-3, `--model-level 3`, `--publication first-use`,
`--budget-basis ids32`, `--online-budget 524288`, `--thresholds 2`:

- **pretrained**: `--row-set pretrained --pretrained-mode seed-only` → one vote,
  `pretrained-seed-online-k2`. Seed is a **static, co-resident** pack
  (`initial_model_wire_bytes = 0`), charged nothing per job — the online definitions
  (first-use) are charged.
- **empty**: `--row-set empty` → `empty-online-k2` (same codec, **no seed**).

**Disjoint seed rule:** `online-bootstrap-common-rocks-opencv.zst` for all except
RocksDB/OpenCV (in that pack) → `online-bootstrap-common-llvm-godot.zst`, so every test
corpus is held out of its seed's training set.

## The finding

1. **Most compression is project-local self-bootstrap.** The *empty* codec alone reaches
   DuckDB 391×, Firefox 767×, Godot 689× — just by learning each project's own recurring
   structure over 200 TUs.
2. **The seed is a front-loaded, toolchain-match-dependent top-up.** Final cumulative-ratio
   gain `pretrained ÷ empty` at TU 200: **matched (gcc-11) median 1.17× (+17%**, range
   1.00 cereal → 1.55 re2); **mismatched (clang) ~1.00×** (seed inert).
3. **Per-TU shape:** TU 1 always identical (nothing to reference yet); matched pairs
   diverge at TU 2 (seed head start, ~3× cheaper early) then **reconverge** as empty
   self-learns; mismatched pairs stay superimposed.
4. **The `.ii` seed does not cross a toolchain boundary** — the three clang/libc++
   projects get ≈0 from the gcc-11 seed, because preprocessed `.ii` is toolchain-specific
   down to the bytes. Empirical support for the report's training boundary: the universal
   bootstrap must be raw-source + structural-program trained (toolchain-invariant), with
   the byte-substrate derived per-env; the `.ii` seed is a mechanism/accounting control.

## Toolchain map (fingerprinted from `.ii` include roots)

22 corpora are gcc-11/libstdc++; the three megas built with their own toolchain are:
Firefox = clang-21/libstdc++15, ClickHouse = clang-21/libc++, V8 = clang-23. See
`toolchain-map.tsv`.

## Files

- `bakeoff25.tsv` — pretrained votes (charged_ratio, tus, seed_model_bytes, exact).
- `bakeoff25_empty.tsv` — empty-online votes.
- `bakeoff25_joined.tsv` — joined with codec50 cold + S0-amortized (from `study25.tsv`).
- `toolchain-map.tsv` — corpus → name → toolchain → prior_match → seed_pkg.
- `curves/<corpus>.tsv`, `curves_empty/<corpus>.tsv` — **complete per-TU curves** (18
  columns; the metric is `wire_bytes` col 8 ÷ `raw_bytes` col 3). Empty TSVs carry two
  specs (`empty-frozen` + `empty-online-k2`) — filter `empty-online-k2`.
- `curve_overlay.json` — per-TU aligned arrays (`pre`, `empty`, cumulative ratios).
- `curves_viz.html` — the published figure source (Artifact `603cd618`).
- `scripts/` — drivers + analysis (`bakeoff25_qb.sh`, `bakeoff25_empty_qb.sh`,
  `join_bakeoff.py`, `analyze_curves.py`, `build_curves_viz.py`).

## Reproduce

On a box that can hold the corpora (quietbox2, 503 GB): `bash scripts/bakeoff25_qb.sh`
then `bash scripts/bakeoff25_empty_qb.sh`, then `python3 scripts/analyze_curves.py`.

## Published

- Curves figure (this control): https://claude.ai/code/artifact/603cd618-fe74-4a84-b3cb-4ec5df3512a8
- local-oracle deep report (canonical): https://claude.ai/code/artifact/5b1d1d31-43f6-4e27-98b2-34e288374b3f
