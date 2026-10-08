# P50 Route Pipelining — Design Doc (DRAFT, WIP)

Status: DRAFT — skeleton, sections being filled in. No implementation code in this
tree change; this document only.

Branch: `perf-pipe`, off `a348414e` ("services/sys_close_range.h: guard the UAPI
include with `__has_include`"), the current farm "perf-combo" build
(instrumentation + robustness fixes + perf-opt).

Author: pipelining designer session, reviewed by sorbet-main before any code lands.

## 0. Problem statement and goal

Today, one route (a C-store-GUID, F-store-GUID, profile identity with a monotonic
`rel_seq` cursor and state digest) allows exactly **one uncommitted TX in flight**
at a time, enforced redundantly at four independent layers (see §2). Each TX pays,
end to end, roughly: C-side prepare (~12.6 ms) + write (~4 ms) + one network RTT
waiting for F's STATE/COMMIT frame (~22.8 ms) + F-side serial work (~19.5 ms on one
host, ~13.3 ms on another) + the return trip. (This breakdown is externally
measured telemetry, not a code constant — there is no `~70 ms` literal anywhere in
`cache/p50_cache_service.cpp`; see the owner-thread/transfer-lock audit cited
throughout §1.) ~19% of TXs additionally pay a TCP slow-start tax because every TX
opens a fresh connection (host-level `initcwnd` tuning was explicitly refused by
the farm owner on 2026-10-08; pipelining must *hide* this cost through overlap,
not eliminate it through host tuning).

Net effect: one route does ~13-15 TX/s; three routes to one F give ~41 TU/s, while
F's own worker slots sit mostly idle (one host ~33% occupied, another ~64-73%).
The farm is **round-trip- and serialization-bound, not compute-bound**.

Goal: let C hold up to `K` *uncommitted successors* per route (`K` configurable via
an env var, e.g. `ICECC_P50_ROUTE_PIPELINE_DEPTH`, default `K=1` == exactly today's
behavior) so that the per-route lock shrinks from covering the *entire* operation
(prepare, encode, write, wait-for-STATE, wait-for-COMMIT) down to covering only the
local, F-independent part (prepare + encode + write). The RTT-bound "wait" part is
then overlapped across up to `K` outstanding TXs. Target steady state: per-route
throughput approaches `1 / max(C local serial time, F local serial time)` — e.g.
~1/19.5 ms ≈ 51 TX/s on the slower host, versus ~13-15 TX/s today — bounded by
whichever side's own CPU-bound stage is slower, not by RTT.

Fixed constraints carried into every section below (approved 2026-10-08, not
re-litigated here):

- Still exactly one TCP connection per job/TX; `rel_seq` is still the TX identity;
  the per-TX ARM claim stays job-bound (§4).
- No new session concept, no new frame types, no codec/digest changes, no
  authority changes.
- `K` is env-configurable; `K=1` must reproduce today's wire behavior and today's
  single-slot invariants exactly (so this is a strict superset design, not a
  rewrite).
- C prepares TX `n+1` against the *predicted* post-state of TX `n`; C already
  computes this value locally (see §1.1) — "prediction" is deterministic replay of
  C's own serializer output, not speculation about F.
- The per-route lock covers prepare+encode+write only, not the STATE/COMMIT wait.
- Any fault on any outstanding TX (predecessor fail/abort/timeout, digest
  mismatch, any reconnect outcome other than `ExactMatch`) invalidates **all**
  outstanding successors; they abort through the existing cold-replacement path
  (`RouteHistoryReset`/`ColdFStore`), falling back exactly as a faulted TX does
  today (§3).

## 1. C-side and F-side state-machine diffs

### 1.1 C side: predicted-state bookkeeping

**Today's single-successor enforcement (all four sites, to be generalized):**

1. `P50PreparationAuthority::prepare_for_route` (`cache/p50_endpoint.cpp:1068`):
   admission check at **1096-1100**:
   ```cpp
   if ((route.profile == ProfileId::ZSTD_ROUTE ||
        route.profile == ProfileId::P29V1) &&
       route.uncommitted_route_entry.has_value())
       throw std::logic_error(
           "selected route profile requires its predecessor to commit before "
           "preparing the next TU");
   ```
   backed by `Impl::RouteState::uncommitted_route_entry` (`std::optional<uint64_t>`,
   field at **904**, "admitted twice" guard at **1208**, set at **1214**, reset at
   **1231-1232** (error path), **1463-1465**, **1513**, **1531** (commit paths)).
2. `P50ClientEndpoint::run_connected` (`cache/p50_endpoint.cpp:3320`): single-dialogue
   guard at **3339-3340**: `if (impl_->active_session != 0) throw
   std::logic_error("C endpoint already has one active dialogue");` — field
   `active_session` at **1892**; `impl_->active` / `impl_->queued` (single
   `optional<PreparedInputPtr>` each) gate admission at **3347-3356**.
3. `CRoute` (`cache/p50_slice0.h:332-380`): scalar `active_` field
   (`std::optional<CActiveTx>`, one of **371-378**); "C route already has one
   ACTIVE_TX" at `cache/p50_slice0.cpp:1880`.
