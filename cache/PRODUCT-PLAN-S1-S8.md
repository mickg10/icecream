# Protocol-50 Productization Plan — S0→S8 (executable, hyper-detailed)

**Revision: v7 — narrow successor to v6 per local-oracle rereview (HOLD on `e7707864`, #16 2026-08-23T15:43Z; v6 structure ACCEPTED).** The five v7 deltas: (1) S2 endpoint discovery = Login-only advertisement + **assignment-bound S→C handoff** (P50 UseCS-carrier extension bound to epoch/wire-id/nonce + selected-F identity; scheduler reads Login snapshot only post-selection; full mutation-gate set); (2) S2/M3 = the **frozen InputRecord→compiler ownership contract** (ATTEMPT_ID never in cache identity; one input-ready per Open commit; cursor/close/reclaim/cancel semantics; deletion-tested); (3) S4 = **blocking role-artifact step** (exact P43 from tag `1.4`/`cd74801e`, exact P50 trunk, per-role runtime roots/images, selection-changes-launched-binary deletion test) + the **default-caret companion cell** per the caret ruling; (4) new **S1b release identity 1.5.90** (configure.ac + installed identity + 3-distro probes); (5) S8 = **full cross-product** (every retained corpus × every implemented profile, sim+live+comparison, staged but complete — honoring the immutable GOAL).

*(v6 was: restructured per the first THOROUGH REVIEW — unification-first onto `a862`, frozen registry, 8-cell matrix, supervised sidecar, transactional streaming state, exact/predictive split, bounded TLA claims, provenance.)* Changes vs v5: unification moved FIRST (S0/S1, trunk = the `a862` convergence — not a late S4 port off the stale proto-44 root); profile-ID registry frozen S0 (4/5 = reserved `z3_long`/`z3_shared_long`, never reused); eight-cell S/C/F compatibility matrix (scheduler included); Login-only advertisement + ONE supervised-sidecar ownership model; `ZSTD_ROUTE` gets a committed/tentative context state machine (a digest is not state); farm topology corrected (C1F4 nas642-C, C1F3 q2-C, slots ≠ Fs, slow-client cells); S6 measurement hardening (manifests, prep-outside-window, counterbalanced repeats, exact per-direction ledgers); S8 split into exact conformance replay (zero tolerance) vs predictive simulation (calibrated); TLA claims weakened to what the gates actually establish + emitter census/mutation/witness gates + global model precondition; vendor provenance pinning. Prior v5 4-round deep-reviewer changelog retained in git history.

## GOAL (invariant — every revision must preserve this verbatim)

A **DEPLOYED, functional, backwards-compatibility-tested, verified Protocol-50 cache transport** running on the 4-host farm (q3 + research6 + research7 + q2), with **IMPLEMENTED profiles `P29`, `GRZ`+RESIDUAL, and `ZSTD_ROUTE`** (`ZSTD_COHORT` deferred to later testing), followed by **S8: per-`.ii`-corpus isolated SIMULATOR benchmark + LIVE farm run + sim-vs-live comparison** for each profile. Byte-exactness is the non-negotiable gate at every step.

*Registry note (S0, non-normative to the GOAL): the owner's `ZSTD_ROUTE`/`ZSTD_COHORT` semantics are reconciled against the already-frozen simple profiles `z3_long=4`/`z3_shared_long=5` at S0 — same-semantics ⇒ the frozen name/ID carries the requirement; different ⇒ new IDs ≥6. Either way the GOAL's three profiles ship implemented.*

## §0 Branches & authority — what we work on, what we integrate

| Role | Branch / commit | Authority |
|---|---|---|
| **PRODUCT TRUNK (from S0 on)** | `provisional/issue16-foundations-convergence-b5` @ `a86285e486d1` (**pending external acceptance = S0.1**) plus the accepted advertisement child/successor lineage (`43297d5352…` inert Login advertisement, exact child of `a862`) | Contains the accepted P49/P50 assignment lineage, zero-wire fix, InputRecord + R6 seams, C++23/package corrections, identity formal matrix. `b5b76761` is a constituent (stale subset — NOT a target) |
| ASSET SOURCE (cache lane) | `bigoracle/issue16-p50-fill-closure` @ `8c8cb881` (proto-44 root, M0/M1 `cache/*`) | **Source material only — no branch merges.** Missing M0/M1/M2/reset assets are transplanted by exact commit/blob with an **authority table** (blob sha → origin commit → destination) |
| ASSET SOURCE (M2/reset) | PR #22 head `838ae31b`, PR #23, PR #24 head `22b8b721`, PR #30; accepted endpoint source lineage `e1e87980` | Diverged histories — **PR-head merges are NOT authority** (local-oracle §1). Extract accepted assets per the authority table; every extraction re-runs the lower layers' deletion gates |
| RESEARCH IMPORTS (S6) | `implementer/issue16-capability` (`capability/grouprlz/*`, `codec50-m1.cpp`, ledgers), `implementer/issue16-superblock` (`linecache/codec50.cpp`), fused-harness | Canonical `CW_P29_BSC_Z3_M64` SHA + test vectors confirmed by local-oracle before port |
| PLAN BRANCH | `deep-reviewer/p50-product-plan` (this file, `cache/PRODUCT-PLAN-S1-S8.md`) | plan only |
| Reference | `capability/distribution/PROTOCOL50-FULL-IMPLEMENTATION-PLAN.md`, `ICECREAM-REAL-TRANSFER-ARCHITECTURE.md`, `capability/GUID_ROUTING_RESIDENCY.md` + local-oracle reviews 2026-08-23 | design of record |

**Build/test loop (proven):** rsync → q3 `~/phaseb-build` → pinned container `icecream/farm-node:ubuntu22-gcc11-boost174` → `make check` + farm byte-exact gate (`bt.sh` green on baseline). Deployment via `farm.py` (bijection join gate live: 51/51 fmt with per-TU JobID→endpoint→sha rows, zero orphans). research6 = backup build host.

## S0 — Trunk acceptance, registry freeze, authority map (NO code)

1. **S0.1** External review acceptance of `a862` (deep-reviewer assignment already queued + bigoracle exact-SHA verdict). Until accepted, nothing lands.
2. **S0.2** Publish/review the exact advertisement successor commit (child of `a862`) that carries the inert Login-side cache-endpoint advertisement (canonical `0/0/0`).
3. **S0.3** **Freeze the profile-ID registry** (one document, reviewed): `P29=1, ZSTD_TU=2, GRZ=3, z3_long=4 (reserved, frozen name), z3_shared_long=5 (reserved, frozen name)`; owner-semantics reconciliation (`ZSTD_ROUTE`↔`z3_long`, `ZSTD_COHORT`↔`z3_shared_long`) — same ⇒ frozen name carries; different ⇒ allocate ≥6. **4/5 are never reused.** The simple z3 profiles appear in encode/decode/negotiation compatibility tests whether operational or reserved.
4. **S0.4** Publish the **authority/blob map** for every asset to transplant (M0/M1 `cache/*`, M2 loopback, reset-ack, InputRecord deltas): blob sha256 → origin commit → destination path → owning review.
**Gate:** `a862` accepted; registry + authority map reviewed by both oracles. **Fallback:** if `a862` acceptance stalls > the review SLA, escalate to owner with the exact blocking findings — no work proceeds on an unaccepted trunk.

## S1 — Port the cache lane onto the trunk (asset transplant, layer-gated)

Port ONLY genuinely missing assets, in dependency order (M0/M1 core → wire tests → formal → M2 loopback endpoint → reset-ack → InputRecord deltas not already in `a862`), each layer followed by:
- build + full `p50_*` unittests + `check_trace.py` + key-layout census in the container;
- **deletion-mutant gates incl. a known-caught control mutation** (harness liveness);
- **old-wire gate:** legacy path message-level bytes unchanged vs `a862` golden (deterministic artificial-cluster capture);
- the previously-ported layers' gate suites re-run (deletion-aware stacking).
**Entry check for the M2 assets:** the two R2 HOLD defects (completion-identity clauses surviving `if(false)`; window bound not live) must demonstrably die in the transplanted revision.
**Exit:** trunk carries M0/M1/M2/reset with all gates green. **Fallback:** a transplanted asset failing its origin gate is dropped back to its owning PR for rework — the trunk never carries unowned code.

## S1b — Release identity 1.5.90 (bounded, after advertisement acceptance)

One dedicated step (v6-rereview §4 — never buried in M3/profile code): `configure.ac` development version **1.5.90**; generated package/archive names + installed `icecc --version`/library identity; Ubuntu22/Ubuntu24/Fedora package-dependency probes + exact artifact manifests; old-P43 / new-P50 binary-set labels derived from INSTALLED artifacts. **Gate:** installed identity matches manifests on all three distro probes.

## S2 — ONE endpoint/owner model + real M3 one-TU attachment

**Frozen ownership decision (per review §4 — one model, not a hybrid):** the cache roles run as a **separate supervised sidecar process** (`icecc-cache-service`, C-role and F-role) **over AF_UNIX**, started/watched by `iceccd` — matching the architecture doc's naming and keeping the daemon event loop untouched.
**Specified in full before nonzero advertisement:** startup readiness (READY handshake), crash/restart supervision (restart budget + degrade-to-legacy), fd ownership (daemon passes the accepted cache-connection fd or the sidecar owns the listening socket — entry design decision), backpressure (bounded queues, one outstanding write), shutdown ordering, privilege boundary (sidecar drops to `icecc`), advertisement transition (Login stays canonical `0/0/0` until sidecar READY).
**Endpoint discovery (local-oracle v6-rereview §1):** the **capability advertisement is Login-only**, but active cache use requires a separate **assignment-bound S→C handoff** (NOT a second advertisement authority): a **P50-only extension of the selected-assignment/UseCS carrier** (primary choice — it already carries the assignment epoch/wire-id/nonce under `ASSIGNMENT_IDENTITY`; a new assignment-bound message is the reviewed fallback) delivering the selected F's qualified cache endpoint {host-derived-from-selected-F, port, cache protocol/profile eligibility}, **bound to the exact assignment epoch/wire-id/nonce and selected-F identity**. Scheduler reads the retained Login snapshot only AFTER ordinary worker selection and only for explicit cache-mode eligibility; mixed/absent/stale values project wholly absent; ordinary worker scoring stays cache-neutral unless a later reviewed policy changes it. C validates the full assignment binding before connecting. **Mutation gates:** stale endpoint, wrong F, wrong assignment, partial metadata, post-reconnect reuse, selection-code deletion — each must fail.
**M3 attachment — frozen InputRecord→compiler ownership contract (v6-rereview §2, BEFORE product code):**
- daemon owns `logical_job_id/ATTEMPT_ID → (C_GUID, TU_SEQ)`; **`InputRecordKey` is exactly `(C_GUID, TU_SEQ)` — ATTEMPT_ID never enters cache identity**;
- a committed Open input produces exactly ONE same-owner input-ready notification before compiler admission; a Closed commit never exposes an attachable key;
- attach transfers an independent byte-zero cursor; close blocks new attaches while existing cursors survive; record reclamation waits for logical-job close + all cursor releases;
- cancellation, remote-result rejection (incl. caret local retry), scheduler loss, daemon/sidecar restart, and attempt replacement each close EXACTLY their owner without touching another attempt;
- AF_UNIX request/reply identities, ack/replay semantics, fd/cursor ownership, and the bounded pending-ready table are **deletion-tested**.
Then: one real TU flows C-clone → C-cache → (cache channel) → F-cache → F-clone → compiler, `ZSTD_TU`, byte-exact; restart/soak (kill -9 each role mid-TU, seeded; ≥1000-TU soak; ASan clean; 4-case reconnect replay A–D with per-case counters).
**Async model:** framed reader + single writer per relationship, bounded priority queues, codec CPU on a bounded worker pool (gcc11 fallback: stackless callbacks, no `co_await`; io_uring OUT).
**Gates:** deletion-test the advertisement (removing READY must force `0/0/0` + legacy); every S2 seam mutation dies; trace conformance (§TLA) on all restart scenarios. **Exit:** profile interface frozen (vtable seen by the transaction engine) — S6 profile work may proceed against it. **Fallback:** if supervised-process plumbing stalls, the REVIEWED alternative is same-process-on-owner-loop — switching requires a design-review sign-off, not silent drift.

## S3 — Persistence + eviction under a GLOBAL resource model

As v5 (immutable arena per (C_GUID, generation); ABSENT→INSTALLING→PRESENT→PINNED; byte-cap LRU whole-namespace eviction; ≈3h C-gen / ≈2h F-idle retention; generation-wrap → admission stop + GUID flip), **plus the review's precondition: a global/cross-relationship TLA extension (namespace eviction, aggregate memory caps, staging-slot ownership) lands BEFORE the product claims these transitions** (model-first rule).
**Gates:** eviction storm byte-exact; forced wrap → GUID flip, zero cross-namespace reuse; same-key-different-content ⇒ fatal fault; RSS bounded over ≥10k-TU soak; crash mid-INSTALLING idempotent; global-model TLC green + trace conformance. **Fallbacks:** on-disk persistence deferred (RAM arena + degrade-to-legacy); plain LRU (no ARC).

## S4 — Compatibility: EIGHT logical S/C/F cells + physical topology

**Logical (scheduler included — a three-role protocol):** all eight P43↔P50 tuples `S/C/F ∈ {43,50}³`. Only `50/50/50` may engage the cache; **`43/50/50` stays whole-legacy** (no direct C↔F negotiation smuggling cache mode around the scheduler). Peer classes distinguished per cell: P43 old, P48/P49 boundary, identity-only P50, cache-advertising P50, P50-endpoint-absent/disabled. Every legacy cell: engagement counter == 0 AND old wire bytes unchanged (golden capture).
**Physical (slots ≠ Fs; one worker container/route/cache identity per physical F):** C1F1 → C1F2 → **C1F4 with nas642 as C**; **q2-as-C = C1F3** (it cannot be its own F); per-F slots configured independently (1–32).
**Slow-client cells:** nas642-vs-q2 as C over the SAME F subset, same per-F slots, scheduler, binaries, compiler env, workload/order, cache state, network route — otherwise client-bandwidth attribution is invalid.
**Role-artifact step (BLOCKING, v6-rereview §3):** before any cell runs — build **exact P43** from tag `1.4` / commit `cd74801e0fa4e83e3ae254ca1d7fe98642f36b89` and **exact accepted-P50-trunk** binaries; immutable per-set runtime roots/images + compiler environments + manifests + hashes + installed verification; `selected_binary_sets[S|C|F]` must actually select each role's runtime path/image (**deletion test: changing a role selection changes the LAUNCHED binary, not merely its label**); preflight every role's executable + image digest. No fabricated `p50-*` run identity before an exact P50 artifact is selected. All eight tuples run through real container processes at C1F1 minimum, then the all-P50 physical ladder.
**Caret cells (per the 2026-08-23 caret ruling):** the remote object-byte/cache-performance cells run with manifest-bound `ICECC_CARET_WORKAROUND=0`; one mandatory **default-caret companion cell** (override absent) joins the warning-TU's full sequence — assignment → F remote completion → client rejection classified E102 → local diagnostic retry → final accepted digest → clean closure of the remote assignment/InputRecord (no pending TX, no leaked lease/slot, no duplicate acceptance) — as an explicit expected-diagnostic-policy outcome, excluded from scored cache-performance totals; release/compatibility gate.
**Gates:** all 8 logical cells + physical ladder byte-exact with the per-TU bijection join (JobID→F begin/done→client acceptance→object digest, zero orphans/local fallbacks — implemented and green on fmt 51/51). **Fallback:** none — cells exist to fail loudly; any red is a release blocker.

## S5 — DEPLOY `ZSTD_TU` baseline (exact ledgers + rollback)

As v5 S6, hardened per review §7: **immutable manifests** (binary sha, compiler-env digest, workload manifest); preparation strictly outside the measured interval; cold/warm defined operationally (exact reset commands: fresh C_GUID + F-store wipe = cold; retained = warm); per-TU object/reference digest same env/argv/cwd/inputs; **exact C→F and F→C product ledgers** (interface counters = coarse reconciliation only); **paired/counterbalanced repeats** (not N=3 fixed-order): ABBA ordering per mode pair, report every sample + median + p50/p95/p99; idempotent up/test/down + crash-safe cleanup; contention cells (same-load, mixed-load) at C1F2/C1F4.
The ±% wire comparison is defined ONLY after fixing: direction (C→F vs F→C separately), layer (cache-channel protocol bytes vs job-connection bytes), env-transfer excluded, denominator (raw preprocessed bytes).
**Rollback:** previous product tree retained per host; rollback = bind-path flip, one `farm.py` flag.
**Exit = DEPLOYED functional P50 baseline.** **Fallback:** perf regression ⇒ cache opt-in (deployed, default legacy).

## S6 — Profiles under the frozen registry

Order: **(1) `ZSTD_ROUTE` — transactionally correct FIRST** (review §5): committed vs tentative encoder/decoder contexts; a TU operates on a tentative checkpoint/clone; only the matching terminal commit promotes it; rejection/disconnect discards tentative state; lost-terminal replay = retained exact encoded bytes + same committed predecessor, or deterministic reconstruction; cases C/D = explicit identity-bound reset acknowledged by both sides. **The state machine exists in TLA + product tests BEFORE the profile is implemented.** Gates: dropped/duplicated/reordered completion, lost commit, C/F process loss, reset-during-pending-TU, state-digest mutation, window-memory cap, exact post-reset recovery.
**(2) `P29`** — canonical `CW_P29_BSC_Z3_M64` (local-oracle SHA confirmation first); stage 1 HistoryIndependent, stage 2 RouteHistory. **(3) `GRZ`+RESIDUAL** — `capability/grouprlz` port; residual = libbsc BWT stage behind `--with-libbsc`; libbsc absent ⇒ GRZ not advertised.
**Simple z3 profiles:** `z3_long`/`z3_shared_long` remain in the registry and in encode/decode/negotiation compatibility tests (operational or reserved per the S0 reconciliation — the owner requires them kept).
**Vendor provenance (review §10):** libbsc/Asio pinned by upstream revision + tar digest + license files + local patch set + reproducible source-package membership; reuse the reviewed scoped Boost discovery — no second unscoped dependency path.
**Per-profile gates:** corpus1/2/3 offline replay byte-exact; live C1F1 byte-exact; corrupt-object ⇒ fatal fault; pinned-core encode ≥ ~100 MB/s on q3 (EPYC 8124P), cold+warm separately; negotiation matrix incl. unsupported-profile fallback.
**Fallbacks:** GRZ-no-residual first; P29 ships HistoryIndependent if RouteHistory unstable; `ZSTD_ROUTE` window clamped by route-count budget.

## S7 — EXACT simulator replay conformance (zero tolerance)

`cache/sim/p50sim` links the SAME product codec objects. **Conformance replay** (review §8.1): consume the exact validated live input manifest, ordering, cache/reset state, and canonical route trace from a live run; the simulator's action sequence, per-stage bytes, digests, and totals must match the product **EXACTLY — zero tolerance** (any mismatch is a correctness bug in sim or product; localize via the per-stage ledger). Bind: source commit, scenario manifest, route-trace digest, compiler-env digest, workload-input digests, binary selection, cold/warm state.
**Gate:** exact match on {fmt, RocksDB} × {ZSTD_TU, ZSTD_ROUTE, P29, GRZ+res} conformance cells.

## S8 — PREDICTIVE simulation vs live performance

**Predictive mode** (review §8.2): simulator runs WITHOUT the live route trace (its own scheduling/routing model); scored against live per (corpus × profile), cold and warm separately; tolerances are **empirically calibrated performance-model metrics, not correctness gates**. **Cell matrix = the FULL cross-product honoring the immutable GOAL (v6-rereview §5): every retained target corpus {fmt, corpus2 RocksDB, corpus3 DuckDB, corpus1 LLVM-1238} × every implemented profile {ZSTD_TU, ZSTD_ROUTE, P29, GRZ+res} — sim AND live AND conformance/predictive comparison for each.** Resource-heavy cells are STAGED (cost order: fmt → RocksDB → DuckDB → LLVM) but S8 is not complete until the full cross-product has run — any reduction requires an explicit owner change to the GOAL. Giants sim-first (**Firefox authority = the retained unified 2,498-TU / 15,240,876,398-raw-byte manifest** unless a new immutable manifest supersedes); plus the nas642-vs-q2 matched-topology cells.
**Deliverable:** published comparison (Artifact + #16) + per-regime profile recommendation + proposed default. `ZSTD_COHORT`/`z3_shared_long` operationalization enters here, sim-first.

## §TLA — conformance mode (claims bounded to what the gates establish)

Coverage today: bounded ccache↔fcache transaction core (1C × 2F × 2TU, `MaxRel=2`, 7 invariants); abstracts compression/timing/env/scheduler/parsing; pin/evict + arbitration enter with S3/S4/S5 (model-first).
**What the mode establishes (review §9 — claim weakened accordingly):** Level-1 (`check_trace.py`, every run) proves emitted action names + core ordering are model-known — it does NOT prove the product emits faithfully. Therefore the mode adds: **(a) exhaustive production-transition→trace-emitter census** (every state-changing site maps to exactly one emitter); **(b) emitter mutation matrix** — one-at-a-time deletion/misordering/identity mutation of each emitter must fail Level-1/Level-2; **(c) full expected/observed identity + state-precondition checks in the checker; (d) source SHA + trace SHA + schema binding; (e) fail-closed `trace_to_tla.py` with source-shape/semantic tests; (f) reachability witnesses** — every required action observed in at least one gate scenario. Level-2 = TLC trace refinement per step scenario. **Global/cross-relationship model is a precondition for S3 eviction, S4 arbitration, S5 deploy claims.** Runtime flag `ICECC_P50_TRACE=<path>`, audit-pipe bytes excluded from scored ledgers.

## Review protocol & risks

- Every gate reproduced first-hand by deep-reviewer before any PASS; bigoracle exact-SHA verdicts per step; local-oracle design deltas. No relayed PASS without reproduction. **No merges to `main` — the owner lands everything.**
- **Critical path:** S0→S1→S2→S3→S4→S5 serial; S6.1 (`ZSTD_ROUTE` TLA state machine) may be drafted during S3–S5; S6 implementation after S2's frozen interface + S5 baseline; S7 after S6 profiles exist; S8 last.
- Schedule risk: S1 (transplant discipline) and S2 (sidecar) — mitigated by per-layer gates and the one-TU M3 demo.
- HOLD discipline: no S1 work until S0.1 (`a862` acceptance) completes.
