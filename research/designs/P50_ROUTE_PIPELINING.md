# P50 Route Pipelining — Design Doc (DRAFT, WIP)

Status: COMPLETE — addresses sorbet-main's correctness review of `cacae856`
in full. §1.2 (F-side parking) is rewritten end-to-end (raw frame buffer +
peek-only `rel_seq` peek + per-route sequencer, parking before any semantic
processing); §2 (invariant 8 corrected to C-side-only queue generalization),
§3 (failure matrix simplified for the new parking model), §4 (ARM window
corrected to the farm's real value, K-forces-lanes=1 clamp, write-complete
signal split, owner-thread CPU budget), §5 (test plan rebuilt around the raw
buffer/sequencer design), §6 (LOC table rebuilt, ~1400 LOC / 11 steps), and §7
(all six open questions converted to settled decisions, plus a new §8 citation
note) are all updated and now mutually consistent with the revised §1.2. No
implementation code in this tree change; this document only.

**Errata (2026-10-08, during Step 2 implementation):** §1.1 diff item 3 and
§2 invariant 2's "same 4 layers" claim were wrong about `P29Serializer`.
`P29Serializer` stays **scalar** — it is never ring-buffered. The guard that
actually fires, observably, on a second `begin_tu()` is `P29Serializer`'s
own `pending_.active` check (`cache/codec/p29_wire.h`, in `begin_tu()`):
it runs before any mutation and before `OnlineS1::matcher_.prepare()` is
ever reached, and calls `fail("P29 serializer already has a pending TU")`,
which throws `std::runtime_error`. `OnlineS1::matcher_`'s own single-pending
reentrancy guard (`cache/codec/p29_online_s1.h`: `prepare()` throws
`std::logic_error("S1 prepare while a transaction is already pending")` if
called again before `commit()`/`abort()`) is a secondary, normally
unreachable defense-in-depth layer underneath it — it would only fire if
`P29Serializer`'s own flag were ever desynced from `matcher_`'s state.
Neither guard is touched by this design. This is not a gap: §1.1 point 2's
`predicted_cursor_` already advances "immediately after each *local*
`P29Serializer::commit()`," and point 5 already gates TX `n+1`'s BEGIN on
"TX `n` already committed *locally* on C" — so under K>1, TX `n`'s
`P29Serializer::commit()` always runs, on C, strictly before TX `n+1`'s
`begin_tu()`. Serializer `Pending` occupancy is therefore ≤1 by
construction for any `K`, and the existing single-pending guard is exactly
the invariant the whole design leans on at this layer, pinned by a test
(§5.1/`unittests/codec_wire_test.cpp`) rather than widened. See the
corrected diff item 3 and invariant 2 below, and the new paragraph after
diff item 6 on where `P29Serializer::commit()`'s call site moves under K>1.

**Errata (2026-10-08, during Step 4 sub-step 4d review):** §1.1 diff item 4
overstated the required change. `ClientIoState`/`ClientRunSocketTarget`
(`cache/p50_endpoint.cpp:277`/`289`) are already constructed fresh per
`run_connected()` call (`auto io = std::make_shared<ClientIoState>(executor);`)
— never stored on `Impl` — so "each entry owning its own `ClientIoState`
(socket)" is not a new requirement; that part of item 4 is already true
today and needs no change. The actual required change is narrower:
`Impl::active_session`/`active`/`queued` become a bounded **window**: a HEAD
(the oldest outstanding dialogue) plus up to `K-1` SUCCESSORS, ordered by
`rel_seq` — not a generalized map with its own reconcile logic per entry.
Corrected item 4 text below.

This also settles the open question of how the STATE/COMMIT reconciliation
branches (exact/`same_f` match, `HISTORY_RESET` fallback,
`LOST_COMMIT_ACCEPTED` replay detection, `accept`/`stamp`/`live_identity`)
behave under `K>1`: they stay keyed to the HEAD only, and run **only** when
the window holds no successors — i.e. they degenerate exactly to today's
`K=1` path whenever a successor exists; nothing about them is generalized
to multi-dialogue. A successor only ever exists on the deviation-free
straight line opened by `pipelined_begin` (diff item 5); it never enters
any reconcile branch.

Any deviation — on the head or on any successor, while the window holds at
least one successor — is handled conservatively, not by generalizing
reconcile: (i) close all successor sockets (F bulk-releases the
corresponding parked BEGINs via its own mismatch path, independently, in
§1.2/Step 5); (ii) clear the window and `CRoute::active_queue_` in one shot
via the existing whole-queue `abandon_active()` (`cache/p50_slice0.cpp`);
(iii) run the existing full C-side route history reset
(`Impl::reset_history`, `cache/p50_endpoint.cpp:2803-2830`) so the next
transfer starts cold at `K=1`; the window only refills once that TX commits
cleanly. Failing TX(s) return their existing failure statuses — callers
retry/fall back exactly as today. This is consistent with touch-up (a)
below (every fault is a full route history reset, never a partial unwind)
and with the TLA model's `Fault` action (cold reset on both sides, epoch
bump, stale-epoch traffic rejected); it is not a new rule, only a
restatement of (a) scoped explicitly to the window.

Persistent cursor fields (`history_nonce`/`next_rel`/`state`/`route_known`/
`route_f_fingerprint`) keep CONFIRMED values exactly as today, advanced
only by the HEAD's real COMMIT; successor BEGINs use PREDICTED values from
`CRoute`'s `predicted_cursor_`/`predicted_post_digest` (diff item 5), never
touching the confirmed fields. Retirement goes strictly in `rel_seq` order
through the diff-item-1 reorder buffer.

None of this changes `K=1`: the window never holds a successor at `K=1`,
so this whole paragraph is dead code there, and
`test_p29v1_k1_pipeline_wire_identity` (§5.1, part of 4a) remains the
byte-identity gate.

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
3. **(Corrected 2026-10-08 — see errata above.)** `P29Serializer::Pending`
   (`cache/codec/p29_wire.h:~1201-1232`) is **not** changed and stays scalar.
   `OnlineS1::matcher_` is a single, non-reentrant instance per `P29Serializer`
   (`cache/codec/p29_online_s1.h`); its `prepare()` unconditionally throws if a
   prior `prepare()` hasn't yet been followed by `commit()`/`abort()`. Ring-
   buffering `Pending` without also making `matcher_` reentrant per-slot would
   either be dead capacity (occupancy never exceeds 1, by diff item 2/5's
   ordering) or silently wrong (a second `begin_tu()` while occupancy is "1 of
   K" would still throw). Neither this file nor `p29_online_s1.h` is touched by
   this design; the serializer's existing "at most one pending TU" guard is
   relied upon, not generalized — see invariant 2's correction in §2 and the
   paragraph after diff item 6 below.
4. **(Corrected 2026-10-08 — see errata above.)** `P50ClientEndpoint::Impl`
   (`active_session` at 1892, `next_rel` at 1898, `state` at 1899): scalar
   `active_session`/`active`/`queued` → a bounded **window** of up to `K`
   outstanding dialogues, ordered by `rel_seq`: a HEAD (the oldest, today's
   sole dialogue) plus up to `K-1` SUCCESSORS. `ClientIoState`/
   `ClientRunSocketTarget` need **no** change — they are already constructed
   fresh per `run_connected()` call, never stored on `Impl`. This is
   required, not optional: `async_wait_peer_close`'s guard at
   `cache/p50_endpoint.cpp:788-802` ("message arrived after final TX_COMMIT"
   at line 799) structurally forbids sending a second BEGIN on one
   already-committed connection, so TX `n+1` *must* use a brand-new TCP
   connection — confirmed independently from both the C-side and F-side
   audits. `run_connected`'s guard at 3339-3340 becomes "already have `K`
   outstanding dialogues for this route." All existing reconciliation
   machinery (exact/`same_f` match, `HISTORY_RESET` fallback,
   `LOST_COMMIT_ACCEPTED` replay detection, `accept`/`stamp`/`live_identity`)
   stays keyed to the HEAD only and runs only when the window holds no
   successors; see the errata above for the any-deviation-while-successors-
   exist handling.
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

**Where `P29Serializer::commit()` is called changes under K>1 (corrected
2026-10-08, replaces the ring-buffer framing above).**
- **`K=1`:** unchanged, exactly where it is today — inside `accept_commit`
  (`cache/p50_slice0.cpp:2112-2135`), gated on F's real `TX_COMMIT` ack.
  Byte-identical behavior.
- **`K>1`:** called *locally* on C, right after the FILL write completes and
  before the route lock is released (diff item 5's write-completion point,
  ~3555), feeding `predicted_cursor_`. `accept_commit` then becomes
  **confirm-only** at `K>1`: reorder buffer (touch-up (b)) → head-of-queue
  `rel_seq` check → compare F's committed digest against this entry's
  `predicted_post_digest` → advance `confirmed_cursor_`. Any mismatch
  invalidates the whole chain via the existing full `reset_history`
  (touch-up (a)) — never a partial unwind. This is the mechanism invariant 3
  (§2) refers to when it says only *when* C computes the asserted digest
  changes, not the check itself.

**C-side touch-ups required by sorbet-main's review (additive; §1.1 above is
otherwise accepted unchanged):**

