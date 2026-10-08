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

(TODO)

## 5. Test plan

### 5.1 Unit tests

(TODO)

### 5.2 Sim (cache/sim/p50sim.cpp)

(TODO)

### 5.3 TLA+

(TODO)

### 5.4 Farm A/B plan

(TODO)

## 6. LOC estimate and implementation order

(TODO)

## 7. Open questions for sorbet-main

(TODO)
