# Protocol-50 Productization Plan — S1→S8 (executable, hyper-detailed)

**Revision: v5 — deep-reviewer 4-round consensus** (R1 gate quality: control mutations, RocksDB-median perf budget, pinned-host encode gate, cold/warm separated in S8, deterministic golden capture; R2 integration reality: stacked-PR re-gating, canonical-P29-SHA confirmation, S4 freeze-at-start; R3 risk: libbsc vendoring decision, S6 rollback, S8 LLVM-live dependency, S2-exit profile-interface freeze; R4 goal-alignment: verified complete vs the owner's goal, no blocking findings). The GOAL section is verbatim-unchanged across all revisions.

## GOAL (invariant — every revision must preserve this verbatim)

A **DEPLOYED, functional, backwards-compatibility-tested, verified Protocol-50 cache transport** running on the 4-host farm (q3 + research6 + research7 + q2), with **IMPLEMENTED profiles `P29`, `GRZ`+RESIDUAL, and `ZSTD_ROUTE`** (`ZSTD_COHORT` deferred to later testing), followed by **S8: per-`.ii`-corpus isolated SIMULATOR benchmark + LIVE farm run + sim-vs-live comparison** for each profile. Byte-exactness is the non-negotiable gate at every step.

## §0 Branches — what we work on, what we integrate

| Role | Branch | State (verified 2026-08-23) |
|---|---|---|
| **WORK TRUNK** | **`deep-reviewer/p50-product`** — cut from `bigoracle/issue16-p50-fill-closure` @ `8c8cb881` | M1-accepted base: full icecream tree (proto-44 wire), `cache/` M0/M1 landed (`protocol50.{h,cpp}`, `p50_actions.*`, `p50_slice0.*`, `formal/Protocol50.tla`, wire/slice0/fill-closure unittests, key-layout census) |
| INTEGRATE 1 | `bigoracle/issue16-p50-m2-loopback-endpoint` @ `838ae31b` (PR **#22**) | M2 staged C1F1 `ZSTD_TU` loopback transport. I HELD its R2 (`c5575dc8`); head has 2 newer commits ("restore self-contained route cursor mutation", "live Zstd streaming window bound gate") that appear to address the HOLD — **entry check required** |
| INTEGRATE 2 | `bigoracle/issue16-p50-input-record` (PR **#23**) | exact-input retention for compiler restart + late-route lease closure |
| INTEGRATE 3 | `bigoracle/issue16-p50-reset-ack-continuity` (PR **#24**) | initial HISTORY_RESET ack bound to staged session |
| INTEGRATE 4 | `bigoracle/issue16-r6-input-seam-hardening` (PR **#30**) | client/daemon R6 input seam ownership |
| IMPORT (research→product) | `implementer/issue16-capability`: `capability/codec50-m1.cpp`, `capability/grouprlz/*`, `capability/distribution/build_{p29,grz}_ledger.py`; `implementer/issue16-superblock`: `linecache/codec50.cpp`; `implementer/issue16-fused-harness`: interner/fused/fcache/ipc2 sources | the measured P29 (`CW_P29_BSC_Z3_M64`), GRZ2 G0/G1/G2, residual-BWT, cohort-dict bench |
| **UNIFICATION TARGET (S4)** | `fix/p50-zero-wire-claim` @ `b5b76761` (deployed farm lineage) | assignment-identity P50 + 2483 fork commits. **DISJOINT ROOT vs codec base (no merge-base)** — port is a seam-patch/shim exercise, NOT a git merge |
| S4 RESULT | **`unified/p50-cache-fork`** | the deployable product tree |
| Reference docs | `implementer/issue16-capability`: `capability/distribution/PROTOCOL50-FULL-IMPLEMENTATION-PLAN.md` (frozen, 1424 lines), `ICECREAM-REAL-TRANSFER-ARCHITECTURE.md`, `capability/GUID_ROUTING_RESIDENCY.md` | design of record + BigOracle rulings |

**Build/test loop (proven):** rsync source → q3 `~/phaseb-build` → build in pinned container `icecream/farm-node:ubuntu22-gcc11-boost174` → `make check` + byte-exact farm gate (`bt.sh`, green on baseline). Deployment via `farm.py` (proven C1F1→C1F3).

## S1 — Merge M2: C1F1 `ZSTD_TU` loopback (artificial cluster)

**Objective:** the landed in-memory M1 state machine drives a real framed loopback endpoint pair carrying whole-TU `ZSTD_TU` payloads, C1F1, inside `tests/test.sh`.

**Work items**
1. Re-review PR #22 head `838ae31b` against my R2 HOLD: (a) the completion-identity clauses that survived `if(false)` neutralization must now DIE under the same mutation; (b) the live Zstd streaming window bound must be a live comparison, not a constant. Run the full build+mutate suite in the container.
2. Merge #22 into `deep-reviewer/p50-product`.
3. Wire the loopback pair into `tests/test.sh` (one scheduler, one submit daemon, two workers, private ports): replay every M1 stream/object boundary; compare reconstructed input bytes.
4. Exercise: cold, warm, lost-final-commit, route reset, F-store reset, process replacement. Retain endpoint action traces + daemon logs.
5. Re-run `cache/formal` trace gates (`check_trace.py`) + key-layout census.

**Gates (each must be able to FAIL):** mutation suite kills every completion-identity clause **and includes a known-caught control mutation proving the harness itself is live** (a gate that cannot fail is not a gate); byte-exact reconstruction on all 6 scenarios; trace replay green; census green.
**Exit:** #22 merged; loopback C1F1 green in artificial cluster.
**Fallbacks:** R3 still vacuous → I implement the live-state comparison on the trunk myself (no further review bounce). Loopback flaky under cluster timing → run endpoints under the unittest harness first; cluster wiring slides into S2.

## S2 — Product seams: sidecar, real sockets, async, InputRecord, reconnect

**Objective:** the cache roles live in the real daemons: dedicated cache endpoint, async I/O, atomic input, restart safety.

**Work items**
1. **Dependency preflight (½ day, blocking):** container has xxHash ≥0.8.0, zstd ≥1.4 (refPrefix/LDM), vendored header-only Asio in-tree, libbsc — **decision: vendor libbsc source in-tree (Apache-2.0) behind `--with-libbsc`, system lib as fallback** — verify ALL build in the container now, not at S7.
2. Advertise the dedicated cache endpoint in `Login`/`UseCS` (per BigOracle ruling: dedicated port, NOT same-daemon-port fd handoff).
3. Host sidecar C-role/F-role as a thread inside `iceccd` behind AF_UNIX (`NamespaceRuntime` interface keeps process-split reversible).
4. Async model per relationship: framed reader + single writer (Asio), bounded priority queues, one outstanding write, codec CPU on a bounded worker pool, writer priority ladder (Fill/ack > DICT_END continuation > BODY DRR > maintenance).
5. Integrate PR #23 (InputRecord), #24 (reset-ack), #30 (input seam) — in that order, each after its own build+mutation gate. **Stacked-integration discipline: #24 builds on #22's session staging and #30 touches the S4 seam files — after EACH integration, re-run the previously-merged layers' gate suites (deletion-aware: an upper layer can silently dead-code a lower layer's guard — known rebase-regression failure mode).**
6. Reconnect cursor + 4-case replay (A exact / B lost-final-commit / C F-store gone / D history-mismatch → HistoryIndependent re-warm).
7. Mid-step demo gate: two REAL daemons exchange one TU end-to-end over the cache channel (catch integration drift early).

**Gates:** real C1F1 on real sockets byte-exact; `kill -9` each role mid-TU across seeded runs → byte-exact + no wedge + correct A–D case counters; ≥1000-TU soak; ASan clean (`-static-libasan` for the scheduler shim per known recipe); formal model updated for any new action + trace gates green.
**Exit:** cache path works daemon-to-daemon on one host with restart safety; **the profile interface (encode/decode vtable seen by the transaction engine) is FROZEN at S2 exit** — S7 profile work may then proceed in parallel against a stable seam and port cleanly onto the unified tree.
**Fallbacks:** gcc11 C++23 coroutine gaps → Asio stackless callbacks (same state machines, no `co_await`). Event-loop contention in-daemon → split sidecar to its own process behind the same AF_UNIX (reversible by design). io_uring explicitly OUT of scope.

## S3 — Persistence + eviction

**Objective:** bounded-memory long-running caches with correct namespace lifecycle.

**Work items:** immutable append arena per (C_GUID, generation); F object states ABSENT→INSTALLING→PRESENT→PINNED; byte-cap **LRU whole-namespace** eviction (never partial); retention ≈3h C-generation read-only + lease drain, ≈2h F idle; generation-field wrap → stop admissions + C_GUID flip (no roll handshake — per ruling).

**Gates:** eviction storm under load stays byte-exact (a later Root re-Needs evicted objects); forced wrap (tiny generation space build) → GUID flip observed, zero cross-namespace reuse; Key64 same-key-different-content injection → fatal namespace fault (never silent); RSS cap held over ≥10k-TU soak; crash mid-INSTALLING → idempotent re-FILL.
**Exit:** week-long-run memory profile bounded; lifecycle proven.
**Fallbacks:** on-disk persistence DEFERRED (RAM arena + degrade-to-legacy on daemon restart is acceptable v1 — re-warm cost measured small); ARC dropped for plain LRU byte cap (ARC never required by ruling).

## S4 — UNIFICATION onto the fork lineage

**Objective:** ONE deployable tree containing the fork's deployed behavior (assignment-identity P50, operator requirements) + the cache lane.

**Work items**
1. **Seam inventory FIRST:** diff the 4 seam files between lineages (`services/comm.{h,cpp}`, `daemon/main.cpp`, `client/remote.cpp`, + build files) before choosing mechanics.
2. Port `cache/` (additive dir) + unittests + the seam patches onto `fix/p50-zero-wire-claim` → `unified/p50-cache-fork`.
3. Freeze the fork head **as it stands at S4 start** (NOT today's `b5b76761` — the farm lane may land fixes during S1–S3); single rebase at S4 end if it moved again.

**Gates:** unified tree builds in container; **both** suites green — `p50_*` unittests AND fork suites (`remoteice-quick.sh`, p49daemon, assignment-fence tests) AND the farm C1F1 byte-exact gate on the unified binary; legacy-path golden: message-level wire byte stream identical to pre-cache build on a captured reference run.
**Exit:** `unified/p50-cache-fork` is the single product branch.
**Fallbacks:** seam conflicts explode → vendor `cache/` as a subtree + minimal 4-file seam shim. Reverse direction (fork→codec-base) REJECTED: 2483 commits incl. operator requirements (nice-19, BBR) — replay risk too high.

## S5 — Backwards-compatibility matrix

**Objective:** prove the owner's hard requirement: old fleets keep working, byte-exact, with the C-cache **provably not engaged** on legacy jobs.

**Cells (artificial cluster via `OTHERVERSIONPREFIX` + farm):**
| C | F | Expect |
|---|---|---|
| new | new (cache on) | cache path, byte-exact |
| new | old P43 / P44 / identity-50-only | **legacy path, C-cache untouched (engagement counter == 0)**, byte-exact |
| old | new | legacy path, byte-exact |
| new | new (endpoint disabled) | legacy path, byte-exact |
| new | mid-build downgrade (kill new F; old F takes the retry) | one-attempt-one-mode holds; retry legacy; byte-exact |

**Gates:** every cell byte-exact; legacy wire (message level) byte-identical to today's build — **captured in the deterministic artificial cluster (golden run), NOT farm pcaps** (TCP segmentation is nondeterministic; farm cells assert byte-exact objects only); scheduler mixed-version matrices (P43/P48 suites) re-run green. Env tarballs are explicitly OUT of scope (ruling: environments are not P50 objects; `EnvTransferMsg` unchanged) — considered, not forgotten.
**Exit:** compat matrix green.
**Fallbacks:** none — any cell failure is a release blocker by definition (this step exists to fail loudly).

## S6 — DEPLOY = the baseline

**Objective:** the unified binary on all 4 farm hosts; cache mode functional with `ZSTD_TU`; this is the DEPLOYED FUNCTIONAL P50 baseline.

**Work items:** container build of `unified/p50-cache-fork`; distribute tree via hub; `farm.py` up/test/down; run acceptance in LEGACY mode first (regression vs today), then `mode=cache profile=ZSTD_TU`; export the ledger per route (`Wire = CtoF{Root,Fill,CControl} + FtoC{Need,FControl}`); include one OLD-binary F in the live farm for a real mixed-fleet cell; fix the research7 0-jobs cold-start observation (warm check).

**Gates:** fmt 51/51 + RocksDB 367/367 byte-exact in BOTH modes; jobs/s(cache) ≥ 0.9× LEGACY **measured on the RocksDB warm rebuild, N=3 fixed-order repeats, median (fmt is too small/noisy to score a 10% budget)**; `ZSTD_TU` wire within ±5% of LEGACY (parity class — the wins come at S7); mixed-fleet live cell byte-exact; ledger sums reconcile with interface counters.
**Exit:** **DEPLOYED functional P50** — the baseline to improve.
**Rollback:** the previous product tree (`c9488d74`+`b5b76761`) is retained side-by-side on every host; rollback = flip the container bind path back — one `farm.py` flag, no rebuild.
**Fallbacks:** perf regression → cache ships opt-in (deployed, default legacy) while S7/S8 proceed; still satisfies the baseline definition.

## S7 — Implement the profiles: `ZSTD_ROUTE`, `P29`, `GRZ`+RESIDUAL

**Profile framing (design-consistent):** a profile defines how DICT/BODY/NEED/FILL are populated for a PreparedTU on a negotiated relationship. `P29`/`GRZ` = dictionary profiles (objects + Need/Fill live). `ZSTD_TU` = BODY-only, stateless. **`ZSTD_ROUTE` = BODY-only, STREAMING**: one zstd stream context per relationship, window carried across TUs (windowLog≈27); restart cases C/D reset the stream (HistoryIndependent until re-warm); NO Need/Fill. Enum: add `ZSTD_ROUTE=4`; reserve `ZSTD_COHORT=5` (deferred).

**Work items (order: smallest risk first)**
- **7a `ZSTD_ROUTE`:** streaming contexts keyed by relationship; stream state summarized in the route cursor digest; decoder symmetric window.
- **7b `P29`:** port `CW_P29_BSC_Z3_M64` (`capability/codec50-m1.cpp`, `linecache/codec50.cpp`) into `cache/profiles/p29.*`. Stage 1: HistoryIndependent (per-TU, already beats streaming per census). Stage 2: RouteHistory using the landed route learner.
- **7c `GRZ`+RESIDUAL:** port `capability/grouprlz/` G0/G1/G2 (causal, bounded-memory) into `cache/profiles/grz.*`; residual = libbsc BWT-on-residual behind `--with-libbsc`; absent libbsc ⇒ GRZ not advertised (negotiation falls back).
- Inventory the exact research sources at 7-start (paths verified per branch before porting) — **and local-oracle CONFIRMS the canonical `CW_P29_BSC_Z3_M64` implementation SHA + test vectors before the port begins** (the serializer lane is still moving: OnlineS1/ROUTE_S1 in flight; porting a stale copy is the known failure mode).

**Gates per profile:** offline corpus replay — every `.ii` in corpus1/2/3 encode→decode byte-exact with digest; live C1F1 byte-exact; corrupt-object injection → fatal namespace fault; per-core encode ≥ ~100 MB/s (link-saturation rule) **on pinned-core corpus replay on the deployed C-host class (q3, EPYC 8124P) — cold and warm reported separately**; negotiation matrix (profile unsupported ⇒ clean fallback to `ZSTD_TU`).
**Exit:** four selectable, negotiated, byte-exact profiles in the product.
**Fallbacks:** GRZ residual trouble → GRZ-no-residual first (flag in profile params, same enum). P29 RouteHistory instability → ship HistoryIndependent. `ZSTD_ROUTE` RAM (window × routes) → clamp windowLog by route budget (24 measured acceptable).

## S8 — Per-corpus SIMULATOR benchmark vs LIVE runs

**Objective:** benchmark each `.ii` set in isolation via a simulator built from the PRODUCT profile code, then run live farm builds, and COMPARE — validating both the model and the product.

**Simulator:** `cache/sim/p50sim` links the SAME profile/codec objects as the product (no reimplementation). Input: a corpus manifest (`/tanksmall/scratch/ictmp/corpus{N}/manifest.txt`, mirrors on quietbox2 `~/corpus*`), replayed chronologically with a route split (reuse `CF-SPLIT-30-SLOT`/`DENSE-AFFINITY` splits). Output: predicted ledger per profile per corpus — CtoF{Root,Fill,CControl}, FtoC{Need,FControl}, encode/decode CPU, cold vs warm.

**Live:** farm runs of the matching real projects with each profile selected; product ledger exported per route.

**Comparison gates:** |sim − live| / live ≤ **10%** aggregate wire per (corpus × profile), **evaluated SEPARATELY for the cold (first-build) pass and the warm (rebuild) pass** — mixing them lets one-time def transfer mask steady-state drift; ≤ **20%** per stage (Root/Need/Fill); CPU advisory (report only). Divergence → per-stage localization via the ledger split → fix sim or product → re-run.

**Matrix v1:** sim = {corpus1 LLVM-1238, corpus2 RocksDB, corpus3 DuckDB, fmt} × {ZSTD_TU, ZSTD_ROUTE, P29, GRZ+res}; live = {fmt, RocksDB} × all four + LLVM × best two — **the live-LLVM leg depends on the farm BIG cell (parallel farm lane); S8 v1 ships complete on fmt+RocksDB live if LLVM-live is not yet ready (non-blocking)**. Giants (llvmfull, firefox) sim-first. `ZSTD_COHORT` enters here later, sim-first.
**Deliverable:** comparison table published (Artifact + #16) + a per-regime profile recommendation (cold/warm × corpus size) + proposed default.

## §TLA — formal conformance mode (cross-cutting, every step S1→S7)

**Coverage today (verified from `cache/formal/`):** `Protocol50.tla` is a bounded model of the ccache↔fcache transaction core — 1 C relationship × 2 F stores × 2 TUs × 2 keys/contents × 2 session tokens (`MaxRel=2`); invariants `TypeOK, OneActive, SessionFence, InstalledContentExact, NeedIsExact, CommitOnlyAfterExactMaterialization, AtMostOneAhead`. Covers token fencing + stale callback, route reset, overlays, disconnect/replay, durable-F/lost-C-receipt. Abstracts compression/timing/env/scheduler/P29-parsing. Does NOT yet claim pin/evict or cache-vs-legacy arbitration.

**P50_TRACE conformance mode (owner requirement — "no transitions inconsistent with the TLA model"):**
1. **Runtime flag** (`ICECC_P50_TRACE=<path>`, off by default): the real sidecars emit the canonical JSONL action stream (15-action vocabulary, actor+action, keyed (C_STORE_GUID,F_STORE_GUID)) on the audit pipe (whose bytes never enter the scored ledger, per `GUID_ROUTING_RESIDENCY.md`).
2. **Level-1 gate (every CI/farm run):** `check_trace.py` — vocabulary + core ordering + Need-set/Key64-digest state. An action the model doesn't know **fails the run** ⇒ unmodeled product transitions are impossible to ship silently.
3. **Level-2 gate (per S-step, on the step's scenario traces incl. every crash/restart case):** TLC **trace refinement** — new `cache/formal/trace_to_tla.py` renders a captured JSONL trace as a TraceSpec module constraining the next-state relation to the observed sequence; TLC (parity jar `tla2tools.jar` 1.7.4, known sha) verifies the behavior is admitted by `Spec` and satisfies all invariants.
4. **Model-first rule:** any NEW product action (S3 eviction/pin, S5/S6 cache-vs-legacy arbitration) enters `Protocol50.tla` + the trace vocabulary BEFORE the product code lands; its S-gate includes re-running TLC on the extended model. S3 explicitly discharges the "pin/evict … enter the model with the eviction slice" debt recorded in `cache/formal/README.md`.
5. Farm-scale note: the bounded model checks the per-relationship core; farm runs conform via per-(C,F) trace checking — every relationship's trace must independently refine the model.

## Review protocol & risks

- Every gate is reproduced first-hand by deep-reviewer before any PASS is posted; bigoracle receives exact-SHA review requests per step; local-oracle rules on design deltas. (House rule: no relayed PASS without reproduction.)
- **No merges to `main` by the swarm — the owner lands everything.** Work branches + review verdicts only.
- **Critical path:** S1→S2→S3→S4→S5→S6 serial. S7a/b/c can start after S2 (they depend on seams, not persistence/unification) but their LIVE gates wait for S6. S8 sim legs start as soon as S7 profile code exists; live legs wait for S6+S7.
- **Schedule risk = S2** (async + seams): mitigated by the mid-step one-TU demo gate.
- Single-host risk: q3 is build+sched+client → research6 is the backup build host (image present).