(a) *No partial rollback is ever needed.* Every fault takes the route through
a full history reset, discarding whichever uncommitted `n, n+1, …` state
existed, wholesale, never via a partial unwind — this remains exactly true
under K>1, confirmed on both sides of the wire:
  - F side: `CRoute::reset_history` (`cache/p50_slice0.cpp:2145-2154`) calls
    `abandon_active()` and then `p29v1_.reset()` — the entire P29V1 F-side
    codec view is destroyed and rebuilt, not patched.
  - C/F-endpoint side: `Impl::reset_history`
    (`cache/p50_endpoint.cpp:2803-2830`) replaces the whole `Route` entry via
    `space.routes.insert_or_assign(*session.profile, Route{...})`, with
    `.dialogue = nullptr` as part of the fresh struct — the shared
    `P29V1LiveDialogue` pointer is dropped, not reset in place.
  Under K>1 this is unchanged: §2 invariant 6's "any mismatch invalidates all
  outstanding successors" always discards the *whole* outstanding chain via
  one `reset_history`/cold-store call, exactly the all-or-nothing reset
  already in place at `K=1`.

(b) *`accept_commit` must retire strictly in `rel_seq` order.*
`CRoute::accept_commit` (`cache/p50_slice0.cpp:2112-2132`) has no reordering
logic today — it validates against the single `active_` slot and
unconditionally does `++next_rel_seq_.value; active_.reset();`. Under K>1,
COMMITs for `n` and `n+1` arrive on two different sockets and can be observed
out of order by C's coroutines (no ordering guarantee between two
independent connections' read-ready callbacks). Fix: a small reorder stage in
front of today's `accept_commit` body, keyed by `rel_seq` — an early-arriving
`n+1` ack is buffered (not applied) until `n`'s ack has retired
(`next_rel_seq_` advanced past `n`); only then is the buffered `n+1` ack
replayed through the existing, unchanged `accept_commit` body. This reuses
the same `rel_seq` keying as `active_queue_` (§1.1 point 2), so it is a
small addition to the same data structure, not a new concept.

**Correction (2026-10-08):** at `K>1`, "the existing, unchanged `accept_commit`
body" above is replaced by the confirm-only body described in the new
paragraph after diff item 6 (compare F's digest to `predicted_post_digest`,
advance `confirmed_cursor_`) — the reorder stage in front of it is otherwise
exactly as described here, unchanged.

### 1.2 F side: parking (revised per sorbet-main review of commit `cacae856`)

**Today's server-side flow** (`P50ServerEndpoint::run_connected`, server-side span
`cache/p50_endpoint.cpp:4136-4557`, one TX per connection): accept
(4301-4305) → HELLO (4306-4314) → `stage()` + STATE write (~4321-4324) →
optional HISTORY_RESET (4332-4343) → BEGIN (4356-4373) → BODY/NEED/FILL loop
(4382-4421) → `begin_materialization` (4430) → `async_materialize` (4433, def
508-596) → `finish_materialization` (4439, def 3018) →
`select_materialized_job_state` (4444, def 3084) → `commit_materialized`
(4453, def 3103) → COMMIT write (4463-4466) → wait-peer-close (4485-4487, def
788-802) → disconnect+close (4492/4494) → `co_return` (4499).

**Today's single-slot enforcement (unchanged by this design — restated for
reference, since the sequencer below sits strictly *in front of* all of
this, never inside it):**

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
  specifically 2609-2613) does that.
- `P29V1LiveDialogue` (`cache/p50_profile.cpp`): single `state_` field (default
  `State::Idle`, line 16/273) and a single `FStore store_` member (line 264,
  one dialogue+codec view per *route*, not per TX). `begin()` (32-64) guard at
  40-41 ("P29V1 TX_BEGIN arrived while dialogue is active"); `commit_visible()`
  (133-145) requires `Materialized` and resets to `Idle` via `clear_per_tu()`
  (139-140, def 238-245).
- `async_wait_peer_close` (**788-802**): throws "message arrived after final
  TX_COMMIT" (line 799) if any byte arrives after COMMIT on the same connection —
  the exact code that structurally forbids reusing one connection for two
  TXs, confirming §1.1 point 4's "new connection per outstanding TX" requirement
  from the F side too.
- Separate, pre-existing bound worth flagging: `EndpointCodecPool`
  (**462-490**), `kWorkerCount = 2` (464), `kMaxOutstanding = 8` (465),
  `try_acquire()` (467-476, throws "endpoint codec queue is full" at 539) — an
  *existing*, separate concurrency ceiling (2 worker threads, 8 outstanding)
  that already tolerates more than 1 outstanding materialize call across
  *different* routes; K>1 on one route adds load to this same pool and is
  checked against it (§4, §7(5)), but does not require changing it.

**What the first pass of this document got wrong.** The original §1.2 had F
run TX `n+1` through the whole BODY/NEED/FILL loop (4356-4421) "exactly as
today" and park only in front of `begin_materialization` (4430). That is
unsound: two things *earlier* in that span already read the shared, scalar
per-route cursor, which is only correct once TX `n` has committed —

- The STATE write itself: `impl_->stage(session, hello, selection)`
  (`cache/p50_endpoint.cpp:2593-2618`), called unconditionally right after
  HELLO (~4322) and written to the wire immediately
  (`co_await async_write_message(socket, state, ...)`, ~4324), computes
  `state = snapshot(hello, selection)` — TX `n`'s *pre*-commit cursor, if a
  successor arrives before `n` commits. Under the first draft this stale
  STATE would already be on the wire by the time F "decided" to park, out of
  order with its own later processing.
