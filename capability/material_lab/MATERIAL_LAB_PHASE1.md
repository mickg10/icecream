# material_lab — Phase 1 record (icecream #16)

Intended repo path: `capability/material_lab/MATERIAL_LAB_PHASE1.md`.
Single-process, byte-exact compression-research engine. NO fork/socket/process split.
Reuses `cap_codec` (Interner, S1 LZ, P24/M3 reduced-grammar materializer). Built on quietbox2.

## Verdict — NO-GO for the MATERIAL_BLOCK slot representation
The O0 free-definition **impossibility screen fires on all 16 fixed corpora and the aggregate**
(O0 = 93.1% of C0; gate needs ≤80%). The decisive fully-charged **O1 = 100.1% of C0 and 121.9%
of the RBASE-P29 frontier** — i.e. it is *worse than the current frontier on every corpus*. The
~20 MB gap is dominated by identifier-sequence entropy (73.1% of the residual is variable slot
content, ~85% identifiers), which static templating cannot remove; the structural headroom that
exists at z3 (16.4%) collapses to 1.2% under z19+long. **Recommendation: stop the static
MATERIAL_BLOCK language; do not start GPU/ranker work.**

## A. Coverage
`capability/coverage_manifest.tsv`. Phase-1 ran the **fixed-16** inventory only
(corpus + corpus2..corpus16 = 9,292 TUs, 28.55 GB raw). Corpus→project map confirmed from
manifest paths (llvm/rocksdb/duckdb/abseil/opencv/godot/fmt/spdlog/catch2/nlohmann-json/
range-v3/eigen/re2/leveldb/simdjson/cereal). native-only mega corpora (17–25) and the tar.zst
four-profile pilots are coverage passes, not the gate — not run here.

## B. Pinned Z19 baseline
libzstd **1.4.8**, `ZSTD_c_compressionLevel=19`, `ZSTD_c_enableLongDistanceMatching=1`,
`ZSTD_c_windowLog=27`, session reset per TU → one **independent whole-TU frame, no cross-TU
history**. CLI-equivalent: `zstd -19 --long=27 -c <tu>.ii` (v1.4.8). gcc 11.4.0.

## C. RBASE-M3 (cold0) and RBASE-P29 frontier curves  *(updated — real P29 consumed)*
- **RBASE-M3 (cold0, C0)**: reproduced BYTE-EXACT single-process. `ledger --mode m3` →
  **DuckDB 9,802,066** and **RocksDB 10,111,855** (= committed M3 to the byte). This is the
  trust anchor. Per-TU cold0 curve in `curve.*.tsv` (`cold0_this_tu`/`cold0_cumulative`).
- **RBASE-P29**: consumed VERBATIM from local-oracle's exact export
  `linecache/ml-artifacts/rbase-p29-fixed16-per-tu.tsv` on branch
  `local-oracle/issue16-p29-4build-matrix` — sha256 `0673e735…a32a` verified; 16 projects,
  9,292 TUs, total wire **91,864,787 B**, all rows `exact=true`. Wired per-TU into `curve.*.tsv`
  (`p29_this_tu`/`p29_cumulative`, matched on native-manifest TU order). **NOT regenerated or
  approximated.** Label: **STATEFUL / chronological (cross-TU flushes) — a valid online
  comparison, NOT an independently-framed result (NOT M4).**

### P29 (frontier) vs C0 (RBASE-M3) vs O1 (MATERIAL_BLOCK fully-charged)
```
project          TUs         P29      C0(M3)   O1(matlab)  P29/C0   O1/C0  O1/P29
llvm            1238     9328055     9523192     9735772    98.0%  102.2%  104.4%
rocksdb          622     9823481    10111855    10081124    97.1%   99.7%  102.6%
duckdb           689     9589726     9802066     9645571    97.8%   98.4%  100.6%
abseil           700     5863183     6126208     6245169    95.7%  101.9%  106.5%
opencv          1506     8563234     8803206     9036056    97.3%  102.6%  105.5%
godot           2207    39589524    56332520    55470208    70.3%   98.5%  140.1%
fmt               50     1046398     1232507     1305333    84.9%  105.9%  124.7%
spdlog            34      539513      714528      773873    75.5%  108.3%  143.4%
catch2           857     1006039     1200175     1247880    83.8%  104.0%  124.0%
nlohmann-json     99     1168818     1412488     1433450    82.7%  101.5%  122.6%
range-v3         259      874082     1063790     1133868    82.2%  106.6%  129.7%
eigen            650     1377958     1553861     1665386    88.7%  107.2%  120.9%
re2               72      488243      642164      704696    76.0%  109.7%  144.3%
leveldb           72      685350      854380      913525    80.2%  106.9%  133.3%
simdjson         153     1408595     1723543     1806910    81.7%  104.8%  128.3%
cereal            84      512588      703807      745658    72.8%  105.9%  145.5%
AGG-16          9292    91864787   111800290   111944479    82.2%  100.1%  121.9%
```
- **P29 = 91,864,787 = 82.2% of C0** — the current frontier. It beats RBASE-M3 by 17.8% (driven
  mainly by Godot: P29 70.3% of C0), but still **misses the 20% gate** (82.2% > 80%; 0.80·C0 =
  89,440,232) and the **400× target** (raw/400 = 71,386,679; P29 is 28.7% above it).