4. `P29Serializer` (`cache/codec/p29_wire.h`): scalar `Pending pending_` member;
   `has_pending()` at **992**; `begin_tu()` guard at **~1037-1058** ("P29 serializer
   already has a pending TU"); each `Pending` (struct at **~1201-1232**) already
   carries its own `base_*` pre-state snapshot fields.

**Key existing precedent — C already knows the "predicted" digest without
guessing.** `P29Serializer::commit()` (`cache/codec/p29_wire.h:1134`) computes TX
`n`'s resulting state digest the moment C finishes encoding it *locally*, before
C has heard anything back from F. That digest is exactly the "predicted post-state
of TX n" the task asks for — it is not probabilistic, it is C replaying its own
already-computed output. Today this value is thrown away until F's COMMIT comes
back and the *reconnect* exact-match check reconstructs the same comparison after
the fact, at `cache/p50_endpoint.cpp:3601-3604`:
```cpp
const bool exact = same_f && peer.namespace_present && peer.route_present &&
                   impl_->route_known && peer.history_nonce == impl_->history_nonce &&
                   peer.next_rel_seq == impl_->next_rel &&
                   peer.state_digest == impl_->state;
```
Pipelining reuses this exact mechanism one layer earlier: instead of only
comparing a predicted cursor against F's STATE *after a reconnect*, C compares it
against F's STATE for TX `n+1`'s own fresh connection, in the normal (non-faulted)
path.

A second existing precedent is `pipelined_begin`
(`enable_pipelined_begin()` at **3287-3289**, field at **1900**, gate + comment at
**3505-3512**, write ordering at **3514-3555**): C already writes
HELLO/BEGIN/BODY/FILL *before* reading F's STATE for a single fresh TX, verifies
the guess against STATE once it arrives, and falls back to a non-`ExactMatch`
reconnect outcome on mismatch. Cross-TX pipelining is structurally the same
pattern one level up: write speculatively, verify against STATE later, fall back
to the same outcome enum on mismatch — no new fallback vocabulary needed.

**Diffs required (C side):**

1. `Impl::RouteState::uncommitted_route_entry` (`optional<uint64_t>`) →
   bounded FIFO `outstanding` (depth ≤ `K`, e.g.
   `boost::container::static_vector<OutstandingEntry, kMaxPipelineDepth>`) of
   `{rel_seq, pre_digest, predicted_post_digest, PreparedTuHandle}`. The admission
   check at 1096-1100 becomes `outstanding.size() >= route_pipeline_depth(route)`;
   the "admitted twice" guard at 1208 generalizes to "this `rel_seq` is already in
   `outstanding`".
2. `CRoute` (`cache/p50_slice0.h:371-378`): scalar `active_` → bounded FIFO
   `active_queue_` of `CActiveTx`, each entry carrying its own
   `{history_nonce, rel_seq, pre_digest, predicted_post_digest}` snapshot. Route
   fields split in two: `confirmed_cursor_` (advances only on a real F COMMIT ack,
   i.e. inside `accept_commit`/`reset_history`,
   `cache/p50_slice0.cpp:2112-2135`/`2145-2154`) and `predicted_cursor_` (advances
   immediately after each *local* `P29Serializer::commit()`, feeding the next
   `begin()`'s expected pre-digest). The 1880 guard becomes
   `active_queue_.size() < K`.
3. `P29Serializer::Pending` (`cache/codec/p29_wire.h:~1201-1232`): scalar
   `pending_` → a ring buffer of depth `K`. No new fields per entry — each
   `Pending` already snapshots its own `base_*` state. `begin_tu()`'s guard becomes
   a depth check against the ring's occupancy.
4. `P50ClientEndpoint::Impl` (`active_session` at 1892, `next_rel` at 1898,
   `state` at 1899): scalar `active_session`/`active`/`queued` → an ordered
   map/deque keyed by `rel_seq`, each entry owning its *own* `ClientIoState`
   (socket). This is required, not optional: `async_wait_peer_close`'s guard at
   `cache/p50_endpoint.cpp:788-802` ("message arrived after final TX_COMMIT" at
   line 799) structurally forbids sending a second BEGIN on one already-committed
   connection, so TX `n+1` *must* use a brand-new TCP connection — confirmed
   independently from both the C-side and F-side audits. `run_connected`'s guard
   at 3339-3340 becomes "already have `K` outstanding dialogues for this route."
5. For TX `n+1`, reuse the existing `pipelined_begin` code path
   (3505-3512 gate, 3514-3555 body) almost verbatim: open a new connection, write
   HELLO, then — gated on `predicted_cursor_` being valid (TX `n` already
   committed *locally* on C, even though F hasn't acked it yet) instead of today's
   `!impl_->active` (which only allows the fast path when nothing at all is
   outstanding) — write BEGIN(`rel_seq = n+1`, `pre_digest = predicted_post_digest`
   of TX `n`) / BODY / FILL eagerly, exactly as today. New: the route lock is
   released right after this write completes (today it is held until STATE is
   read back at 3561-3566; the new design releases it at the point the FILL write
   finishes, ~3555, *before* the `co_await async_read_frame` STATE read at
   3561-3564).
6. The STATE read (3561-3566) and the exact-match computation (3601-3604) for
   TX `n+1`'s connection must now treat "F hasn't reached my predecessor yet" as a
   normal, expected, non-error outcome rather than something only reachable via
   reconnect — see §1.2 for why this requires *zero* further change on the C side
   under the recommended design.

### 1.2 F side: parking

**Today's server-side flow** (`P50ServerEndpoint::run_connected`, server-side span
`cache/p50_endpoint.cpp:4136-4557`, one TX per connection): accept
(4301-4305) → HELLO (4306-4314) → STATE exchange (4321-4327) → optional
HISTORY_RESET (4332-4343) → BEGIN (4356-4373) → BODY/NEED/FILL loop (4382-4421) →
`begin_materialization` (4430) → `async_materialize` (4433, def 508-596) →
`finish_materialization` (4439, def 3018) → `select_materialized_job_state`
(4444, def 3084) → `commit_materialized` (4453, def 3103) → COMMIT write
(4463-4466) → wait-peer-close (4485-4487, def 788-802) → disconnect+close
(4492/4494) → `co_return` (4499).

**Today's single-slot enforcement:**

- `FStore::begin` (`cache/p50_slice0.cpp:2318-2358`): cursor-match check
  (2324-2326, "TX_BEGIN does not match F's route cursor"), route-established
  check (2322), overflow guard (2333), occupied-slot check (2334-2338, "F route
  already has a different ACTIVE_TX" — tolerant of byte-identical replay via
  `same_begin`/`operator==` at line 167).
- `FStore::Namespace::Route` (**2168-2175**): `history_nonce`, `next_rel_seq`,
  `state_digest`, `last_commit`, and a *scalar* `pending: optional<FPending>`, plus
  a **shared, per-route** (not per-TX) `p29v1` codec view.
- `FStore::materialize_and_verify` (**2454-2583**) mutates that shared
  `P29FRouteCodec` deserializer in place; on failure it sets
  `route.p29v1->terminal = true` (2573-2582), poisoning the *whole route*. It does
  not touch cursor/`pending` lifetime — only `commit_input` (2585-2615,
  specifically 2609-2613) does that. **Consequence: `materialize_and_verify` is
  not safely callable for TX n+1 while TX n has not yet committed** — the shared
  deserializer's internal state assumes strictly sequential application. Parking
  must defer this call, not just the wire-visible COMMIT.
- `P29V1LiveDialogue` (`cache/p50_profile.cpp`): single `state_` field (default
  `State::Idle`, line 16/273) and a single `FStore store_` member (line 264,
  one dialogue+codec view per *route*, not per TX). `begin()` (32-64) guard at
  40-41 ("P29V1 TX_BEGIN arrived while dialogue is active"); `commit_visible()`
  (133-145) requires `Materialized` and resets to `Idle` via `clear_per_tu()`
  (139-140, def 238-245).
- `async_wait_peer_close` (**788-802**): throws "message arrived after final
  TX_COMMIT" (line 799) if any byte arrives after COMMIT on the same connection —
  this is the exact code that structurally forbids reusing one connection for two
  TXs, confirming §1.1 point 4's "new connection per outstanding TX" requirement
  from the F side too.
- Separate, pre-existing bound worth flagging: `EndpointCodecPool`
  (**462-490**), `kWorkerCount = 2` (464), `kMaxOutstanding = 8` (465),
  `try_acquire()` (467-476, throws "endpoint codec queue is full" at 539) — the
  pool used by `async_materialize`'s worker call to `dialogue->materialize_verified()`
  (558). This is an *existing*, separate concurrency ceiling (2 worker threads,
  8 outstanding) that already tolerates more than 1 outstanding materialize call
  across *different routes*; K>1 on one route adds load to this same pool and
  must be checked against it (§4), but does not require changing it.

**Design choice — "F delays STATE for a parked TX" vs "C accepts a STATE cursor
within its outstanding window":**

The task asks to evaluate both and pick the one needing fewer changes.

- **Option A (recommended): F parks.** After accepting TX `n+1`'s connection and
  reading HELLO, F reads and drains TX_BEGIN/BODY/NEED-reply/FILL
  (4356-4421) **exactly as today, with no delay** — this is important: it means
  C's write phase under the per-route lock (§1.1 point 5) always completes
  promptly regardless of ordering, because F is still actively reading. What F
  defers is only the "become visible" tail: `begin_materialization` through the
  COMMIT write (4430-4466). Concretely: F stages TX `n+1`'s fully-received input
  in a **new per-connection staging buffer** (not the shared
  `FStore::Namespace::Route.pending`/`p29v1`), and parks that connection's
  coroutine on a per-route sequencer keyed by expected `rel_seq` (e.g. a small
  `map<rel_seq, promise/channel>`, signaled by the predecessor's `commit_input`
  at `cache/p50_slice0.cpp:2609-2613`). Once signaled, F hands the staged input to
  the shared deserializer and proceeds through `materialize_and_verify` →
  `commit_materialized` → COMMIT write exactly as today.

  *Why fewer changes*: C's STATE-read path (3561-3566) and exact-match logic
  (3601-3604) need **zero** changes — C already blocks on
  `co_await async_read_frame` for STATE, and it is already correct, today, for F
  to take an arbitrary amount of time before writing that frame. All new
  machinery (the parking map, the staging buffer, the generalized occupied-slot
  check) lives entirely on F, which already owns the authoritative route cursor
  and is the natural place to decide "whose turn is it."

  *Changes needed*: (a) `FStore::begin`'s occupied-slot check (2334-2338)
  generalizes from "reject if ACTIVE_TX already set" to "accept iff this
  `rel_seq` is exactly `nextExpected .. nextExpected+K-1`", i.e. extends a bounded
  FIFO instead of filling one scalar; `Namespace::Route.pending` becomes a small
  `map<rel_seq, FPending>` bounded by `K`. (b) a new per-route commit sequencer
  that parks out-of-turn connections' coroutines until their immediate
  predecessor's `commit_input` fires, then releases them strictly in `rel_seq`
  order. (c) `P29V1LiveDialogue`'s single `state_`/`store_` do **not** need to
  become `K` parallel instances — the cheaper option is to keep one shared
  dialogue instance and let (b)'s sequencer be the only thing that ever calls
  `begin()`/`commit_visible()` on it, strictly in order. TX `n+1`'s own
  `begin()` call against the shared dialogue is itself deferred until TX `n`'s
  `commit_visible()`'s `clear_per_tu()` → `Idle` transition has happened — a
  direct consequence of (b), not a separate change. This confines the
  generalization to "one more layer of queueing in front of an unchanged
  single-shared-dialogue state machine."

- **Option B (rejected): C accepts a windowed STATE cursor.** This requires
  generalizing C's exact-match comparison (3601-3604) to accept a *range* of
  valid `(rel_seq, digest)` pairs, and requires F to answer STATE immediately,
  with a cursor value that is only correct "once TX n commits" — i.e. F would
  have to do its own prediction of a not-yet-committed cursor, duplicating the
  job C already does. It also requires relaxing `FStore::begin`'s cursor-match
  check (2324-2326) to a window, but F *still* needs the same ordering/staging
  machinery as Option A to decide when it's actually safe to run
  `materialize_and_verify` relative to the predecessor. Option B therefore pays
  the full cost of Option A's F-side machinery *plus* extra C-side
  generalization, for no additional benefit — rejected.

**Fate of TCP-buffered bytes while parked:** not left in the kernel socket
buffer. F's read loop for a parked connection runs unchanged through
BODY/NEED/FILL (4382-4421); every byte is drained into F's normal per-connection
decode path exactly as fast as for an unparked TX. Only `materialize_and_verify`
and everything after it are deferred. This matters structurally: if F stopped
reading while parked, C's async write under the route lock could block
indefinitely on TCP backpressure, stalling the lock for a reason unrelated to the
route itself — unacceptable given the owner thread is single-threaded and shared
by *all* routes and lifecycle RPCs on that endpoint (confirmed: `endpoint_owner_loop`
is `context_.run()` on one dedicated thread, `cache/p50_cache_service.cpp:2709-2722`
/ `:1926`; lifecycle RPCs are posted onto the *same* `context_`,
`cache/p50_cache_service.cpp:3241`, and must complete inside
`input_lifecycle_timeout`, 250 ms default / 5000 ms in production
(`cache/p50_daemon_sidecar_adapter.h:118`, `daemon/main.cpp:5349`) or the whole
sidecar is retired on `Timeout`, `cache/p50_daemon_sidecar_adapter.cpp:3265-3287`).

**Timeout for a parked TX:** bounded by the same `transfer_deadline` +
`cancellation_grace` that already bounds `transfer_source_on_owner`
(`cache/p50_cache_service.cpp:2083`/`2623-2626`). If the predecessor does not
commit before the successor's own deadline, the successor's park is abandoned and
it aborts through the existing deadline-exceeded path
(`ClientRunStatus::DeadlineExceeded`, `cache/p50_endpoint.cpp:3328-3330`) — no new
timeout concept, the existing per-op deadline doubles as the park timeout.

## 2. Invariants that must still hold under K>1

Each invariant below is stated as it holds **today** (`K=1`, implicit), then
restated for general `K`, with the evidence (code + the formal/trace tooling
that already encodes it) that it continues to hold.

1. **`rel_seq` is monotonic, contiguous, per-route, no gaps.** Today: enforced
   by `FStore::begin`'s cursor-match check (`cache/p50_slice0.cpp:2324-2326`)
   and `CRoute`'s own `active_`/cursor bookkeeping. Under K>1: unchanged in
   substance — the §1.2 generalization of `FStore::begin` only widens *how many*
   `rel_seq` values may be simultaneously staged (`nextExpected ..
   nextExpected+K-1`), it does not relax the requirement that each individual
   `rel_seq` arriving at F must be exactly the next unseen value in that
   window; a `rel_seq` outside the window is still rejected exactly as "does
   not match F's route cursor" is today. The TLA+ model's
   `SingleUnresolvedTU` invariant (`cache/formal/Protocol50ZstdRoute.tla:2602-2604`)
   generalizes from "at most one unresolved TU" to "at most `K` unresolved TUs,
   each with a distinct, consecutive `rel_seq`" — same shape of invariant, wider
   bound.
2. **At most one ACTIVE_TX per route, redundantly enforced at 4 layers.**
   Generalizes to **at most `K` outstanding (uncommitted) TXs per route**, with
   the same 4 layers (§1.1 points 1-4, §1.2 "`FStore::begin`'s occupied-slot
   check") each independently capping at `K` rather than `1`. `K=1` makes every
   one of these checks byte-for-byte identical to today's (`size() >= 1` is
   exactly today's `has_value()`/non-null check). This is the central
   backward-compatibility argument for the whole design: no enforcement site is
   *removed*, each is only parameterized by `K`.
3. **Digest chain integrity**: TX `n+1`'s `pre_digest` must equal TX `n`'s
   resulting digest. Today this is only checked once, synchronously, because
   TX `n+1` cannot even be prepared until TX `n` commits. Under K>1 it becomes a
   *chain* of `K` predicted digests (§1.1's `predicted_cursor_`), each still
   checked the same way — F's cursor-match check at commit time
   (`cache/p50_slice0.cpp:2324-2326`, run once per TX, strictly in order via
   the §1.2 sequencer) is unchanged; what changes is only *when* C computed the
   value it is asserting (ahead of time, locally, via
   `P29Serializer::commit()`, rather of necessity synchronously). If any
   predicted digest in the chain turns out wrong (predecessor aborted/mismatched),
   invariant 6 below is what discards the rest of the chain — the digest-chain
   invariant itself never has to tolerate "being wrong," it is instead backed by
   universal invalidation.
4. **Exactly-once, in-order apply on F.** Today trivially true (one TX at a
   time). Under K>1: guaranteed by the §1.2 sequencer, which hands TX `n+1`'s
   staged bytes to the *shared* `P29FRouteCodec`/`P29V1LiveDialogue` only after
   TX `n`'s `commit_input` (`cache/p50_slice0.cpp:2609-2613`) completes —
   i.e. F never has two TXs "in" `materialize_and_verify` concurrently for one
   route, by construction, because the shared deserializer object is single-
   threaded-use and the sequencer is the only caller. This is the one invariant
   where "fewer changes" (§1.2 Option A) was explicitly chosen *because* it lets
   this invariant hold by sharing the existing single-instance object rather
   than needing a new concurrency-safe multi-instance redesign.
5. **One connection carries exactly one TX; nothing may arrive on a connection
   after its COMMIT.** Unchanged, verbatim, under K>1 — `async_wait_peer_close`
   (`cache/p50_endpoint.cpp:788-802`) is not touched by this design at all; it
   is in fact the reason (§1.1 point 4, §1.2 "today's single-slot enforcement")
   that each outstanding TX *must* open its own fresh connection, which is
   already a fixed constraint from §0, not a new one.
6. **Universal invalidation on fault.** New framing of an old mechanism: today,
   a faulted TX already invalidates "the route" wholesale via
   `RouteHistoryReset`/`ColdFStore` reconnect outcomes — there is nothing *else*
   outstanding to invalidate because `K=1`. Under K>1, the *same* reconnect
   outcomes fire from the *same* trigger conditions (digest mismatch, timeout,
   abort), but now there is a queue of successors behind the faulted TX; the
   invariant is that **all** of them are discarded together with the faulted
   TX, never selectively kept. Concretely: the §1.1 `outstanding`/`active_queue_`
   FIFOs and the §1.2 parked-connection map are *cleared in their entirety*
   (not just popped at the front) whenever a non-`ExactMatch` outcome is
   produced for any member of the chain — because every successor's
   `pre_digest` was predicted against a chain rooted at the faulted TX, so every
   successor's prediction is simultaneously invalid. See §3 for the full matrix
   of which stage-combinations trigger this.
7. **Session continuity (`f["connected"] != replacing"`, one live logical F
   session per relationship at a time)** — from `check_trace.py`'s
   `current_f_session()`/session-replacement bookkeeping. This invariant is
   about the **logical session** (identified by `history_nonce` plus the
   route's established/not-established state), not about TCP connections — and
   it was already true that a session's *connections* are transient and
   per-TX (invariant 5), so a session having `K` simultaneously-open
   connections for its `K` outstanding TXs is not a new kind of multiplicity;
   it is still exactly one *session*. No change to session-replacement
   semantics (`RouteHistoryReset`, cold-store adoption) is proposed anywhere in
   this design.
8. **Scalar "pending"/"active" slot checks in the trace checker.**
   `check_trace.py`'s `f["pending"] == tx` (checked on `INPUT_MATERIALIZED`/
   `INPUT_COMMITTED`) and `c["active"] == tx` (checked on `COMMIT_ACCEPTED`/
   `LOST_COMMIT_ACCEPTED`) are literally scalar-equality assertions today and
   **must change** to "`tx` is a/the head-of-queue member of a bounded ordered
   set" — this is the one invariant class that cannot be preserved as "the same
   check with a wider bound" purely in code; the *trace checker itself* needs
   new logic (§5.1) because today's assertions are written assuming `K=1`.
   The underlying semantic invariant they encode — "F only materializes/commits
   a TX that C still considers outstanding, and C only accepts a commit for a
   TX it has not already retired" — is unchanged; only its *representation*
   (scalar vs. queue-membership) changes.
9. **Reconciliation witness** (F-ahead-of-C-by-exactly-one-`rel_seq` during the
   window between F's commit and C's acceptance, enforced by
   `check_reconciliation_witness()` in `check_trace.py`). Generalizes from
   "ahead by exactly 1" to "ahead by at most `K`, and never behind" — F may have
   committed up to `K-1` TXs that C has locally predicted but not yet had
   confirmed via STATE/COMMIT read-back. At `K=1` this generalized statement
   collapses to exactly the original "ahead by exactly 1" (since the only
   non-zero value ≤ `K-1=0`... — more precisely: at `K=1` there is only ever at
   most one outstanding TX, so "ahead by at most 1" degenerates to today's
   statement exactly). This witness needs updating in `check_trace.py` (§5.1).
10. **Per-TX ARM claim stays job-bound.** Unchanged by construction (§0 fixed
    constraint) — each outstanding TX still claims exactly one ARM slot for
    exactly its own lifetime; `K` outstanding TXs on one route now hold up to
    `K` simultaneous ARM claims instead of ≤1, which is purely a capacity/
    admission question for §4, not a change to the claim-is-job-bound
    invariant's *shape*.
11. **Route-lock mutual exclusion.** Today the lock serializes the *entire*
    per-TX operation, so "lock held" and "TX outstanding" are the same interval.
    Under K>1 these intervals split: the lock still provides strict mutual
    exclusion over the *local* prepare+encode+write stage (so two TXs on one
    route never interleave their writes on the wire — connections are still
    opened and written to one at a time, in `rel_seq` order), but "TX
    outstanding" now extends well past lock release, through the STATE/COMMIT
    wait. The invariant that changes shape (not disappears) is: "the route
    lock orders TX *admission/write* across the route" rather than "the route
    lock orders the TX end-to-end" — end-to-end ordering is now carried by
    `rel_seq` plus the §1.2 sequencer instead of by lock scope.
12. **`K=1` is byte-identical to today.** The umbrella invariant tying all the
    above together, and the primary regression-safety argument for §5: every
    structural change above is a parameterization by `K`, not a behavioral
    fork; setting `K=1` must make every new bounded-FIFO/queue/map degenerate to
    exactly one slot with exactly today's check sequence and exactly today's
    wire bytes. §5.1 includes an explicit "K=1 produces an identical trace to
    pre-pipelining code" regression test as the first-line defense for this.

## 3. Failure matrix

Successor state at the moment a predecessor fails is one of:

- **S1 — not yet prepared**: C has not called `begin_tu()`/encoded it; cannot
  exist until its immediate predecessor's *local* `P29Serializer::commit()`
  has produced a `predicted_post_digest` to seed it (prepare order is still
  strictly sequential — only the *wait* is pipelined, not local preparation).
- **S2 — prepared locally only**: in C's `outstanding`/`active_queue_` FIFO
  (§1.1), nothing written to a connection yet (the route lock still serializes
  writes to connections one at a time, in `rel_seq` order — invariant 11).
- **S3 — parked at F**: TX's connection is open, BEGIN/BODY/FILL fully sent and
  drained by F, staged in F's per-connection buffer, waiting on the §1.2
  sequencer's per-`rel_seq` signal.

There is no S4 ("already independently progressed past its predecessor") by
construction — invariant 4 (§2) guarantees the sequencer never hands a
successor's bytes to the shared deserializer out of order.

| # | Predecessor failure | Trigger / where | Effect on this TX | Effect on successors (S2/S3) | New mechanics needed |
|---|---|---|---|---|---|
| P1 | C-local prepare/encode fails | exception in `prepare_for_route`/`begin_tu()`/`commit()`, before any wire write | existing error path, unchanged | **none exist** — S1 only, nothing to unwind, because successor prepare is gated on this TX's local commit output | none — identical to today |
| P2 | C's wire write of BEGIN/BODY/FILL fails (connection error, partial write) | socket error during §1.1 point 5's write | existing error path for this connection | successors can only be in **S2** (writes are serialized by the lock in order) — simply dropped from the FIFOs, their connections are **never opened** | none — F never saw them, nothing to clean up on F |
| P3 | F rejects BEGIN at the cursor/digest-match check (`cache/p50_slice0.cpp:2324-2326`) — C's prediction was wrong | race/desync or genuine bug; under K>1 this becomes a reachable *expected-under-fault* path, not only a bug signal, because C now writes BEGIN before confirming STATE | reconnect outcome worse than `ExactMatch` (`RouteHistoryReset`/`ColdFStore`) for this TX | successors in **S2**: dropped by C, connections never opened. Successors in **S3**: F's cold-reset/HISTORY_RESET handling must also **erase the parking map's entries** for those `rel_seq`s — new mechanic; today's reset only clears a scalar `pending` | **F's reset path must clear the whole parking map, not one scalar** |
| P4 | Predecessor is itself a parked successor whose own predecessor already failed (cascading) | recursive case of invariant 6 | n/a — resolved as whichever concrete mode (P1-P8) actually originated the chain | all queued behind it, transitively | none — parking adds no *new* failure mode here, only widens the blast radius of an existing one |
| P5 | `materialize_and_verify` fails (digest/content mismatch, corrupt input) → `route.p29v1->terminal = true` (`cache/p50_slice0.cpp:2573-2582`) | F-side verify, same as today at K=1 | route marked terminal, exactly as today | successors in **S3**: the sequencer must refuse to hand their staged bytes to the now-terminal deserializer and instead fail them with the same terminal/cold-reset outcome. **Requires the per-`rel_seq` park signal to carry a success/failure variant**, not just a single "go" — new mechanic. Successors in **S2**: dropped by C once it observes the non-`ExactMatch` outcome for this TX | **park signal needs a failure variant, propagated to every still-parked successor** |
| P6 | F commits TX, writes COMMIT, but C's connection drops before C reads STATE back — `LostFinalAcknowledgement` | ambiguous-ack case; F's cursor genuinely *did* advance correctly | today: C reconnects, gets `LostFinalAcknowledgement`, re-syncs via STATE, treats TX as committed | **per the §0 mandate, successors are invalidated too — uniformly, even though this TX's prediction was actually correct.** Flagged as a candidate relaxation in §7, not implemented here | none required for the conservative behavior; a future optimization is out of scope |
| P7 | Sidecar `fail_stop()`/`_Exit(125)` while this TX was active anywhere in the pipeline | deliberate hard-crash to avoid releasing a route gate on ambiguous state | whole process exits, supervisor restarts it | **every route on that sidecar** reconnects cold — already true at K=1 for every *other* route; K>1 only means *this* route loses more unsent, locally-queued work than it would at K=1. Blast radius shape is unchanged | none — mechanism identical, just more work discarded per incident |
| P8 | Lifecycle retirement: missed `input_lifecycle_timeout` → `retire_input_lifecycle_relationship` via `CloseAcceptedJob`/`Timeout` | owner-thread-wide, not route-local | whole relationship retired, same category as P7 | same as P7 | **new risk this design must guard against**: if §1.2's "F must keep draining parked connections at full speed, never block the owner thread" property is violated by a bug, a stalled parked-connection write could itself *cause* a P8 retirement that would not have happened at K=1 — i.e. parking is the one mechanism in this design that could turn a route-local slowdown into a sidecar-wide outage. Mitigated by: no blocking calls on the owner thread's path for a parked connection (reads continue on the existing async loop exactly as for an unparked one), and explicitly probed in §5.2/§5.4 (backpressure injection) |

Summary of the one truly new failure *mode* introduced by this design (as
opposed to existing modes whose blast radius widens): **P8's parking-induced
owner-thread stall.** Every other row is either unchanged from today (P1, P2,
P6's mechanism, P7) or an existing mode whose cleanup now has to reach a
multi-entry structure instead of a scalar (P3, P5). This narrows what §5 needs
to stress-test beyond ordinary regression: specifically, backpressure on a
parked connection's socket must never be allowed to block the owner thread.

## 4. Interactions (ARM admission, route lock, deadlines, lifecycle RPCs, telemetry)

**ARM admission window — resolving a figure discrepancy.** The admission
window is `RouteGate::kArmedSessions = 8` (`cache/p50_cache_service.h:276`),
overridable via `ICECC_P50_ARMED_WINDOW` (`armed_window_slots()`,
`cache/p50_cache_service.cpp:2008-2017`). An earlier draft of this task's
brief cited "16" for this window; that figure is a *different*, unrelated
constant — `kMaxBacklog = 16` (`cache/p50_cache_service.cpp:93`), which only
bounds the `--backlog` CLI argument (TCP listen backlog) validated at
`cache/p50_cache_service.cpp:3698-3702`. **The correct, authoritative ARM
window default is 8**, and this document uses that figure throughout.

**What "ARM admission window" actually does today, precisely, and how K
interacts with it.** It is easy to conflate with this design's `K`, but it is
a different, complementary mechanism operating one layer up: a *lane* (one of
possibly several derived C-store-GUID identities multiplexed onto one logical
C/F/profile relationship, `RouteGate::Lane`, `cache/p50_cache_service.h:300-303`)
is allowed to hold up to `armed_window_slots()` (8) *pre-connected, HELLO'd*
F sessions **open ahead of its own transfer gate**, purely to amortize
connection-setup RTT (the header comment: "arming costs four round trips
before the gate, so a far F needs a window of about eight to keep its gate
busy"). Critically, at `K=1` **only one** of those armed sessions is ever
mid-transfer (holding `Lane::transfer`, the `std::timed_mutex` that is the
"per-route lock" referenced throughout §1) at a time — the other up-to-7 armed
sessions sit idle-but-connected, waiting their turn at the gate one at a time.
`route_lane_count()` (`cache/p50_cache_service.cpp:2019-2032`) already clamps
`lanes * armed_window_slots() <= max_live_sessions` (F's shared live-session
table capacity, `P50ServerOwnerLimits::max_live_sessions`,
`cache/p50_endpoint.h`) so that arming-ahead never oversubscribes F.

This design's `K` changes how many of a lane's already-armed sessions may be
**simultaneously past BEGIN** (i.e. actually outstanding/parked per §1, not
just idle-connected) rather than strictly serialized one-at-a-time through the
gate. The key resource-budget consequence: **every outstanding TX under this
design still needs its own live F session**, exactly as an armed-but-idle one
does today — so `K` consumes from the *same* `armed_window_slots()` budget a
lane already has, rather than requiring a new, additional budget. The natural
and simplest admission rule is therefore: **`K` must not exceed
`armed_window_slots()` for the lane** (default 8 ≫ default `K=1`, so there is
no practical pressure at today's defaults, but this must be an explicit
validation/clamp — analogous to `route_lane_count()`'s existing
`P50_ROUTE_LANES_CLAMPED` warning pattern — rather than an unenforced
assumption). No change to `route_lane_count()`'s formula is required *if* `K`
is clamped to fit inside the existing per-lane window; if a future tuning
wants `K` and `lanes` to both grow independently beyond what one shared
`armed_window_slots()` budget allows, the clamp formula would need to become
`lanes * armed_window_slots() <= max_live_sessions` **and** `K <=
armed_window_slots()` jointly — flagged as a §7 open question only if `K`
default/tuning is ever expected to approach 8; out of scope for an initial
`K` in the 2-4 range.

**Per-route lock.** Already covered in depth in §1.1/§1.2/§2 invariant 11:
`Lane::transfer` narrows from guarding prepare+encode+write+wait end-to-end to
guarding prepare+encode+write only. No change to the lock's *type*
(`std::timed_mutex`) or its *existing* timeout-based `fail_stop()` escape
hatch (§3 P7) — only the *scope* of what is held under it shrinks. Because the
lock no longer orders "is TX n+1 safe to start" (that is now `rel_seq`
sequencing, §1.2), the lock's sole remaining job is: never interleave two
TXs' wire writes on the same lane. This is a strictly smaller job than today,
so no new failure mode is introduced by the lock itself.

**Transfer deadlines.** `transfer_deadline`/`cancellation_grace`
(`cache/p50_cache_service.cpp:2083`/`2623-2626`) are reused unchanged as each
outstanding TX's own deadline (§1.2 "Timeout for a parked TX"), including for
the park wait itself. No new deadline concept. One consequence worth stating
explicitly: with `K>1`, up to `K` deadline timers are concurrently armed per
lane instead of ≤1 — this is a quantitative increase in timer-wheel load
proportional to `K`, not a new mechanism, and should be included in the farm
A/B's resource-overhead checks (§5.4) rather than treated as a correctness
risk.

**Lifecycle RPCs.** `apply_input_lifecycle_on_owner` and friends are posted
onto the *same* single `endpoint_owner_thread_`/`context_` that services every
route's transfers for that sidecar (`cache/p50_cache_service.cpp:2709-2722`,
`:1926`, `:3241`), and must complete inside `input_lifecycle_timeout` (250 ms
default, 5000 ms production override,
`cache/p50_daemon_sidecar_adapter.h:118`, `daemon/main.cpp:5349`) or
`retire_input_lifecycle_relationship` fires via the `Timeout` path
(`cache/p50_daemon_sidecar_adapter.cpp:3265-3287`) — this is the mechanism
behind failure-matrix row P8. The one interaction this design must guarantee
by construction: **no operation added for parking (staging-buffer writes,
sequencer park/signal, the generalized `FStore::begin` check) may block the
owner thread** — all of it must be ordinary non-blocking coroutine logic
posted on the same `context_`, exactly like today's single-TX path, so that
lifecycle RPCs interleave with it exactly as they do today and never see
additional latency attributable to pipelining. §5.2/§5.4 must specifically
test lifecycle-RPC latency under a fully-saturated (`K`-deep, all lanes armed)
sidecar to confirm this.

**Telemetry / trace additions.** Production trace emission already lives
inline in the protocol code — `record(ActionType::TX_BEGIN, ...)` at
`cache/p50_slice0.cpp:1918` and `:2354`, `cache/p50_endpoint.cpp:1869`,
`:2939`, and `record(..., ActionType::COMMIT_ACCEPTED, ...)` at
`cache/p50_endpoint.cpp:3859` — consumed by both the production-side checker
`cache/p50_actions.cpp` (an in-process C++ port of the same per-action
state-machine checks `cache/formal/check_trace.py` applies to replayed JSONL)
and by `check_trace.py` itself for offline/CI replay. `ActionType` (`cache/p50_actions.h:14-29`)
has 15 members: `SESSION_OPENED, SESSION_REPLACED, SESSION_DISCONNECTED,
HISTORY_RESET, TX_BEGIN, TX_ABORTED, BODY_COMPLETE, NEED_RECORDED,
OBJECT_APPLIED, INPUT_MATERIALIZED, INPUT_COMMITTED, COMMIT_ACCEPTED,
ACTIVE_REPLAYED, LOST_COMMIT_ACCEPTED, F_STORE_INCAR_REPLACED` — a superset of
(and, confirmed, identical in spirit to) `check_trace.py`'s 14-action
vocabulary (that file lacks `F_STORE_INCAR_REPLACED`, a C++-side-only action).
No new `ActionType` is required by this design — every lifecycle event under
K>1 is still exactly one of these 15 (a parked TX still eventually emits
`TX_BEGIN`, then either `INPUT_MATERIALIZED`→`INPUT_COMMITTED`→`COMMIT_ACCEPTED`
or `TX_ABORTED`, just later than it would at K=1). What **does** need a new
field is `ActionRecord` itself (`cache/p50_actions.h:37+`): today's checkers
assume at most one outstanding `rel_seq` per side and infer ordering purely
from arrival order on one shared stream per connection; under K>1, trace
consumers need an explicit way to tell "TX n+1's `TX_BEGIN` arrived while
parked, before n's `COMMIT_ACCEPTED`" apart from a same-shaped trace at K=1
(where that interleaving cannot occur) — otherwise `check_trace.py`'s
generalized queue-membership checks (§2 invariant 8, §5.1) cannot distinguish
"successor admitted" from "successor already active," which they must, since
only one of those states is allowed per TX to touch `materialize_and_verify`.
The minimal addition: a boolean or small enum `parked: bool` (or
`stage: {PARKED, SEQUENCED}`) alongside each F-side `TX_BEGIN` record,
set at the exact point `FStore::begin`'s generalized admission check (§1.2)
places it in the staging map rather than handing it directly to the shared
deserializer. No digest/codec/frame change — purely an added observability
field on the existing action-log plumbing.

## 5. Test plan

### 5.1 Unit tests

The existing `unittests/` tree already has targeted coverage for every layer
this design touches; the plan is to extend each in place (parameterize by
`K`), not add a parallel test tree:

- **`p50_slice0_test.cpp`** (covers `CRoute`/`FStore` directly, §1.1 point 2 /
  §1.2 "today's single-slot enforcement"): add cases that drive `FStore::begin`
  through `K` admissions before any commit, assert the generalized occupied-
  slot check (§1.2 (a)) accepts exactly `rel_seq ∈
  [nextExpected, nextExpected+K-1]` and rejects outside it; assert
  `commit_input` releases the sequencer's park signal for the immediate
  successor only (not all parked successors at once).
- **`p50_route_owner_test.cpp`** / **`p50_role_owner_test.cpp`**: extend to
  drive `K`-deep `outstanding`/`active_queue_` FIFOs (§1.1 point 1-2) through
  normal advancement and through every row of the §3 failure matrix — in
  particular P3/P5 (verify the *entire* FIFO is invalidated, not just the
  head) and P6 (verify successors are invalidated even though the triggering
  TX's digest was in fact correct, per the conservative §0 mandate).
- **`p50_profile_test.cpp`**: extend `P29V1LiveDialogue` tests to confirm the
  single shared `state_`/`store_` instance (§1.2 (c)) is only ever driven in
  strict `rel_seq` order by the new sequencer, including a test that
  attempting to call `begin()` out of order (bypassing the sequencer) still
  hits the existing "P29V1 TX_BEGIN arrived while dialogue is active" guard —
  i.e. the sequencer is additive, not a replacement for the existing
  single-dialogue guard, which remains the last line of defense.
- **`p50_endpoint_test.cpp`** / **`p50_endpoint_run_cancel_test.cpp`**: extend
  `run_connected` tests (both `P50ClientEndpoint` and `P50ServerEndpoint`
  sides) to open `K` connections per route and assert: (a) `K=1` byte-for-byte
  reproduces every existing assertion in these files unchanged (the primary
  regression gate, §2 invariant 12); (b) lock hold time (§1.1 point 5 / §4) is
  bounded to prepare+encode+write and does not include the STATE/COMMIT wait,
  measured directly rather than inferred.
- **`p50_source_arm_wait_test.cpp`** / **`p50_source_arm_live_wait_test.cpp`**
  / **`p50_source_arm_wire_test.cpp`**: extend to assert the §4 clamp — `K`
  must not exceed `armed_window_slots()` for a lane — including a negative
  test that an out-of-range `ICECC_P50_ROUTE_PIPELINE_DEPTH` is clamped with a
  warning, mirroring the existing `P50_ROUTE_LANES_CLAMPED` pattern.
- **`p50_sidecar_lifecycle_test.cpp`** / **`p50_daemon_sidecar_adapter_test.cpp`**:
  add the §3 P8 regression directly here — drive a parked connection that
  intentionally delays its read-drain and assert `input_lifecycle_timeout` is
  **not** affected (lifecycle RPC latency unchanged versus a `K=1` baseline
  run in the same test binary) — this is the single highest-value new test in
  the whole plan, since it is the one failure mode genuinely novel to this
  design (§3 summary).
- **`p50_s2_restart_replay_test.cpp`**: extend to drive a sidecar restart
  (§3 P7) while `K>1` TXs are outstanding on one route and confirm every other
  route on that sidecar also cold-reconnects, matching the unchanged blast-
  radius argument in §3's P7 row.
- **New test file** (only new file proposed in this plan):
  `p50_route_pipeline_depth_test.cpp` — a focused test owning the cross-cutting
  assertions that don't naturally belong to one existing file: `K=1`
  byte-identical wire capture diffed against a pre-pipelining capture; `K`
  env-var parsing/validation; the full §3 failure matrix driven end-to-end
  against a fake F (reusing the existing fake-sidecar test harnesses,
  e.g. `p50_fake_sidecar_eof.cpp`'s pattern) rather than against each
  individual component test.

`cache/formal/check_trace.py` and its C++ counterpart `cache/p50_actions.cpp`
both need the §2 invariant 8/9 generalization (scalar `pending`/`active` →
queue-membership, reconciliation witness "ahead by exactly 1" → "ahead by at
most `K`") and the §4 `parked`/`stage` field support — these are code changes
to the *checkers*, reviewed and tested the same as production code (their own
existing self-tests, e.g. driven via `p50_trace_check.sh` and
`p50statejsonl-warn.sh`, are extended with `K>1` fixture traces).

### 5.2 Sim (`cache/sim/p50sim.cpp`)

Today's sim (`run_codec`, `run_batch`, loopback TCP, `cache/sim/p50sim.cpp:674`/`1212`)
is fully serialized — one TX's `run_one`-equivalent lambda completes before the
next begins — and has **no fault-injection mechanism** (confirmed by direct
reading; there is no hook analogous to the mutant-config-driven fault
injection the TLA+ side has). Two changes are needed, both additive:

1. **Pipelined batch mode**: extend `run_batch` (or add a sibling entry point
   selected by a new `Arguments` field, e.g. `--pipeline-depth K`) to open `K`
   connections ahead and let their BEGIN/BODY/FILL writes and STATE/COMMIT
   reads interleave on the existing loopback transport, rather than fully
   serializing. This is the only way to exercise the actual *overlap*
   (as opposed to just the bookkeeping) outside a real two-host farm run.
2. **Fault injection hook**: a minimal one — the sim does not need the TLA+
   model's full mutant taxonomy, only enough to drive each §3 row at least
   once: inject a digest mismatch on a chosen `rel_seq` (drives P3/P5),
   inject a connection drop before/after a chosen write (drives P2/P6), and
   inject an artificial read-drain stall on a parked connection specifically
   to validate the P8 non-interaction property from §5.1's highest-value
   test, this time measured against real (loopback) socket backpressure
   rather than a mocked timeout.

Given the sim's existing single-file, lambda-heavy structure, both additions
are scoped as new code paths gated behind the new CLI flag, not a rewrite of
`run_batch`'s existing serialized path (which remains the `K=1` default and
thus the regression baseline).

### 5.3 TLA+

Core model to extend: **`Protocol50ZstdRoute.tla`**
(`cache/formal/`, 108 KB) — generalize `SingleUnresolvedTU`
(lines 2602-2604) and `OutstandingCodecJobsBlockReuse` (lines 2584-2593) from
scalar to bounded-by-`K` collections, per §2 invariants 1-2, as a new model
constant (e.g. `PipelineDepth`) rather than a hardcoded `2`/`3` — the model
should be checked at `PipelineDepth = 1` (must be isomorphic to today's
checked model — the TLA+-level analogue of §2 invariant 12) and at small `K>1`
(2-3; full state-space checking at the farm's real `K` is expected to be
infeasible, so this is a representative-small-`K` check, not an exhaustive
one).

The two previously-unexamined files are directly relevant and now confirmed
by inspection:

- **`Protocol50ZstdRouteFInputComposition.tla`** (50 KB) composes the core
  per-route model (`core`, instantiating `Protocol50ZstdRoute`) with an
  **owner-side input-interface model** (`owner`, instantiating
  `Protocol50ZstdRouteFInputInterface`) via actions named
  `ObservePreparedSync`, `CancelAfterTouchSync`, `SelectCommitSync`,
  `FCommitAndPublish`, `OwnerLateClose`/`OwnerNormalClose`,
  `ResetCommitAfterOwnerCancel`, `ResetCommitWithoutOwner`, and
  `RejectStaleOwnerEvent` (names read directly from the file). This is the
  exact composition surface §1.2's parking design must be reflected in: a
  parked TX is precisely "input has been touched/received by the owner side
  but not yet synced/selected into the core route's committed state" — a state
  this file's `TerminalResidualCore`/`CoreIndependent`/`ContextLossWithoutPreparedOwner`
  predicates already partially describe for `K=1`'s single-slot case. The
  concrete TLA+ work: widen the `owner` instance's prepared/touched state to a
  bounded sequence of up to `K` pending syncs, and re-derive
  `ContextLossAfterPreparedSync`/`RejectStaleOwnerEvent` for "which of the `K`
  pending syncs does a given owner event apply to" — directly analogous to the
  C++ sequencer's per-`rel_seq` park map (§1.2).
- **`Protocol50ZstdRouteFInputInterface.tla`** (12.6 KB) is the smaller model
  `FInputComposition` instantiates as `owner`; it is the one to extend first
  (narrower surface) before propagating the generalization up into the
  composition file.

Mutant coverage: `s6_mutation_manifest.jsonl` (65 KB) is the master list of
named mutants exercised against these models; new mutants specific to this
design (e.g. "sequencer releases a parked TX before its predecessor commits,"
"parking map entry not cleared on cold reset" — directly the P3/P5 new
mechanics from §3) should be added here following the file's existing naming
convention, run through the existing `run_formal_aggregate.py` harness
unchanged.

### 5.4 Farm A/B plan

- **Baseline**: current `perf-combo` build (`a348414e`) at `K=1`, unchanged
  wire behavior, as the control.
- **Treatment**: same build plus this design, `K` swept over a small range
  (2, 3, 4) via `ICECC_P50_ROUTE_PIPELINE_DEPTH`, on the existing subfarm
  topology (H1/H2 only, per standing constraints — no H4).
- **Primary metric**: per-route TX/s and farm-wide TU/s, compared against the
  §0 baseline figures (~13-15 TX/s/route, ~41 TU/s farm-wide) and target
  (~1/max(C local, F local) per route — ~51 TX/s on the slower host at
  saturation).
- **Guardrail metrics** (regressions here fail the A/B regardless of
  throughput gain): `input_lifecycle_timeout` miss rate (§3 P8, §4) must be
  statistically indistinguishable from the `K=1` baseline; sidecar
  `fail_stop`/retirement rate must not increase; `EndpointCodecPool`
  (`kWorkerCount=2`, `kMaxOutstanding=8`, §1.2) queue-full rate
  ("endpoint codec queue is full") must not increase disproportionately to
  the TX-rate increase (a proportional increase is expected and acceptable;
  a *super*-proportional increase indicates the pool, not the route lock, is
  now the bottleneck and `K` tuning guidance needs to account for it).
- **Fault-injection leg**: repeat the above with the sim's new fault-injection
  hooks (§5.2) replayed against the real farm topology at small scale (a
  handful of routes, not the full farm) to validate the §3 failure matrix
  under real network conditions, not just loopback — specifically the P6
  (`LostFinalAcknowledgement`-triggers-full-chain-invalidation) and P8
  (parking-does-not-stall-owner-thread) rows, which are the two rows this
  design's correctness most hinges on.
- **Rollback plan**: `K=1` via env var is the kill switch at every layer (§0,
  §2 invariant 12) — no code rollback needed, only a config change, if the A/B
  surfaces a regression.

## 6. LOC estimate and implementation order

(TODO)

## 7. Open questions for sorbet-main

(TODO)