- `Impl::prepare_begin` (`cache/p50_endpoint.cpp:2835-2879`), reached via
  `prepare_begin_candidate`/`install_begin` (2879-2924) off the BEGIN frame,
  has the three-way cursor check at **2842-2844**:
  ```cpp
  if (begin.history_nonce != route.nonce || begin.rel_seq != route.next_rel ||
      begin.pre_state_digest != route.state)
      throw std::logic_error("TX_BEGIN differs from the F route cursor");
  ```
  `route.nonce`/`route.next_rel`/`route.state` are exactly the shared,
  synchronous cursor fields that only become correct after TX `n` commits —
  TX `n+1`'s own `pre_state_digest` is checked against TX `n`'s *pre*-commit
  value if this runs before `n` commits, so it fails 2842-2844 (the
  endpoint-level twin of `FStore::begin`'s 2324-2326 check one layer down).
- The NEED computation inside the BODY/NEED/FILL loop
  (`cache/p50_endpoint.cpp:~4382-4392`): `pending.dialogue->need_messages(...)`
  reads `pending.dialogue`, which for P29V1 *is* `route.dialogue` — the same
  shared, per-route object reused across TXs (`prepare_begin` wires it up via
  `if ((begin.profile == ProfileId::P29V1 ...) && route.dialogue)
  result.pending.dialogue = route.dialogue;`). Computing NEED against it
  before TX `n`'s regions are applied produces a NEED list that will not
  match the FILL C builds against its own *post-n* prediction — `validate_need`
  is exact, so this mismatches.

That is why the original design had to widen `FStore::begin` into a bounded
occupied-slot window, split `Namespace::Route.pending` into a per-`rel_seq`
map, add a per-connection staging buffer, and add a parking map with a
success/failure variant on the park signal. All of that machinery existed
only to let the *shared-state-reading* parts of the pipeline still run
early, for a parked TX. The fix below removes the need for any of it by
never running those parts early in the first place.

**Revised design: raw frame buffer, peek-only `rel_seq`, park before any
semantic processing.**

1. **Drain into a raw buffer, no decoding.** `run_connected` reads and
   decodes `SessionHello` exactly as today (4306-4314 — HELLO carries no
   route-cursor-dependent field, so nothing to defer here). It then drains
   the socket into a per-connection **raw frame buffer** — frame-boundary-
   aware (reusing `FrameParser`/`decode_frame_header`,
   `cache/protocol50.h:287-301`, so frame headers are parsed but payloads are
   not interpreted), with *no* call into `impl_->stage`,
   `prepare_begin_candidate`, `install_begin`, `append_body`, or
   `need_messages` — none of the functions that read shared route state run
   yet. The buffer is bounded by the TX's own self-declared wire size: once
   the first `TX_BEGIN` frame (step 2) has been peeked, its
   `raw_bytes`/`body.encoded_bytes` fields (`cache/protocol50.h:229-240`,
   plus the frame-header/NEED-frame overhead already accounted for by
   `kMandatoryControlFramePayload`, `protocol50.h:113`) give an exact,
   protocol-native ceiling for everything that follows in this TX; exceeding
   it fails the connection closed — the same severity class as today's
   existing decoder bounds (e.g. `FillStreamDecoder::max_record_bytes`,
   `protocol50.h:364-374`). This drain is driven by the *same* async read
   loop as an unparked connection, never a blocking read — see point 7 (P8).
2. **Peek, don't decode-and-act.** The first `TX_BEGIN` frame in the buffer
   (immediately, or after a leading `HISTORY_RESET` frame — a connection
   that opens with HISTORY_RESET is always rooted at `rel_seq=0` and so never
   needs to park, by construction) is read with the same pure
   `decode_as<TxBegin>` the live path already uses, purely to extract
   `rel_seq`. This reads bytes already in the buffer, has no side effects,
   and does **not** call `prepare_begin`/`install_begin`.
3. **Admission decision.** Read `route.next_rel`/whether the route already
   has an active (released, mid-processing) TX — a cheap, side-effect-free
   peek at the existing scalar `Namespace::Route` fields, **not** the full
   `stage()`/`snapshot()` call:
   - If `rel_seq == route.next_rel` and no TX is currently active for the
     route, proceed now — byte-for-byte today's path, and what `K=1` always
     does.
   - If `rel_seq ∈ (route.next_rel, route.next_rel + K − 1]`, park: register
     this connection on a per-route sequencer keyed by `rel_seq` and return
     control to the async read loop (the raw-buffer drain from step 1
     continues in the background).
   - Otherwise (outside the window, or a genuine desync), proceed now and let
     the existing, unchanged mismatch path handle it — `FStore::begin`
     (`cache/p50_slice0.cpp:2324-2326`) or `Impl::prepare_begin`
     (`cache/p50_endpoint.cpp:2842-2844`) rejects it, producing the ordinary
     `RouteHistoryReset`/`ColdFStore` outcome. No new rejection path: parking
     is purely an *additional* gate in front of the existing one, never a
     replacement for it.
4. **Deferred STATE, then replay through the unchanged path.** For a parked
   connection, `impl_->stage(...)` and its STATE write (~4321-4324) are
   **not** called at accept time — deferred wholesale until release, not
   split. Once the sequencer releases this `rel_seq`: F calls `stage()` now
   (`snapshot()` now reads TX `n`'s *post*-commit cursor — exactly C's
   prediction for TX `n+1`'s `pre_state_digest`), writes STATE, and replays
   the already-buffered frames through the completely **unchanged** path:
   STATE → [HISTORY_RESET] → BEGIN (hitting the same 2842-2844 check, which
   now passes, because the cursor is current) → BODY/NEED/FILL (now
   computing NEED against the post-`n`-commit shared dialogue, matching C's
   FILL) → `materialize_and_verify` → COMMIT. The only new plumbing is a thin
   "read from the buffer first, then fall through to the live socket"
   wrapper around the handful of `co_await async_read_frame(...)` call sites
   in this span — mechanically the same function, fed from a different
   source for frames that already arrived while parked.
5. **Release triggers**, all posted on the owner `context_` (so none of this
   introduces a new thread or a blocking call):
   - The predecessor's `commit_input` (`cache/p50_slice0.cpp:2609-2613`)
     releases exactly `rel_seq+1` — the ordinary, single-successor case.
   - Predecessor abort/disconnect, route terminal (e.g. after
     `materialize_and_verify` sets `p29v1->terminal = true`,
     `cache/p50_slice0.cpp:2573-2582`), `HISTORY_RESET`, cold-store adoption,
     or sidecar stop releases **all** currently-parked connections for that
     route at once. Each then proceeds through the *same* unchanged live path
     as step 4 — it calls `stage()`, gets the real (post-reset/post-failure)
     cursor, and the existing mismatch logic sends it down the ordinary
     reconnect-outcome path exactly as an unparked TX hitting a genuine
     desync would today. **No failure variant is needed on the release
     signal itself** — releasing always means "go run the unchanged live path
     now and let it decide," never "go run it and tell it to fail."
   - The park deadline itself, reusing the same `transfer_deadline` +
     `cancellation_grace` that already bounds `transfer_source_on_owner`
     (`cache/p50_cache_service.cpp:~2461-2464` acquire /
     `~2624-2668` watchdog) — no new timeout concept. If the predecessor does
     not commit before this deadline, the park is abandoned and the
     connection proceeds through the existing deadline-exceeded path
     (`ClientRunStatus::DeadlineExceeded`, `cache/p50_endpoint.cpp:3328-3330`).
6. **What stays unchanged.** `FStore::begin`
   (`cache/p50_slice0.cpp:2318-2358`), `Namespace::Route.pending` (scalar
   `optional<FPending>`), `P29V1LiveDialogue`'s single `state_`/`store_`, and
   F's trace records are all **scalar and untouched** by this design — the
   sequencer's only job is deciding *when* a connection may start the
   existing single-TX-at-a-time machinery, never running two TXs through it
   concurrently or splitting it into multiple instances. F's `TX_BEGIN` trace
   record (`record(ActionType::TX_BEGIN, ...)` inside `install_begin`) is
   emitted at the point `install_begin` actually runs — i.e. at
   release/processing time, in order — exactly as for an unparked TX today.
   No `parked`/`stage` field is needed on the F-side trace (contrast with the
   first draft's §4 proposal, dropped — see the revised §4 below).
7. **Drain-while-parked is retained (P8).** The raw buffer in step 1 must be
   filled by the *same* async read loop used for an unparked connection,
   never by a blocking read — the one requirement carried over unchanged from
   the first draft, because it is still what prevents a parked connection's
   TCP backpressure from stalling the single owner thread shared by every
   route and lifecycle RPC on that endpoint (`endpoint_owner_loop` is
   `context_.run()` on one dedicated thread,
   `cache/p50_cache_service.cpp:1926`; lifecycle RPCs share the same
   `context_`, `:3241`, and must complete inside `input_lifecycle_timeout`
   or the sidecar is retired, `cache/p50_daemon_sidecar_adapter.cpp:3265-3287`).

**Option B (rejected, unchanged conclusion): C accepts a windowed STATE
cursor.** Requires generalizing C's exact-match comparison (3601-3604) to
accept a *range* of valid `(rel_seq, digest)` pairs, and requires F to answer
STATE immediately with a cursor value that is only correct "once TX n
commits" — i.e. F would have to do its own prediction of a not-yet-committed
cursor, duplicating the job C already does — while *still* needing the same
park/release machinery as the revised design above to decide when it is safe
to run `materialize_and_verify` relative to the predecessor. Option B pays
the full cost of the chosen design's F-side machinery *plus* extra C-side
generalization, for no additional benefit — rejected.

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
   **3** of the same 4 layers (§1.1 points 1, 2, 4; §1.2 "`FStore::begin`'s
   occupied-slot check") each independently capping at `K` rather than `1`.
   **Corrected 2026-10-08:** the fourth layer, `P29Serializer`'s own
   `has_pending()`/`begin_tu()` guard (§1.1 point 4 of "today's enforcement,"
   not to be confused with diff item 4), does **not** generalize to `K` — it
   stays "at most one pending TU," by construction, forever (see the errata
   above and the corrected diff item 3). `K=1` still makes every one of the
   3 generalized checks byte-for-byte identical to today's (`size() >= 1` is
   exactly today's `has_value()`/non-null check), and the 4th (serializer)
   check is already byte-for-byte identical at every `K` since it never
   changes. This is the central backward-compatibility argument for the whole
   design: no enforcement site is *removed*, 3 are parameterized by `K` and
   1 is deliberately left at its existing bound of 1.
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
   `LOST_COMMIT_ACCEPTED`) are literally scalar-equality assertions today.
   Per the §1.2 revision, **the F half needs no change at all**: F's
   `install_begin`/`materialize_and_verify`/`commit_input` sequence still runs
   exactly one TX at a time, strictly in order, against the scalar
   `Namespace::Route.pending` — the sequencer only gates *when* that unchanged
   machinery starts, so `f["pending"] == tx` continues to hold exactly as
   written, unmodified. Only the **C half** generalizes — `c["active"] == tx`
   must change to "`tx` is a/the head-of-queue member of a bounded ordered set"
   (`active_queue_`, §1.1 point 2), since C now has up to `K` TXs it considers
   outstanding simultaneously. The underlying semantic invariant — "F only
   materializes/commits a TX that C still considers outstanding, and C only
   accepts a commit for a TX it has not already retired" — is unchanged; only
   the C-side *representation* (scalar vs. queue-membership) changes.
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
- **S3 — parked at F**: TX's connection is open; raw bytes have been drained
  into F's per-connection **raw** buffer (§1.2) — frame-boundary-aware but with
  *no* semantic decode yet: no STATE write, no cursor check, no touch of the
  shared dialogue — waiting on the §1.2 sequencer's per-`rel_seq` release.

There is no S4 ("already independently progressed past its predecessor") by
construction — invariant 4 (§2) guarantees the sequencer never hands a
successor's bytes to the shared deserializer out of order.

| # | Predecessor failure | Trigger / where | Effect on this TX | Effect on successors (S2/S3) | New mechanics needed |
|---|---|---|---|---|---|
| P1 | C-local prepare/encode fails | exception in `prepare_for_route`/`begin_tu()`/`commit()`, before any wire write | existing error path, unchanged | **none exist** — S1 only, nothing to unwind, because successor prepare is gated on this TX's local commit output | none — identical to today |
| P2 | C's wire write of BEGIN/BODY/FILL fails (connection error, partial write) | socket error during §1.1 point 5's write | existing error path for this connection | successors can only be in **S2** (writes are serialized by the lock in order) — simply dropped from the FIFOs, their connections are **never opened** | none — F never saw them, nothing to clean up on F |
| P3 | F rejects BEGIN at the cursor/digest-match check (`cache/p50_slice0.cpp:2324-2326`) — C's prediction was wrong | race/desync or genuine bug; under K>1 this becomes a reachable *expected-under-fault* path, not only a bug signal, because C now writes BEGIN before confirming STATE | reconnect outcome worse than `ExactMatch` (`RouteHistoryReset`/`ColdFStore`) for this TX | successors in **S2**: dropped by C, connections never opened. Successors in **S3**: this is one of §1.2's bulk-release triggers (route goes through `RouteHistoryReset`/`ColdFStore`) — **there is nothing to clear except waking the parked waiters**, since no semantic state was ever touched for them. Each released connection runs `stage()` fresh, gets the real (post-reset) cursor, and hits the *same* 2324-2326/2842-2844 check itself, landing on the ordinary non-`ExactMatch` outcome on its own — no special-casing needed | **none** — the existing bulk-release trigger (§1.2 point 5) already covers this; released successors self-resolve via the unchanged mismatch path |
| P4 | Predecessor is itself a parked successor whose own predecessor already failed (cascading) | recursive case of invariant 6 | n/a — resolved as whichever concrete mode (P1-P8) actually originated the chain | all queued behind it, transitively | none — parking adds no *new* failure mode here, only widens the blast radius of an existing one |
| P5 | `materialize_and_verify` fails (digest/content mismatch, corrupt input) → `route.p29v1->terminal = true` (`cache/p50_slice0.cpp:2573-2582`) | F-side verify, same as today at K=1 | route marked terminal, exactly as today | successors in **S3**: setting `terminal = true` is itself one of §1.2's bulk-release triggers ("route terminal") — the sequencer wakes every parked waiter for the route, each of which proceeds through the *unchanged* live path (step 4) exactly as in P3 above: it calls `stage()`, observes the terminal/reset state, and resolves to the ordinary terminal/cold-reset outcome on its own. **No success/failure variant on the release signal is needed** — release always means "go run the unchanged path and let it decide." Successors in **S2**: dropped by C once it observes the non-`ExactMatch` outcome for this TX | **none** — same bulk-release trigger as P3; no new variant on the wake signal |
| P6 | F commits TX, writes COMMIT, but C's connection drops before C reads STATE back — `LostFinalAcknowledgement` | ambiguous-ack case; F's cursor genuinely *did* advance correctly | today: C reconnects, gets `LostFinalAcknowledgement`, re-syncs via STATE, treats TX as committed | **per the §0 mandate, successors are invalidated too — uniformly, even though this TX's prediction was actually correct.** Flagged as a candidate relaxation in §7, not implemented here | none required for the conservative behavior; a future optimization is out of scope |
| P7 | Sidecar `fail_stop()`/`_Exit(125)` while this TX was active anywhere in the pipeline | deliberate hard-crash to avoid releasing a route gate on ambiguous state | whole process exits, supervisor restarts it | **every route on that sidecar** reconnects cold — already true at K=1 for every *other* route; K>1 only means *this* route loses more unsent, locally-queued work than it would at K=1. Blast radius shape is unchanged | none — mechanism identical, just more work discarded per incident |
| P8 | Lifecycle retirement: missed `input_lifecycle_timeout` → `retire_input_lifecycle_relationship` via `CloseAcceptedJob`/`Timeout` | owner-thread-wide, not route-local | whole relationship retired, same category as P7 | same as P7 | **new risk this design must guard against**: if §1.2's "F must keep draining parked connections at full speed, never block the owner thread" property is violated by a bug, a stalled parked-connection write could itself *cause* a P8 retirement that would not have happened at K=1 — i.e. parking is the one mechanism in this design that could turn a route-local slowdown into a sidecar-wide outage. Mitigated by: no blocking calls on the owner thread's path for a parked connection (reads continue on the existing async loop exactly as for an unparked one), and explicitly probed in §5.2/§5.4 (backpressure injection) |

Summary of the one truly new failure *mode* introduced by this design (as
opposed to existing modes whose blast radius widens): **P8's parking-induced
owner-thread stall.** Every other row is either unchanged from today (P1, P2,
P6's mechanism, P7) or an existing mode whose cleanup, after the §1.2 revision,
reduces to **"wake the parked waiters and let the unchanged live path resolve
each of them"** — P3 and P5 need no new success/failure variant on the release
signal and no multi-entry structure to clear beyond the sequencer's own
wait-list, because no semantic state was ever applied to a parked connection
before release. This narrows what §5 needs to stress-test beyond ordinary
regression to just one thing: backpressure on a parked connection's socket
must never be allowed to block the owner thread.

## 4. Interactions (ARM admission, route lock, deadlines, lifecycle RPCs, telemetry)

**ARM admission window — resolving a figure discrepancy.** The admission
window is `RouteGate::kArmedSessions = 8` (`cache/p50_cache_service.h:276`),
overridable via `ICECC_P50_ARMED_WINDOW` (`armed_window_slots()`,
`cache/p50_cache_service.cpp:2008-2017`). An earlier draft of this task's
brief cited "16" for this window; that figure is a *different*, unrelated
constant — `kMaxBacklog = 16` (`cache/p50_cache_service.cpp:93`), which only
bounds the `--backlog` CLI argument (TCP listen backlog) validated at
`cache/p50_cache_service.cpp:3698-3702`. **The compiled-in default is 8; the
farm's actual runtime value is 16** (`ICECC_P50_ARMED_WINDOW=16` is how the
farm is run today, per sorbet-main). This document uses **16** — the farm's
real operating value — wherever it reasons about admission budget below, and
calls out the compiled-in default of 8 only where the distinction matters.

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
lane already has, rather than requiring a new, additional budget.

**`K>1` forces `lanes=1` (per sorbet-main review 4(4) — the user has already
rejected multi-lane RAM duplication as a tuning axis).** Rather than letting
`K` and `lanes` tune jointly against the shared `armed_window_slots()` budget,
this design simply does not pursue that combination: whenever `K>1` is
configured for a route, `lanes` is clamped to `1` with a warning, mirroring
`route_lane_count()`'s existing `P50_ROUTE_LANES_CLAMPED` pattern
(`cache/p50_cache_service.cpp:2019-2032`). With `lanes` fixed at `1`, the
admission rule collapses to the simple case: **`K` must not exceed
`armed_window_slots()`** for that one lane — at the farm's actual runtime
value of 16 this gives ample headroom over the §7-settled default (`K=1`) and
A/B ceiling (`K≤4`). The joint formula
(`lanes * armed_window_slots() <= max_live_sessions` **and**
`K <= armed_window_slots()`) is explicitly *not* pursued; it would only matter
if a future tuning wanted `K` and `lanes` to grow independently, which the
user has ruled out.

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

**C-side route-lock release point — the "write complete" signal (review point
2c).** In `transfer_source_on_owner`
(`cache/p50_cache_service.cpp`), the per-route lock is acquired around
`~2461-2464` and, as written today, held until the `co_spawn(context_, ...)`
lambda started at `~2534` fully completes — that lambda contains both
`route_owner_->transfer(...)` (the actual FILL wire-write, `~2570-2574`) and
the eventual `completion->set_value(value); co_return;` that signals the
*whole* operation's result. Under K>1 this is too coarse: holding the lock
until the *final result* is known (which includes waiting on STATE/COMMIT
round-trip confirmation) would re-serialize exactly the latency this design
is trying to pipeline away. The fix: split the single `completion` signal
into two —
  1. a **"write complete"** signal (a separate promise/flag), set the instant
     the FILL write itself finishes (right after `~2570-2574`), which is what
     the route lock actually waits on before releasing — this is the
     "guarding prepare+encode+write only" scope already stated above;
  2. the existing **final result** signal (`completion->set_value(value)`,
     `~2611-2612`), set later, once STATE/COMMIT confirmation completes —
     this is what the TX's own outstanding-ness (invariants 2/11, the
     `active_queue_` membership) still waits on, independent of the lock.
The existing `wait_until` + `fail_stop()`/`_Exit(125)` watchdog
(`cache/p50_cache_service.cpp:~2624-2668`) continues to apply per outstanding
TX, unchanged in mechanism: under K>1 there are simply up to `K` concurrently
armed watchdog timers per lane instead of ≤1 (the same quantitative-not-
structural increase already noted above for `transfer_deadline`), each
independently guarding its own TX's confirmation deadline — a watchdog firing
for TX `n` still triggers the same whole-process `fail_stop()`, which (§2
invariant 6) is exactly the "discard the whole outstanding chain" case,
unchanged by `K`.

**C-side owner-thread CPU budget (review point 3).** sorbet-main's own
measurement: the C sidecar's owner thread runs at ~47% CPU busy at 41 TX/s
(`pidstat`); doubling the per-route TX rate at `K=2` would approach
saturation — this is the C-side analogue of failure-matrix row P8's
owner-thread-stall risk, not an F-side one. Classifying per-TX work by which
thread it runs on:
  - **Off the owner context, on a per-connection worker thread**:
    `handle_connection` (def `cache/p50_cache_service.cpp:1291`, spawned per
    connection at `:4008-4012`) — `prepare_source`, the OnlineS1 digest pass,
    and encode all run here, each on its own connection's worker thread, not
    serialized against other routes' work.
  - **On the shared owner `context_`/`endpoint_owner_thread_` budget**: the
    `asio::co_spawn(context_, ...)` lambda at `~2534` — i.e. exactly the FILL
    wire-write (`route_owner_->transfer(...)`, `~2570-2574`) and both halves
    of the split completion signal above (`~2611-2612`) — plus STATE/COMMIT
    handling for the route, since both touch the same shared `Route`/`Lane`
    object and must be serialized through the owner context to stay
    consistent with the lock-scope statement above.
  Doubling `K` doubles the TX rate specifically on this second,
  owner-context-bound subset of work — the first (worker-thread) subset scales
  independently per connection and is not the bottleneck sorbet-main
  identified. **Action item, folded into §6's implementation order**: measure
  owner-context CPU per TX at `K=2` in the sim (new §6 step, see below) before
  shipping any default above `K=1`. If the measurement shows the owner
  context is over budget, the mitigation is to move the FILL-write dispatch
  itself off the owner context onto a small worker pool mirroring
  `EndpointCodecPool` (`cache/p50_endpoint.cpp:462-490`, 2 workers / 8
  outstanding) — tracked as a **separate commit**, not part of this design's
  first landing, since §7(5) (unchanged pool policy) already says no pool
  change ships with the initial `K` rollout.

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
K>1 is still exactly one of these 15. **Per the §1.2 revision, the F side
needs no new trace field at all**: F's `TX_BEGIN` record is now emitted at
the point `install_begin` actually runs — i.e. at release/processing time, in
order, exactly as at `K=1` — so a trace replayed at K>1 is, from F's side,
indistinguishable in *shape* from a K=1 trace one TX at a time; there is no
"arrived while parked" interleaving to disambiguate on this side, because
parking happens strictly *before* any semantic record is emitted (the
first draft's proposed `parked`/`stage` field on F's `TX_BEGIN` record is
dropped entirely).

The one place a small additive field is still useful is the **C side**: with
`active_queue_` (§1.1 point 2) now potentially holding up to `K` members
instead of a scalar, `check_trace.py`'s generalized queue-membership check
(§2 invariant 8) is simplest if C's own action records (at TX-write time and
at `COMMIT_ACCEPTED`) carry an explicit queue-position/depth field, rather
than requiring the checker to reconstruct queue order purely from
multi-connection arrival interleaving in the merged trace. This is a single
additive JSON field on existing C-side records — no digest/codec/wire change,
no new `ActionType` — and per sorbet-main review point 4(6), moot in practice
for the initial rollout (§7(1): P29V1-only, other profiles clamped to `K=1`,
so most traces will show the field at its degenerate `K=1` value). Its only
consumers are the in-repo checkers (`cache/p50_actions.cpp`,
`cache/formal/check_trace.py`) and this project's own `analyze.py` — nothing
wire-visible, nothing F-side.

## 5. Test plan

### 5.1 Unit tests

The existing `unittests/` tree already has targeted coverage for every layer
this design touches; the plan is to extend each in place (parameterize by
`K`), not add a parallel test tree:

- **New (2026-10-08 errata): `unittests/codec_wire_test.cpp`**
  (`P29Serializer`, §1.1 diff item 3 as corrected; note this corrects the
  filename from an earlier draft of this errata, which wrongly named
  `unittests/p50_wire_test.cpp` — that file only tests digest-math helpers
  and never instantiates `P29Serializer`; `codec_wire_test.cpp` is the file
  that already exercises `begin_tu`/`answer_need`/`commit`/`abandon`): two
  tests, added in Step 2, with no production change to
  `cache/codec/p29_wire.h`/`cache/codec/p29_online_s1.h`.
  (1) *Pin the reentrancy guard*: call `begin_tu()` twice in a row without an
  intervening `commit()`/`abandon()` and assert it still throws
  `std::runtime_error("P29 serializer already has a pending TU")` —
  `P29Serializer`'s own guard, which fires before `OnlineS1::matcher_` is
  ever reached (see Status-block errata above) — proves the serializer
  layer's bound stays at 1 regardless of `K`. (2)
  *Commit-timing equivalence*: for a sequence of TUs, assert
  `begin_tu()`→encode→`commit()`-immediately (the `K>1` timing, where commit
  runs locally right after encode) produces byte-identical encoded output and
  post-commit state digests compared to `begin_tu()`→encode→commit-only-after-
  the-ack-equivalent-point (today's `K=1` timing) — i.e. moving *when*
  `commit()` is called changes nothing about *what* it computes, since both
  are deterministic replay of the same already-encoded bytes.
- **`p50_slice0_test.cpp`** (covers `CRoute`/`FStore` directly, §1.1 point 2 /
  §1.2 "today's single-slot enforcement"): this file's job under the revised
  §1.2 design is now a **regression** gate, not a widening gate — assert
  `FStore::begin`'s occupied-slot check (`cache/p50_slice0.cpp:2324-2326`) and
  `Namespace::Route.pending` stay exactly the `K=1` scalar logic, byte-for-byte
  unchanged, at every `K`, since the admission widening lives entirely in the
  new F-side sequencer (below), not in `FStore`/`CRoute`'s F half (§2
  invariant 8). Add one explicit test that calls `FStore::begin` twice in a
  row without an intervening `commit_input` and asserts it still rejects the
  second call exactly as it does today — i.e. prove the sequencer is doing
  100% of the new admission work and `FStore` was not touched.
- **New: F-side raw-frame-buffer and sequencer tests** (these belong in
  `p50_endpoint_test.cpp`'s server-side (`P50ServerEndpoint`) cases, since the
  sequencer lives in `run_connected`, not in `p50_slice0.cpp` — see §1.2):
  assert that frames for `rel_seq+1 .. rel_seq+K-1` arriving before the
  predecessor's `commit_input` are drained into the raw per-connection buffer
  with **zero** semantic decode beyond the peek-only `decode_as<TxBegin>` for
  `rel_seq` — no STATE write, no `stage()` call, verified via an
  instrumentation counter rather than by absence of an observable side effect;
  assert release-on-predecessor-commit replays the buffered frames through the
  unchanged STATE → BEGIN → BODY/NEED/FILL → materialize → COMMIT path and
  produces byte-for-byte the same wire trace as an unparked `K=1` arrival
  (this is the correctness crux of the revised §1.2 and the single
  highest-value test added by this revision); assert bulk release
  (`HISTORY_RESET`, cold-store, sidecar-stop, abort/disconnect) drains *every*
  parked connection through that same unchanged path, each self-resolving via
  the existing mismatch handling with no special-cased success/failure variant
  on the release signal itself (§1.2 point 5); assert deadline-based release
  (reusing `transfer_deadline`/`cancellation_grace` — no new timeout concept)
  fires when a predecessor never commits.
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
need the §2 invariant 8/9 generalization **on the C side only** — `c["active"]
== tx` generalizes to "head-of-queue member of `active_queue_`," and the
reconciliation witness generalizes from "ahead by exactly 1" to "ahead by at
most `K`." F's half of the checker (`f["pending"] == tx`) is **not** touched —
it stays the exact `K=1` assertion, since F's `install_begin`/
`materialize_and_verify`/`commit_input` sequence still runs one TX at a time
against the unchanged scalar `Namespace::Route.pending` (§2 invariant 8). The
checkers also pick up the new §4 C-side-only additive queue-position/depth
trace field (no F-side `parked`/`stage` field — F's `TX_BEGIN` is now emitted
in order at release time, so it is indistinguishable from a `K=1` trace and
needs no disambiguation field). These are code changes to the *checkers*,
reviewed and tested the same as production code (their own existing
self-tests, e.g. driven via `p50_trace_check.sh` and `p50statejsonl-warn.sh`,
are extended with `K>1` fixture traces).

### 5.2 Sim (`cache/sim/p50sim.cpp`)

Today's sim (`run_codec`, `run_batch`, loopback TCP, `cache/sim/p50sim.cpp:674`/`1212`)
is fully serialized — one TX's `run_one`-equivalent lambda completes before the
next begins — and has **no fault-injection mechanism** (confirmed by direct
reading; there is no hook analogous to the mutant-config-driven fault
injection the TLA+ side has). Three changes are needed, all additive:

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
3. **Owner-context CPU-per-TX measurement at `K=2`** (new — review point 3 /
   §4's "C-side owner-thread CPU budget" subsection): instrument the sim's
   loopback C-side owner `context_` to report busy-time per committed TX,
   separating the on-owner-context work (FILL write dispatch, both the
   write-complete and final-result completion signals, STATE/COMMIT handling)
   from the off-owner-context worker-thread work (`handle_connection`/
   `prepare_source`/OnlineS1/encode). This is the sim-side half of the action
   item §4 flags against the farm's own ~47%-busy-at-41-TX/s pidstat
   measurement — it is cheap to add here (the sim already runs loopback on
   one host) and gives a repeatable, farm-independent number to check the
   §5.4 A/B's owner-thread guardrail against before spending real farm time.

Given the sim's existing single-file, lambda-heavy structure, all three
additions are scoped as new code paths gated behind the new CLI flag, not a
rewrite of `run_batch`'s existing serialized path (which remains the `K=1`
default and thus the regression baseline).

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
- **Treatment**: same build plus this design, `K` swept over **2 and 4**
  (the settled A/B points per §7(2) — not a continuous sweep; `K=4` doubles as
  the hard max clamp enforced at admission, §6 step 1) via
  `ICECC_P50_ROUTE_PIPELINE_DEPTH`, on the existing subfarm topology (H1/H2
  only, per standing constraints — no H4).
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
  now the bottleneck and `K` tuning guidance needs to account for it); C-side
  owner-thread busy-time per TX (§4's "C-side owner-thread CPU budget"
  subsection, review point 3) must not exceed the farm's existing
  ~47%-busy-at-41-TX/s baseline by more than the TX-rate increase alone would
  predict — a disproportionate rise means the owner-context work (FILL write +
  completion signals + STATE/COMMIT handling) is the new bottleneck and the
  worker-pool mitigation (§4) needs to be pulled forward from "flagged" to
  "scheduled."
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