- **O1 (MATERIAL_BLOCK) = 111,944,479 = 100.1% of C0 = 121.9% of P29** — worse than both C0 and
  the frontier, on the aggregate and on every one of the 16 corpora (O1/P29 100.6%–145.5%).
  Building MATERIAL_BLOCK would regress ~22% from the existing P29 frontier.

## D. MATERIAL_BLOCK O0 curve — `oracle_free_definitions` (impossibility screen)
Skeleton dictionary FREE; charged = skeleton-ref + typed-ordinal + new-value-literal.
Aggregate O0 = **104,140,603 = 93.1% of C0** → **FAILS 0.80·C0**. Structure-fully-free variant
(skeleton bodies AND selection free; slot VALUES only) = 97,692,936 = **87.4% of C0** → still
FAILS. `curve.duckdb.o0.tsv` (`--test-mode o0`). Screen fires → representation impossible → STOP.

## E. MATERIAL_BLOCK O1 curve — `fully_charged` (decisive)
= O0 + rule-definition (skeleton-dict) bytes; residuals = 0 (full coverage); child-rules not
modelled (would only add cost). Aggregate O1 = **111,944,479 = 100.1% of C0**. Per-anchor:
DuckDB 9,645,571 (98.4% C0); RocksDB 10,170,967 (100.6% C0). `curve.duckdb.o1.tsv`,
`curve.rocksdb.o1.tsv` (`--test-mode o1`).

## F. Full-run Z / C / P and the hard inequalities
- **P < Z** (must beat independent-per-TU z19): trivially YES everywhere (the shared-window codec
  crushes independent z19 by ~10×; e.g. fmt Z=14,896,337 vs P≈1.1M, BE_Z19 = TU 1).
- **P ≤ 0.80·C0** (the binding constraint): **NO** for O0 and O1 on all 16 corpora and aggregate.
  BE_20 = never. Since both inequalities are required and 0.80·C0 fails, the representation fails.

## G. Component-byte deltas
MATERIAL_BLOCK only touches the `line_def` literal (RAW_RUN) component; root/region_def/block_def/
path_def/missing/framing are unchanged. DuckDB literal lane: cold0 5,478,002 → O0 4,499,464
(−978,538) → **O1 5,307,307 (−170,695 only; the skeleton-dict charge of 807,843 nearly erases the
free-def saving).** All headroom lives in one lane and is small once definitions are charged.

## H. Independent exact reconstruction
Via the two-process `cap_main` harness, F decodes from wire ONLY and byte-checks vs corpus:
DuckDB 689/689 and RocksDB 622/622 `reconstruct byte-exact=OK`, `system-header reads=0`.
material_lab's ledger equals cap_main's C-side wire byte-for-byte, so reconstruction is identical.
`--mode m4` also reproduces cap_main's CAP-M4 exactly (DuckDB 10,086,267 / RocksDB 10,404,024).

## Deliverables (`~/capability/` on quietbox2)
- `material_lab.cpp` / `material_lab` — subcommands: `ledger` (`--mode m3|m4`),
  `residual-census` (`--z19`), `curve` (`--test-mode o0|o1`, `--p29 <tsv>`, `--out`),
  `export-events`, `rbase-p29 --p29 <tsv>`.
- `cap_codec.{h,cpp}` — 5 `-Wmisleading-indentation` fixes + 1 null-guarded census hook
  (ledger byte-identical; `cap_codec.cpp.orig-bak` kept).
- `curve.duckdb.o1.tsv`, `curve.duckdb.o0.tsv`, `curve.rocksdb.o1.tsv` — schema:
  `tu_index, raw_this_tu, raw_cumulative, z19_this_tu, z19_cumulative, cold0_this_tu,
  cold0_cumulative, p29_this_tu, p29_cumulative, test_this_tu, test_cumulative,
  saving_vs_z19_cumulative, saving_vs_cold0_cumulative, saving_vs_p29_cumulative, exact,
  state_bytes`. (z19 columns 0 unless `--z19`.)
- `rbase-p29-fixed16-per-tu.tsv` — local-oracle's exact export (sha256 `0673e735…a32a`).
- `events.duckdb.zst`, `coverage_manifest.tsv`.

## Build / run
```
cd ~/capability
g++ -O3 -std=c++17 -DICE_LINE_CAP_LOG2=23 material_lab.cpp cap_codec.cpp -o material_lab -lzstd
./material_lab ledger --manifest ~/ictmp/corpus3/manifest.txt --z 3 --mode m3          # gate 9,802,066
./material_lab residual-census --manifest ~/ictmp/corpus3/manifest.txt --z 3
./material_lab curve --manifest ~/ictmp/corpus3/manifest.txt --z 3 --test-mode o1 \
    --p29 ~/capability/rbase-p29-fixed16-per-tu.tsv --out curve.duckdb.o1.tsv
./material_lab rbase-p29 --p29 ~/capability/rbase-p29-fixed16-per-tu.tsv                # frontier table
```
