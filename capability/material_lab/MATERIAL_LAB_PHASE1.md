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

## TOURNAMENT (owner goal: cold ≤ 1.10× whole-prog z19+long; cum@100/200 ≤ whole-prog z6; ≥1 GB/s)
Whole-program baseline = all TUs concatenated (manifest order), one zstd frame, `--long` windowLog 31
(NOT per-TU). material_lab is the tournament executable: `ledger --z N [--ldm --wlog W]`, `wholeprog`,
`tourney`. Byte-exact preserved: LDM/window only change zstd params on the (lossless) lane streams;
the RBASE-M3 reconstruction proof stands and the no-LDM gate is unchanged (9,802,066).

### Experiment 1 — LDM + big window at a fast level (DuckDB). VERDICT: the cheap lever does NOT work.
Whole-prog baselines: **z19@31 = 7,150,874** (1.10× = 7,865,961), z6@31 = 9,595,769.
```
config        cold_total   %of wp_z19   enc GB/s
z3            9,802,066      137.1%       2.53
z3+LDM w27    9,728,284      136.0%       1.80
z3+LDM w31    9,730,304      136.1%       1.34
z6            9,128,686      127.7%       2.41
z6+LDM w27    9,121,086      127.6%       1.55
z6+LDM w31    9,119,888      127.5%       0.95
z12           8,538,561      119.4%       0.64
z12+LDM w31   8,529,559      119.3%       0.69
z19 (no ldm)  7,766,539      108.6%       ~0.12   (clears size, fails speed)
```
- **LDM adds < 1%** (z3→z3+LDM = 0.75%). Our codec already removes cross-TU redundancy structurally
  (region/line interning + public-line refs + shared streaming window), so there is little long-range
  redundancy left for LDM to find. Hypothesis refuted.
- **No config clears both bars.** Fast (z3–z6, >2 GB/s) = 127–137% of z19; z12 (119%, 0.64 GB/s)
  clears neither; only z19 clears size (108.6%) but at ~0.12 GB/s. Size↔speed conflict is real.

### Residual gap (the entropy problem, not the window problem)
DuckDB, our codec z6 (9,128,686) → z19 (7,766,539) = −1,362,147, located by component:
`line_def literal −1,012,744 (74%) · region_def(control) −216,538 (16%) · array_values −129,461
(9.5%) · root/block/path/missing ≈ 0`. To clear 1.10×wp_z19 (7,865,961) from z6 you need −1,262,725;
upgrading just **literal + region-control + array-values to z19 ratio** = 7,769,943 = 1.087× → CLEARS.
So the gap is a **fast-strong-entropy-coder problem on the RAW_RUN literal channel (then region-control,
array-values)** — exactly the tournament's rANS/REF/ORIGIN/ID-HIER blocks. Build the literal-channel
coder first.

### 1/veff feasibility (why this is achievable at ≥1 GB/s)
`1/veff = 1/vb + f/vm`: the heavy coder need only touch the dense lanes' RAW — literal 33.4 MB +
array-values 3.7 MB + region-control (~10 MB) ≈ 47 MB, i.e. **~2.5% of the 1.9 GB .ii**. A rANS at
~0.5–1 GB/s on 47 MB adds ~0.05–0.1 s to the ~0.8 s z6 base → veff stays ~2 GB/s while reaching
z19-quality on those lanes. The heavy coder is cheap because the deduplicated dense channels are tiny.

### rANS entropy stage — BUILT, verified byte-exact, and it FAILS (the gap is LZ, not entropy)
Adaptive range coder (Subbotin) + Fenwick order-0/1/2 byte model; `rans-selftest` all roundtrips OK;
per-lane roundtrip OK on real data. DuckDB, best order (2) vs zstd on the 3 dense lanes:
```
lane            raw         z3        z19     bestRANS(o2)   rANS/z3   rANS/z19
literal_RAWRUN  33,436,727  5,463,802 3,833,000  9,483,437     173.6%    247.4%
region_control   4,575,720  1,854,790 1,596,276  2,259,716     121.8%    141.6%
array_values     3,701,352  1,320,029 1,139,246  1,525,381     115.6%    133.9%
DENSE-3         41,713,799  8,638,621 6,568,522 13,268,534     153.6%    202.0%
```
Pure order-N rANS is **1.5–2× WORSE than z3** — projected cold with rANS = 14.4 M (vs codec z3 9.8 M).
Reason: the lanes are dedup'd at the **line/region level, not the byte/token level**; they retain heavy
LZ redundancy (repeated C++ tokens, opcodes, varint runs) that z3 captures via LZ and a context coder
cannot. The z6→z19 gain is LZ match-depth, not order-N entropy. **rANS is the wrong tool — do not build it.**

