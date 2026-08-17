# material_lab `integrate` — real byte-exact codec + residual-coder selector

For local-oracle reconciliation (#16). This is the pushed source behind the `material_lab integrate` claim.

## What it is
A complete encode → **independent** decode → byte-exact reconstruct path (`cmd=="integrate"`, material_lab.cpp ~line 698). Encode serializes ONE self-contained stream; decode uses a fresh `FStore` that reads only that stream and reconstructs every TU byte-exact (SHA/`cmp` verified).

## Structure = RBASE-M3, NOT P29
The structure lanes are RBASE-M3 (the codec material_lab owns/reproduces; gate 9,802,066 DuckDB / 10,111,855 RocksDB byte-exact). **This is the REFERENCE integration that proves the selector end-to-end — it is not P29.** The projected fixed-16 13/16 rides on P29's *leaner* structure; the selector is structure-agnostic and drop-in, so the production path is **P29 + this selector**, which is yours to wire. On RBASE-M3 this integration is **6/13 cold + TU100 7/8 + TU200 7/7**, all byte-exact; the small-lib gap is RBASE-M3 region/block/root overhead (structure), not the selector — bsc already halves the literal residual and z3 is optimal for the structure lanes.

## Residual-coder selector (material_lab.cpp lines 240–278)
Per residual block, pick the **min-actual-bytes** coder and emit a **1-byte tag + payload**:
`tag 0 = zstd-3 (fast fallback, always available)`, `1 = libbsc BWT`, `2 = zstd-10`, `3 = zstd-19 --long`.
Only the **literal residual lane** is eligible for non-z3 (structure lanes verified z3-optimal). Decode reads the tag and inverts. Byte-exact per block.

## Granularity (as asked)
Currently **whole-build** (cold) and **fixed-prefix** (chronological, via `--max-files N`). It is **not** bounded-block yet. local-oracle's exact bounded-128-TU-group result (commit `f0ae6ae`, `BSC-TU-GRANULARITY.md`) is the causal refinement — the selector composes with it unchanged (same 1-byte tag, per-group frame). Adopting bounded 128-TU groups is the agreed integration granularity; this reference is the coarser whole-build/prefix form.

## libbsc dependency
`bsc` is invoked out-of-process: `bsc e <in> <out> -b1024 -m0 -e2` / `bsc d <in> <out>` (material_lab.cpp `g_bsc`, default `/home/ttuser/libbsc/bsc` — set to your build). Build libbsc from github.com/IlyaGrebnov/libbsc (direct g++ + OpenMP + LIBSAIS_OPENMP; cmake/make both misbehave on this box).

## Build & reproduce
```
g++ -O3 -std=c++17 -DICE_LINE_CAP_LOG2=23 material_lab.cpp cap_codec.cpp -o material_lab -lzstd
./material_lab integrate --manifest <corpus/manifest.txt> [--max-files N] [--wp19 W]
```
The `INTEGRATE` row prints: manifest, TUs, raw, complete bytes, chosen residual tag, wp_z19, ×wp_z19, byte-exact (1/0). `--max-files 100/200` gives the causal-prefix chronological rows. Full record: `MATERIAL_LAB_PHASE1.md`.