Estimates are incremental lines (added/changed), not file totals, and assume
each step lands with its own test extension in the same commit (per the
`commit often, tag checkpoints` norm this session already follows for other
work). Ordered so that every step after step 1 is independently buildable and
`K=1`-safe before the next step starts — i.e. the branch is always releasable
at `K=1` throughout implementation, which also means the implementation order
doubles as the rollback-safety argument: stopping after any step leaves a
working, `K=1`-identical system.

| Step | Files | Est. LOC | What lands |
|---|---|---|---|
| 1 | `cache/p50_cache_service.h`/`.cpp` | ~50 | `ICECC_P50_ROUTE_PIPELINE_DEPTH` env parsing (mirrors the existing `ICECC_P50_ARMED_WINDOW`/`ICECC_P50_ROUTE_LANES` pattern exactly). Three clamps land here, all `*_CLAMPED`-style warnings mirroring `route_lane_count()`'s pattern (`cache/p50_cache_service.cpp:2019-2032`): (a) `K <= armed_window_slots()` — the farm's actual runtime value is `ICECC_P50_ARMED_WINDOW=16`, not the compiled-in default of 8 (§4); (b) hard max clamp `K <= 4` regardless of (a) (§7(2)); (c) profile-scoped clamp — `K>1` is only honored when the route's negotiated profile is `P29V1`, every other profile is forced to `K=1` (§7(1)). `K>1` also forces `lanes=1` with its own clamp-with-warning (§4, §7(4)). No behavioral change yet — `K` is parsed and clamped but nothing consumes it. Pure plumbing, trivially reviewable. |
| 2 | `research/designs/P50_ROUTE_PIPELINING.md`, `unittests/codec_wire_test.cpp` | ~30 (tests only; errata is docs) | **Corrected 2026-10-08** (was: ring-buffer `P29Serializer::Pending`, now dropped — see errata; test file corrected from an earlier `unittests/p50_wire_test.cpp` typo to the file that actually instantiates `P29Serializer`). `P29Serializer` stays scalar; `cache/codec/p29_wire.h` is not touched by this step. Lands: (1) a test pinning `P29Serializer`'s own existing single-pending guard (second `begin_tu()` before `commit()`/`abandon()` still throws `std::runtime_error`); (2) a `K=1`-vs-eager-commit equivalence test (begin→encode→commit-immediately produces byte-identical encodings/digests to today's commit-on-ack timing). No production code unless the equivalence test exposes a missing accessor, in which case the addition is minimal and justified in the commit message. |
| 3 | `cache/p50_slice0.h`/`.cpp` | ~130 | **C side only**: `CRoute::active_` → `active_queue_` FIFO (§1.1 point 2); `accept_commit` generalized to retire strictly in `rel_seq` order via a reorder buffer in front of the FIFO head (review point 2b) — a commit that arrives out of order waits in the reorder buffer rather than retiring early. `FStore`/`Namespace::Route.pending` (the F side) is explicitly **not** touched in this step or any other — it stays the exact `K=1` scalar, per the revised §1.2 and §2 invariant 8. Covered by extending `p50_slice0_test.cpp` per §5.1 (including the new regression case proving `FStore::begin` is unchanged). |
| 4 | `cache/p50_endpoint.h`/`.cpp`, `cache/p50_cache_service.cpp` | ~300 | Client-side (`P50ClientEndpoint`) changes: `Impl::RouteState::uncommitted_route_entry` → bounded FIFO (§1.1 point 1); scalar `active_session`/`active`/`queued` → ordered map keyed by `rel_seq`, each with its own `ClientIoState`/socket (§1.1 point 4); extend `pipelined_begin` (§1.1 point 5) to fire for TX `n+1` once `predicted_cursor_` is valid. The **write-complete signal split** (review point 2c): inside `transfer_source_on_owner`'s `co_spawn(context_, ...)` lambda (`cache/p50_cache_service.cpp:~2534`), add a second promise/signal set immediately after the FILL write completes (`~2570-2574`), decoupled from the existing final-result signal (`completion->set_value(value)`, `~2611-2612`); the route lock releases on the **write-complete** signal, not the final result — this is the mechanism that actually lets TX `n+1`'s FILL write start before TX `n`'s STATE/COMMIT confirms. This is the step that changes **C-side wire behavior** for the first time, gated end-to-end behind `K>1` (at `K=1` the new map/FIFO/signal-split code degenerates to exactly the old scalar/single-signal logic, verified by the `p50_endpoint_test.cpp` `K=1`-identity regression from §5.1). Largest and highest-risk step; budget the most review time here, including a direct check against the existing `wait_until`/`fail_stop`/`_Exit(125)` watchdog (`cache/p50_cache_service.cpp:~2624-2668`) to confirm it still fires per *outstanding* TX rather than per route (review point 2c). |
| 5 | `cache/p50_endpoint.cpp` (server side, `run_connected`) | ~170 | The F-side **raw frame buffer + sequencer**, parking before any semantic processing (revised §1.2): per-connection raw buffer bound to frame boundaries via the existing `FrameParser`; a peek-only `decode_as<TxBegin>` to read `rel_seq` without touching STATE or calling `stage()`; a per-route sequencer that holds the buffered frames until release; the three release triggers (predecessor `commit_input`, bulk release on abort/disconnect/route-terminal/`HISTORY_RESET`/cold-store/sidecar-stop, and park-deadline reuse of `transfer_deadline`/`cancellation_grace`); on release, replay the buffered frames through the **completely unchanged** STATE → [HISTORY_RESET] → BEGIN → BODY/NEED/FILL → materialize → COMMIT path. This is the step that makes F-side parking real and safe to enable at `K>1` — steps 1-4 are inert without it. |
| 6 | — | 0 | Folded into step 5: the bulk-release trigger (abort/disconnect/route-terminal/`HISTORY_RESET`/cold-store/sidecar-stop) already drains every parked connection through the unchanged mismatch path with no separate success/failure variant needed (§3 P3/P5) — confirmed by the §5.1 bulk-release test, no additional production code. |
| 7 | `cache/p50_actions.h`/`.cpp`, `cache/formal/check_trace.py` | ~60 | **C-side-only** trace additions: a new additive queue-position/depth field on C's action records (not an F-side `parked`/`stage` field — F's `TX_BEGIN` is now emitted in order at release time and is indistinguishable from a `K=1` trace, §4); both checkers' `c["active"] == tx` assertion generalizes to "head-of-queue member of `active_queue_`," reconciliation witness generalizes to "ahead by at most `K`" (§2 invariant 9); `f["pending"] == tx` is **not** touched (§2 invariant 8). Per review point 4(6), this step is largely moot in practice for the initial rollout since §7(1) clamps every non-`P29V1` profile to `K=1` and only `P29V1` routes ever produce a `K>1` trace to check against. Logically depends on steps 3-5 existing to produce real `K>1` traces to validate the new checker logic against. |
| 8 | `cache/sim/p50sim.cpp` | ~180 | `--pipeline-depth` flag; the digest-mismatch and connection-drop fault-injection hooks (§5.2); **new** — owner-context CPU-per-TX instrumentation at `K=2`, separating on-owner-context work (FILL write + both completion signals + STATE/COMMIT handling) from off-owner-context worker-thread work, feeding the §6 budget question directly (review point 3) rather than waiting for a farm-only answer. |
| 9 | `cache/formal/Protocol50ZstdRouteFInputInterface.tla`, `Protocol50ZstdRouteFInputComposition.tla`, `Protocol50ZstdRoute.tla`, `s6_mutation_manifest.jsonl` | ~250 (TLA+ is verbose) | `PipelineDepth` constant generalization (§5.3), new mutants reflecting the revised §1.2 (sequencer releases a parked TX before its predecessor commits; parking buffer not cleared on cold reset). Can proceed in parallel with steps 2-8 once this design doc is approved — the TLA+ model is a *specification* of the intended design, not a derivation from the C++, so it does not need to wait on implementation. |
| 10 | `unittests/p50_endpoint_test.cpp` additions, `unittests/p50_route_pipeline_depth_test.cpp` (new) | ~200 | Cross-cutting `K=1` identity capture, env-var/clamp validation (window, max-4, profile-scoped, lanes-forced-to-1), the raw-buffer/peek/sequencer tests from §5.1, and the full §3 failure matrix driven end-to-end against a fake F (reusing the existing fake-sidecar test harnesses, e.g. `p50_fake_sidecar_eof.cpp`'s pattern). |
| 11 | farm A/B (§5.4) | 0 (no code; config + measurement) | `K` sweep at 2 and 4, guardrail metrics including the new owner-thread-CPU guardrail, fault-injection leg, go/no-go. |

**Total estimate: ~1400 LOC** across roughly 12 production/test files plus 4
TLA+ files, in 10 code steps + 1 measurement step (down from the first
draft's ~1490 LOC / 10-step estimate — the net reduction comes from step 6
dropping to 0 LOC now that bulk release is confirmed to require no new
production code, offset by the larger step 4 needed for the write-complete
signal split). No step touches `daemon/main.cpp`/`p50_daemon_sidecar_adapter.*`
directly — the lifecycle-RPC timeout constants are *reused* unchanged (§1.2,
§4), not modified, so those files are read-only references throughout, which
is itself a useful confirmation that this design does not touch
sidecar-lifecycle policy, only route-level transfer mechanics.

## 7. Decisions settled by sorbet-main's review

The six items below were open questions in the first draft; sorbet-main's
review settled all of them. Each is now a decision this design implements,
not a question — cross-referenced to where it lands in §4/§6.

1. **Scope: `P29V1LiveDialogue` only.** `ZstdRouteDialogue`/`ZstdTuDialogue`
   are not in scope for this project. Every route negotiated on either of
   those two profiles is clamped to `K=1` unconditionally — this is an
   explicit profile-scoped clamp (§6 step 1), not just an absence of
   implementation work, so a future profile addition cannot silently inherit
   `K>1` behavior it was never designed or tested for. `ZstdRouteDialogue`/
   `ZstdTuDialogue` pipelining, if ever wanted, is a follow-up design, not an
   extension of this one.
2. **`K` default and max.** Shipped default is `K=1` (opt-in only). The A/B
   (§5.4) is run at exactly two treatment points, `K=2` and `K=4` — not a
   continuous sweep. `K=4` doubles as the hard ceiling: every layer clamps
   `K <= 4` regardless of `armed_window_slots()` (§6 step 1), so `K` can never
   be tuned past 4 without a follow-up design change to this clamp itself.
3. **`LostFinalAcknowledgement` chain-invalidation conservatism (§3 P6).**
   Accepted as designed — invalidate all outstanding successors uniformly,
   including the case where the triggering TX's commit later turns out to
   have been correct. The more surgical "re-validate successors against the
   now-confirmed cursor" relaxation is explicitly not pursued; §3 P6 is final
   for this project.
4. **Joint `K` × `lanes` tuning (§4).** Not pursued. Decision: `K>1`
   unconditionally forces `lanes=1`, clamped with a warning (mirroring the
   `P50_ROUTE_LANES_CLAMPED` pattern at `cache/p50_cache_service.cpp:2019-2032`,
   §4, §6 step 1) — reflecting the standing rejection of multi-lane RAM
   duplication as a tuning axis. The two-dimensional clamp formula from the
   first draft's open question is dropped; there is no path to `K>1` and
   `lanes>1` simultaneously in this design.
5. **`EndpointCodecPool` capacity (§5.4 guardrail).** No change to
   `kWorkerCount=2`/`kMaxOutstanding=8` as part of this project. The pool's
   queue-full rate is a guardrail metric (§5.4) only — if the A/B shows a
   super-proportional rise in "endpoint codec queue is full," that becomes its
   own, strictly separate follow-up decision/design, not something
   pre-authorized here.
6. **Trace-format compatibility.** Moot in practice, as a consequence of
   decision 1: the F-side `parked`/`stage` field proposed in the first draft
   was dropped during the §1.2 revision (F's `TX_BEGIN` is now emitted in
   order at release time and is indistinguishable from a `K=1` trace, §4), so
   the only new field is a C-side-only additive queue-position/depth field on
   C's action records (§6 step 7). Its only consumers are in-repo:
   `cache/p50_actions.cpp`, `cache/formal/check_trace.py`, and `analyze.py`.
   No external dashboard/tool outside this repo parses `ActionRecord` JSONL,
   so there is no backward-compatibility exposure to audit further.

## 8. Citation discrepancy with the review (minor)

One citation in sorbet-main's review does not match the code as read in this
worktree: the review places the `wait_until`/`fail_stop`/`_Exit(125)` watchdog
around line "~2147" of `cache/p50_cache_service.cpp`. Direct reading in this
worktree places it at **`~2624-2668`** instead (see §4's "C-side route-lock
release point" subsection, which cites the same span). Line 2147 in this
worktree's copy of the file falls inside unrelated code well before
`transfer_source_on_owner`. This is flagged as a citation-only disagreement —
it does not change any conclusion in the review or this document, since the
design was written against the grounded `~2624-2668` location throughout; it
is noted here only because sorbet-main's review explicitly asked to be told
"anything in this review you think is wrong."