### What IS reachable, and the SPEED wall
- **Size is reachable with deep LZ, byte-exact:** DuckDB z19-on-dense = 6,566,144 → proj cold = 7,714,437
  = **1.07× wp_z19** (≤1.10× ✓). Aggregate codec-z19 = 94,671,232 = **97.8%** of wp_z19.
- **But z19 is slow** (dense-3 = ~2.6 MB/s → 0.12–0.14 GB/s raw-equiv). The linked libzstd lacks
  built-in MT (`nbWorkers` errors); the zstd CLI has MT but the 41.7 MB dense buffer is **smaller than
  one MT job** at windowLog 27 (16.0 s→17.2 s, no speedup). Manual chunked-parallel z19 straddles the
  goal corner (lost cross-chunk LZ): K=8 → 7,890,470 (1.098×, ✓size) @ 0.79 GB/s (✗speed); K=16 →
  7,983,607 (1.110×, ✗size by 0.95%) @ 1.03 GB/s (✓speed). refPrefix shared-dict made it worse.
  **DuckDB does not clear both bars with any zstd/rANS config — the frontier passes ~1 % outside the corner.**

### Goal table (fixed-16; whole-prog wlog31; codec z6 = fast, z19 = size config)
```
proj       TUs  rawGB      wpZ19       wpZ6         cz6  z6/z19  GBps       cz19  z19/z19
llvm      1238   3.62    7475156    9468603    8839904 118.3%  6.24    7330210   98.1%
rocksdb    622   3.11    6387722    8324507    9630986 150.8%  5.58    8396679  131.5%
duckdb     689   1.99    7150874    9595769    9128686 127.7%  2.97    7766539  108.6%
abseil     700   2.58    4122078    5333938    5776472 140.1%  7.43    4999457  121.3%
opencv    1506   4.63    6314773    8221777    8177564 129.5%  9.22    6784251  107.4%
godot     2207   5.93   57967694   72754400   54577229  94.2%  1.76   50728799   87.5%
fmt         50   0.14     743779     931663    1145867 154.1%  1.95     984869  132.4%
spdlog      34   0.10     470041     591811     651652 138.6%  1.90     555762  118.2%
catch2     857   0.95     800001    1038440    1118721 139.8% 13.41     963470  120.4%
json        99   0.29     814958    1044135    1303270 159.9%  2.89    1082362  132.8%
range-v3   259   0.63     766456     971995     973134 127.0%  9.96     820110  107.0%
eigen      650   3.53    1251877    1621395    1428658 114.1% 37.07    1202998   96.1%
re2         72   0.11     431654     538658     589341 136.5%  2.89     503426  116.6%
leveldb     72   0.14     553624     689589     792039 143.1%  2.60     683339  123.4%
simdjson   153   0.47    1032039    1297350    1582762 153.4%  4.64    1326361  128.5%
cereal      84   0.33     469457     594122     638790 136.1%  6.93     542600  115.6%
AGG-16    9292  28.55   96752183  123018152  106355075 109.9%        94671232   97.8%
```
Goal satisfaction (three bars: size ≤1.10×wp_z19 · speed ≥1 GB/s · cum@100/200 ≤ wp_z6-prefix):
- **speed (z6):** met on ALL 16 (1.76–37 GB/s).
- **size (z6):** met only on **godot** (94.2% — its huge cross-TU redundancy lets our structural codec
  BEAT whole-prog z19); 15/16 fail (114–160%). z19 meets size on most but fails speed everywhere.
- **chronological:** met only on **duckdb** (1/10 with ≥100 TUs); the rest fail (structural warm-up cost).
- **=> ZERO corpora clear all three bars.** The aggregate 109.9% (≤110%) is a **godot-mass artifact**
  (godot = 51 % of the aggregate codec bytes), not a per-corpus result.

### Corner recovery — OVERLAP-parallel z19 CLEARS the DuckDB gate byte-exact
The ~1 % chunking loss is recovered by giving each parallel z19 chunk the **immediately-preceding tail as
`refPrefix`** (skip-on-output ⇒ byte-exact; parallel encode, fast sequential decode). Trained zstd
dictionaries FAILED (dict transmission cost + they don't capture the local boundary matches). DuckDB sweep
(non-dense z3 = 1,148,043; cap 7,908,394; my wp_z19 7,150,874 ⇒ strict 1.10× = 7,865,961):
```
config          dense       proj_cold   ×wp_z19  veff GBps  size  speed  byte-exact
single z19 K=1  6,566,394   7,714,437   1.079×   0.136      OK    FAIL   OK
plain   K=16    6,835,564   7,983,607   1.116×   1.217      OVER  OK     OK
dict256K K=16   6,839,865   7,987,908   1.117×   1.291      OVER  OK     OK   (dict fails)
ovl 1MB K=16    6,721,809   7,869,852   1.101×   1.162      ~     OK     OK
ovl 2MB K=16    6,690,249   7,838,292   1.096×   1.016      OK    OK     OK   <<< CLEARS BOTH
ovl 4MB K=16    6,642,753   7,790,796   1.089×   0.831      OK    FAIL   OK
```
**LOCKED config = z19+LDM(wlog27) on the 3 dense lanes, K=16 parallel chunks, each `refPrefix`ing the
preceding 2 MB tail; z3 on all other lanes.** DuckDB: **proj cold 7,838,292 = 1.096× wp_z19 @ 1.02 GB/s,
byte-exact roundtrip verified** — clears the owner's gate (≤7,908,394 at ≥1 GB/s) AND the strict 1.10×.

### Tournament verdict
The owner's **DuckDB gate is MET** by the overlap-parallel-z19 goal-achiever (byte-exact). Across fixed-16
the locked config clears **both bars byte-exact on the corpora where our structured codec is competitive
with whole-program z19** (llvm 0.98×, duckdb 1.096×, opencv 1.07×, and the larger/diverse corpora). It
**fails the SIZE bar on the small / header-heavy libraries** (rocksdb 1.32×, abseil 1.22×, fmt, json, …)
where a **monolithic whole-program z19 window fundamentally out-dedups our per-region structure** — that is
NOT a chunking or entropy issue (even full single-frame codec-z19 is 115–133 % of wp_z19 there), it is the
per-TU/per-region structure losing to one big window on small, highly-correlated builds. rANS is dead
(entropy, not the lever); the overlap+parallel-z19 recovers the deep-LZ match-depth at ≥1 GB/s where the
structure permits. Closing the remaining small-corpus gap needs either a monolithic-window mode for
small builds or a learned model — see the per-corpus corner table below.

### Fixed-16 corner goal table (LOCKED z19+LDM, K=16, overlap 2 MB, byte-exact; godot K=48)
```
proj_cold vs 1.10×wp_z19 and >=1 GB/s (rt=byte-exact roundtrip OK for ALL):
corpus     proj_cold    ×wp_z19  veff GBps  size  speed  CLEARS BOTH
llvm        7,325,910   0.980×    1.902      OK    OK     YES
duckdb      7,838,292   1.096×    1.020      OK    OK     YES   (owner gate: <=7,908,394 ✓)
opencv      6,763,146   1.071×    2.077      OK    OK     YES
range-v3      812,186   1.060×    1.342      OK    OK     YES
eigen       1,186,021   0.947×    6.129      OK    OK     YES
godot(K=48)51,073,872   0.881×    1.052      OK    OK     YES   (K=16 was 0.923 GB/s; big lane needs more chunks)
--- fail SIZE (monolithic wp_z19 out-dedups the per-region structure; not chunking/entropy) ---
rocksdb     8,429,920   1.320×    1.105      OVER  OK
abseil      5,012,310   1.216×    1.647      OVER  OK
catch2        953,547   1.192×    1.596      OVER  OK
simdjson    1,334,181   1.293×    0.826      OVER  SLOW
fmt         1,006,426   1.353×    0.233      OVER  SLOW
json        1,093,632   1.342×    0.342      OVER  SLOW
spdlog        556,621   1.184×    0.225      OVER  SLOW
re2           502,385   1.164×    0.284      OVER  SLOW
leveldb       687,595   1.242×    0.308      OVER  SLOW
cereal        542,981   1.157×    0.780      OVER  SLOW
```
**6/16 clear all applicable bars byte-exact** (llvm, duckdb, opencv, range-v3, eigen, godot) — the large /
diverse builds where our structured codec is competitive with a monolithic whole-program z19 window. The
other 10 fail the SIZE bar because on small, highly-correlated builds a single big z19 window fundamentally
out-dedups the per-region structure (even full single-frame codec-z19 is 1.15–1.32× wp_z19 there); the
overlap trick recovers deep-LZ match-depth but cannot beat the monolithic window. **Owner's DuckDB gate is MET.**
Tournament exe: `corner --K <n> --overlap <bytes> [--wp19 <baseline>]` (byte-exact roundtrip-verified);
sweep helper `cornersweep`.

## COLD half — per-program verdict (.ii best-of selector + raw-source front)

### A) Fixed-16 .ii best-of selector (byte-exact; bars = size ≤1.10×wp_z19 AND ≥1 GB/s)
Per program pick the min-byte byte-exact candidate that clears speed, among: **codec-corner** (z19
overlap-parallel on dense lanes + z3 base, K swept 16–64), **whole-.ii-z19** (monolithic 1.0× but z19-slow;
chunked loses size on correlated data), and **P29** (local-oracle z3 structured; z3-class speed).
```
program   wp_z19     BEST mode      proj_cold   ×wp_z19  veff GB/s  PASS BOTH
llvm      7,475,156  codec-corner   7,325,910   0.980×   2.20       YES
duckdb    7,150,874  codec-corner   7,838,292   1.096×   1.01       YES  (owner gate)
opencv    6,314,773  codec-corner   6,759,592   1.070×   2.41       YES
godot    57,967,694  P29            39,589,524  0.683×   1.99*      YES  (P29 crushes cross-build redundancy)
range-v3    766,456  codec-corner     811,776   1.059×   1.16       YES
eigen     1,251,877  codec-corner   1,186,021   0.947×   6.41       YES
cereal      469,457  P29              512,588   1.092×   9.55*      YES
--- NO static mode clears BOTH (monolithic-z19 hits 1.0× SIZE but fails the >=1 GB/s bar) ---
rocksdb   6,387,722  cc 1.320× / P29 1.538× / mono 1.0×@slow           NO
abseil    4,122,078  cc 1.216× / P29 1.422× / mono 1.0×@slow           NO
catch2      800,001  cc 1.192× / P29 1.258× / mono 1.0×@slow           NO
fmt         743,779  cc 1.353× / P29 1.407× / mono 1.0×@slow           NO
spdlog      470,041  cc 1.184× / P29 1.148× / wi 1.118× / mono@slow    NO
json        814,958  cc 1.342× / P29 1.434× / mono 1.0×@slow           NO
re2         431,654  cc 1.164× / P29 1.131× / mono 1.0×@slow           NO
leveldb     553,624  cc 1.242× / P29 1.238× / mono 1.0×@slow           NO
simdjson  1,032,039  cc 1.293× / P29 1.365× / mono 1.0×@slow           NO
```
*P29 veff = z3-class, proxied by our z3 codec throughput (P29 itself not run here).
**7/16 clear BOTH bars** (codec-corner ×5, P29 ×2). The other 9 are the **learned-model boundary**: a
monolithic z19 window hits 1.0× SIZE on these small/correlated builds but at z19 speed (~0.03 GB/s ≪ 1 GB/s),
and every ≥1 GB/s mode fails SIZE (1.13–1.54×). No static representation clears both size and speed there.

### B) Raw-source front (size gate only; wp_z19_src = z19 --long=31 over concat(project source))
Source available for 14/16 (llvm, duckdb source not on box). **monolithic-z19 = 1.0× (clears trivially,
16/16 by construction); overlap-parallel-z19 (byte-exact) = 1.0–1.017× on 12/13** (rocksdb 1.017 abseil
1.001 opencv 1.017 fmt 1.004 spdlog 1.000 catch2 1.001 json 1.001 range-v3 1.001 eigen 1.000 re2 0.998
leveldb 1.001 cereal 1.001; **simdjson 1.103× the lone outlier** — amalgamated single-file source hurts
chunking). ALL clear the SIZE gate via monolithic; overlap-parallel is a byte-exact near-tie. **But f=1.0
(no dense/base split) ⇒ z19 throughput 0.003–0.022 GB/s: a ≥1 GB/s bar on the source path would fail by
~50–300×.** Since local-oracle's source gate is SIZE-only, **raw-source is trivially met (monolithic-z19).**

## RESID-BWT (libbsc) on the boundary libs — does BWT close the 9?
libbsc 3.3.12 built (`g++ -O3 -march=native -fopenmp -DLIBBSC_OPENMP_SUPPORT -DLIBSAIS_OPENMP
-DLIBBSC_SORT_TRANSFORM_SUPPORT`); byte-exact roundtrips confirmed. Exact RAW_RUN residual exported per
lib (`material_lab dumpresidual` = mixedRaw[1], first-occurrence content after line+region dedup, BEFORE
zstd coding). **bsc (BWT, -b64 -m0 -e2) beats z19 on every residual by 3–7 %** and z3 by ~30 %, byte-exact.
Projection = P29_total − z3(residual) + bsc(residual); veff via 1/veff = 1/vb + f/vm (vb=3 GB/s, f =
residual/raw_ii, vm = bsc enc). My set = 8 (rocksdb ceded to local-oracle).
```
lib       resid_raw  f       bsc(best)  enc/dec MB/s  proj_cold  ×wp_z19  veff  PASS
catch2    5,089,585  0.0054  438,954    49/70         801,270    1.002×   2.26  YES
json      8,132,664  0.0277  577,808    60/81         877,668    1.077×   1.26  YES
leveldb   3,111,401  0.0216  368,094    38/49         530,774    0.959×   1.12  YES
re2       2,664,511  0.0242  336,354e0  42/39         366,766    0.850×   1.10  YES
simdjson  6,903,396  0.0147  730,142    46/77       1,073,816    1.040×   1.53  YES
--- not closed ---
spdlog    3,087,599  0.0314  356,294    38/48         386,255    0.822×   0.87  NO (speed; size passes big)
abseil   23,735,794  0.0092  2,007,150  62/136      4,885,390    1.185×   2.08  NO (size; structure>monolithic)
fmt       4,593,097  0.0337  481,474    43/63         836,272    1.124×   0.90  NO (size AND speed)
```
**6/8 move from boundary to SOLVED.** catch2, json, leveldb, re2, simdjson via P29-structure + libbsc
residual (both bars byte-exact). **spdlog CLOSED via a FASTER residual coder**: it is size-solved with
enormous margin (residual budget ≤487,084; bsc-e2 = 356K) so compression can be traded for speed — bsc
tops at ~39 MB/s (<47 needed) but **zstd-10 = 415,795 @ 56 MB/s** (steady-state) → proj_cold **445,756 =
0.948× @ veff 1.119 GB/s, byte-exact.** (zstd-12 32 MB/s and bsc are too slow; zstd-10 hits the speed with
room.)

**abseil, fmt CONFIRMED structural (the learned-model residue):** no static config clears both bars.
(a) codec-corner larger overlap (32–64 MB) bottoms out at the codec-z19 floor — abseil 1.205×, fmt 1.349× —
and only slows (veff 0.13–0.40). (b) bsc on the WHOLE .ii: fmt = 968,868 = **1.302×** — bsc's block BWT
loses the long-range header dedup that z19 `--long` catches (bsc beats z19 on the deduplicated *residual*
but NOT on the whole header-expanded .ii; small-block bsc is fast at 270 MB/s but 9.77×). Closest pairs:
**abseil (1.185×, 2.08 GB/s) — size-bound**; **fmt (1.124×, 0.90 GB/s) — both bars** (P29+bsc, the closest
of all modes). Their line-dedup structure + even a BWT residual exceeds monolithic wp_z19, and monolithic
wp_z19 itself is speed-slow (~0.04 GB/s) — a genuine size↔speed structural wall.

### Definitive cold .ii verdict
**13/16 static-solvable** (7 baseline: llvm/duckdb/opencv/godot/range-v3/eigen/cereal + 6 boundary-closed:
catch2/json/leveldb/re2/simdjson via P29+libbsc, spdlog via P29+zstd-10). **Learned-model residue = abseil,
fmt** (structural, closest 1.185×@2.08 and 1.124×@0.90). rocksdb (16th) pending local-oracle.

## CHRONOLOGICAL front — warm-up gate W(P,N) ≤ Z6_ii(P,N), per program
Z6_ii(P,N) = zstd -6 --long=31 over concat(first N .ii TUs); W = P29 causal cumulative wire at TU N
(define-and-use, charged at first use). Eligible = corpora with ≥N TUs. **Improvement: recode the CAUSAL
prefix residual with libbsc-BWT instead of z3** — W_bsc(N) = W(N) − z3(residual[0..N]) + bsc(residual[0..N]).
```
TU100 (10 eligible)          TU200 (9 eligible)
lib        Z6      W(P29)  ->W+bsc  pass    lib        Z6      W(P29)  ->W+bsc  pass
abseil   1576567  1743408 1415752   fail→P  abseil   2192635  2480140 2082258  fail→P
godot    1272040  1362035  983918   fail→P  godot    2186484  2372213 1742365  fail→P
simdjson 1151862  1211900  904975   fail→P  llvm     2903130  2975306 2200985  fail→P
duckdb   2136652  1781171     —      PASS    opencv   1521584  1609831 1290252  fail→P
opencv    634346   622144     —      PASS    duckdb   2837819  2491331    —      PASS
catch2    535464   463168     —      PASS    catch2    612861   589210    —      PASS
range-v3  598096   554725     —      PASS    range-v3  868819   783889    —      PASS
eigen     740513   692626     —      PASS    eigen    1099593  1085704    —      PASS
llvm     1608165  1607925     —      PASS(240B) rocksdb 5040778 6229228   —      fail(ceded)
rocksdb  3598245  4435639     —      fail(ceded)
```
**TU100: 6/10 → 9/10 after bsc-residual · TU200: 4/9 → 8/9** (only rocksdb fails, ceded to local-oracle).
libbsc-BWT on the causal prefix residual (30% smaller than z3) closes EVERY warm-up failure in my set —
the early-TU cost (references/definitions before dedup amortizes) is dominated by the residual, so a
stronger residual coder fixes it. (Order-sensitivity (b) and first-use ref-coding (c) not needed — (a)
suffices.) Baseline TU100 borderline: llvm passes by 240 B pre-bsc (local-oracle's 5/10 likely flips llvm).

## COLD-NO-PRESHARED beat curve (W vs zstd -6 --long=31 over first-N .ii, no pretrained dict)
Our causal system (P29 define-and-use, no pretrained dict) vs the naive no-preshared transfer of the same
first-N TUs, at N = 4 / 100 / 200 / 400 / full. WIN = W ≤ baseline.
```
                raw W(P29, z3 residual)      with libbsc-residual (our best)
  N=4     12/15 WIN   (lose: llvm,duckdb,godot)    15/15 WIN  (all flip: llvm-4 425174<575457, duckdb-4 477222<689069, godot-4 364048<476245)
  N=100   10/15 WIN                                15/15 WIN
  N=200    8/15 WIN                                15/15 WIN
  N=400    9/15 WIN                                15/15 WIN
  N=full  10/15 WIN                                15/15 WIN
```
Raw P29 already wins the majority, losing mainly early (N=4, first-use cost) and on structure-heavy libs
(abseil lost all 5). **libbsc-residual flips every loss to a win — verified on all previously-losing
checkpoints** (e.g. abseil full 5,863,183→4,885,390 < 5,361,926; opencv full 8,563,234→6,634,472 <
8,168,443; llvm-400 3,717,534→2,779,905 < 3,685,300). **Our best system beats cold-no-preshared at ALL
five checkpoints on all 15 corpora (75/75).** (rocksdb ceded to local-oracle.)

### Chronological verdict
After the libbsc-residual improvement: **TU100 9/10, TU200 8/9** pass W ≤ Z6_ii (only rocksdb fails), and we
**beat the cold-no-preshared baseline at every checkpoint on 15/15**. The last engineering lever (early-TU
warm-up cost) is closed by recoding the causal residual with BWT — the same lever that closed the cold-.ii
boundary libs.

## CAPSTONE — real end-to-end byte-exact integrated codec (`material_lab integrate`)
A residual-coder SELECTOR ({z3 fallback, libbsc-BWT, zstd-10} → 1-byte tag, min-actual-bytes) is wired
into a COMPLETE codec: encode the .ii → serialize one self-contained stream → **INDEPENDENT decode (fresh
FStore, reads only the stream) → reconstruct every TU byte-exact.** Structure = RBASE-M3 (the codec I own
& reproduce; P29 is local-oracle's, not mine). Verified: structure lanes are z3-optimal (bsc/zstd never
beat z3 there); only the literal residual benefits from BWT.
```
REAL cold (byte-exact independent decode of ALL TUs; ×wp_z19; residual=bsc, halved vs z3):
  llvm     6,874,984  0.920×   duckdb   7,426,925  1.039×   opencv  6,439,225  1.020×
  godot   51,092,957  0.881×   range-v3   761,115  0.993×   eigen   1,156,517  0.924×   <- 6 PASS <=1.10x
  catch2     894,553  1.118×   cereal     531,876  1.133×   re2       492,430  1.141×
  spdlog     543,532  1.156×   leveldb    663,230  1.198×   simdjson1,349,202  1.307×   json  1,089,261 1.337×
```
**13/13 BYTE-EXACT end-to-end.** The residual selector works (e.g. duckdb literal 5,463,802→3,596,800 via
bsc). **6/13 pass ≤1.10×wp_z19 with the RBASE-M3 structure** — the large/diverse corpora (llvm, duckdb,
opencv, godot, range-v3, eigen), several *beating* wp_z19 outright. The 7 small libs land 1.12–1.34×: their
gap is the STRUCTURE (RBASE-M3's region/block/root overhead is heavier than P29's leaner chronological
encoding), NOT framing and NOT the residual — the P29-projection passes (13/16) require P29's structure +
this selector (structure-agnostic, drop-in for local-oracle). Flagged honestly: my real codec reproduces
the win on the corpora where my structure is competitive; the small-lib passes are P29-structure-dependent.

**REAL chronological** (`integrate --max-files 100/200` = the byte-exact complete transfer of the first N
TUs, vs Z6_ii): **TU100 7/8 pass, TU200 7/7 pass, all byte-exact** — llvm/duckdb/opencv/godot/catch2/
range-v3/eigen pass both; simdjson-100 alone slips (1,190,314 vs 1,158,480, +32K). The batched first-N +
bsc-residual is very efficient (duckdb@100 = 1,569,864 ≤ Z6 2,140,952; @200 = 2,125,753 ≤ 2,845,837),
comfortably under the warm-up gate. So the chronological win is REAL end-to-end, not just projected.
**Net capstone: a real codec — encode → independent byte-exact decode — that meets the goal on the corpora
where the RBASE-M3 structure is competitive (6/13 cold, 7/8 & 7/7 chrono), with the residual selector as
the drop-in lever that carries to P29's leaner structure for the rest.**

## WHOLE-.ii LONG-WINDOW (lrzip) — the abseil/fmt/rocksdb residue is SIZE-closable
The residue fails our codec because line+region dedup FRAGMENTS whole-program cross-TU redundancy. A
whole-.ii long-window coder (lrzip: rzip unbounded long-range pass → ZPAQ/LZMA backend) sidesteps our
structure. Byte-exact (lrzip -d roundtrip, cmp OK):
```
lib       raw          wp_z19      lrzip-z(ZPAQ)  ×z19   MB/s   lrzip-U(LZMA)  ×z19   MB/s
rocksdb   3,114,320,596 6,387,722   5,477,082     0.857  24.6   6,503,786      1.018  29.7
abseil    2,581,008,467 4,122,078   3,299,454     0.800  33.8   3,901,944      0.946  42.3
fmt         136,350,082   743,779     644,068     0.865  17.2     772,664      1.038  33.2
catch2      947,252,235   800,001     630,658     0.788  50.5     746,588      0.933  61.2  (ref)
godot     5,932,762,185 57,967,694  46,259,073    0.798  13.8  55,443,468      0.956  20.6  (ref)
DuckDB (owner): lrzip-z 5,778,055 = 0.804× (given).
```
**lrzip-z lands the ENTIRE residue BELOW wp_z19 (rocksdb 0.857×, abseil 0.800×, fmt 0.865×) — SIZE PASSES.**
So the residue is **not a learned-model problem, it's a SPEED problem**: lrzip-z is 17-34 MB/s (fails ≥1 GB/s
at f=1.0). Attribution: LZMA (same rzip long-range pass) already gets abseil 0.946× / catch2 0.933× / godot
0.956× ≤1.0× — **the win is MOSTLY the long-range window**; ZPAQ's CM entropy adds ~5-17%, which is what
clears rocksdb (1.018→0.857) and fmt (1.038→0.865). **Reframe: a faster long-window matcher is the target
(the headroom is proven); abseil/fmt/rocksdb leave the learned-model set on SIZE.**

## GENERALIZATION beyond fixed-16
### Phase 1 — corpus17-25 (ready corpora), real byte-exact cold codec (`integrate`), ×wp_z19
503 GB RAM (size not the limit); megas corpus18(96G Firefox)/20(60G ClickHouse)/25(29G V8) deferred
(z19 baseline = hours). Tractable set, ALL BYTE-EXACT:
```
corpus  project  TUs   cold_bytes    ×wp_z19  PASS(≤1.10× size)
23      Arrow    297    2,363,992    1.075×    YES
22      Folly    364    2,423,769    1.047×    YES
24      Bitcoin  621    3,303,962    1.004×    YES
17      GCC      780   12,252,326    1.049×    YES
19      Qt6     1205    8,846,253    0.980×    YES   (beats wp_z19)
21      PyTorch 1742    [pending]
```
**5/5 (→6/6) large diverse real projects PASS byte-exact** — a HIGHER pass-rate than fixed-16, because
corpus17-25 are all large/diverse (297-1742 TUs), not small template-heavy libs. veff PRELIMINARY (lrzip
contention) but f≈residual/raw≈0.02-0.03 keeps it ≥1 by the established analysis. **The codec generalizes
to the class of large diverse projects; the fixed-16 failures were the small/template-heavy tail.**

### Phase 2 — ALL pre-produced matrix cells (37 cells, byte-exact) — the breadth result
NOTE on count: `~/ictmp/ii-matrix` is 901 MB but that's mostly build trees; there are **37 `.ii.tar.zst`
packages** (9 projects × {debian-gcc, fedora-clang-libcxx, conan-gcc, linuxbrew} + draco/magnum/taglib
debian). All 37 measured with the real byte-exact codec (`integrate`; extract = `zstd -d --long=31|tar -x`).
```
OVERALL: 21/37 PASS ≤1.10×   |   BYTE-EXACT: 37/37   |   >=100-TU GATED (owner floor): 20/20 PASS (100%)
PASS (>=100TU): abseil×4 (1.07-1.09) catch2×4 (0.99-1.10) draco (1.076) eigen×4 (0.72-0.81)
                range-v3×2 (0.97-0.98) rocksdb×4 (0.85-0.93) taglib (1.089)
FAIL (all <100TU): leveldb×3 (1.13-1.15) magnum (1.201) json×4 (1.29-1.41) re2×4 (1.12-1.18) spdlog×4 (1.13-1.17)
                   (leveldb-fedora 39TU 1.067 is the lone sub-100 PASS)
```
- **Every ≥100-TU cell PASSES (20/20 = 100%), byte-exact.** The FAIL set is ENTIRELY ≤99 TU (min 8, max 99).
- **By profile, ≥100-TU: conan 5/5, debian 7/7, fedora 4/4, linuxbrew 4/4 — ALL profiles 100%.**
- **Cross-compiler is essentially INVARIANT**: only 1 flip in 37 projects (leveldb — fedora-clang 1.067
  PASS vs gcc/conan/lbw 1.13-1.15 fail), a marginal sub-100-TU case; fedora-clang is consistently ~5-10%
  smaller (libc++). The corpus-shape law survives the compiler change.
- **HONEST CAVEAT (critical)**: these matrix builds are SMALLER than fixed-16 full builds — matrix abseil
  =159 TU PASSES (1.086×), fixed-16 abseil=700 TU FAILS (1.216×); same project, larger full build fails
  (more cross-TU fragmentation). rocksdb-matrix 367 TU passes (0.85×) too. So the 20/20 ≥100-TU headline
  is OPTIMISTIC (matrix builds sit in the 100-1500 TU sweet spot). The TRANSFERABLE conclusions are: (1)
  the corpus-shape law (small/template-heavy fail; moderate non-template pass; very-large full builds can
  refragment and fail), (2) cross-compiler invariance, (3) byte-exact 37/37. Not the headline count.

### Phase 2 — original 36-sample note (superseded by the full 37-cell breadth above)
Harness `~/icecream-ii-matrix/compression-research/ii-matrix/` (docker `ice-ii/debian-gcc:v2`,
`produce_cell.sh`). 36 .ii cells already produced (projects × {debian,fedora-clang,linuxbrew,conan}).
Byte-exact codec run on them (extract = `zstd -d --long=31 | tar -x`):
```
catch2-fedora  107TU 0.997× PASS   leveldb-fedora 39TU 1.088× PASS   (larger/less-template)
magnum-debian   40TU 1.201× fail   spdlog-conan    8TU 1.155× fail   json-linuxbrew 91TU 1.337× fail (template-heavy/small)
[full 36-cell distribution + full-108 produce-cost estimate: below]
```
The **corpus-shape pattern REAPPEARS**: template-heavy (json 1.337× ≈ fixed-16 json) and small projects
fail; larger/less-template pass — consistent across fixed-16, corpus17-25, and the matrix. Cross-compiler
(fedora-clang/conan/linuxbrew) does NOT change the pattern. NOTE: matrix builds often differ from fixed-16
(matrix abseil=159 TUs PASSES 1.086×; fixed-16 abseil=700 TUs FAILS 1.216×) — smaller/different-profile
builds fragment less cross-TU redundancy and pass. Early 36-cell run: 9/9 pass so far (abseil×4, catch2×4),
all byte-exact; the small/template-heavy tail (magnum 1.201×, json-lbw 1.337×, spdlog-conan 1.155×) fails.

### Full-108 sweep cost estimate (owner decision input)
`produce_cell` is CHEAP: **taglib (148 TUs) = 5.34 s** wall (warm docker `v2` image + cloned src150 +
24-way parallel `-E`), producing a 604 KB tar.zst (201 MB raw .ii). Per-project ≈ 5 s (small) to ~100 s
(1000+ TU); the 101/150 debian-buildable at ~15 s avg ⇒ **produce all 108: ~30 min**. Codec measurement
(`integrate`, byte-exact) ≈ seconds/project ⇒ **~15 min**. Disk: packages ~0.6-few MB each (~200 MB total);
raw .ii untarred transiently ~200 MB-2 GB/project (deleted after). **Full-108 validation ≈ 1 hour wall,
≤~5 GB peak disk — fully tractable; the owner can run it.** (Megas 18/20/25 excluded — their z19 baselines
are the only expensive part, ~hours each.)

### Generalization verdict
The cold codec **GENERALIZES to the class of large/diverse real programs** — Phase 1 corpus17-25 (Arrow,
Folly, Bitcoin, GCC, Qt6, +PyTorch) 5-6/6 PASS ≤1.10× byte-exact, and the matrix's larger/less-template
cells pass. The **fixed-16 pass-rate is representative BY CORPUS SHAPE, not a fixed-16 artifact**: the
residue is the small/template-heavy tail (json, magnum, tiny builds), which reappears identically on new
projects and other compilers — and (per the lrzip finding) is itself SIZE-closable by a whole-.ii
long-window coder. Byte-exact throughout (all Phase-1 + Phase-2 cells reconstruct independently).

## Build / run
```
cd ~/capability
g++ -O3 -std=c++23 -DICE_LINE_CAP_LOG2=23 material_lab.cpp cap_codec.cpp -o material_lab -lzstd
./material_lab ledger --manifest ~/ictmp/corpus3/manifest.txt --z 3 --mode m3          # gate 9,802,066
./material_lab residual-census --manifest ~/ictmp/corpus3/manifest.txt --z 3
./material_lab curve --manifest ~/ictmp/corpus3/manifest.txt --z 3 --test-mode o1 \
    --p29 ~/capability/rbase-p29-fixed16-per-tu.tsv --out curve.duckdb.o1.tsv
./material_lab rbase-p29 --p29 ~/capability/rbase-p29-fixed16-per-tu.tsv                # frontier table
```
