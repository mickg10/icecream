# Protocol 49/50 design: fenced assignment (S-F) and exact assignment capability (C-F)

Status: design only.  No product code changes accompany this document.
Baseline: fork `PROTOCOL_VERSION 48` (`services/comm.h:39`), upstream 44,
upstream base commit `37cb407`, integration branch `fix/stage3-scheduler-policy`.
Specification source: `formal/AUDIT_DELTA.md`, `formal/FencedAssignment.tla`,
`formal/TemporalOwnership.tla`, `formal/README.md`,
`formal/FARM_EXECUTION_PROGRAM.md` on `audit/temporal-ownership-model`.

Companion document: [`CLARIFICATIONS_FOR_REMOTE.md`](CLARIFICATIONS_FOR_REMOTE.md)
lists the numbered questions referenced as Q1..Q12 below.

---

## 1. Problem statement and current-wire facts this design builds on

Two defects are in scope:

1. **Dispatched-but-not-started reclamation.**  After `UseCS` is sent, the
   scheduler has debited a dispatch credit and reserved a worker slot, but the
   worker daemon (F) has learned nothing: today F is told about an assignment
   only by the client itself, when the client's `CompileFileMsg` arrives with
   a bare 32-bit job id that F accepts without any validation
   (`daemon/main.cpp:5545` queues any well-formed id as `TOCOMPILE`;
   `daemon/main.cpp:5489` reports `JobBegin` for whatever id was presented).
   The scheduler therefore cannot cancel a dispatched assignment: it can only
   retain it (`scheduler/scheduler.cpp:1646-1698`, the stall-retention bound)
   or delete its own record and accept that a delayed client may still compile
   against a job the scheduler wrote off (`scheduler/scheduler.cpp:2793`
   explicitly leaves this case "to the worker's own input-wait timeout").
   The `formal/README.md` indistinguishability lemma shows timeout-only
   reclamation cannot be safe with the current alphabet; a revocation
   handshake or worker-side tombstone is required.

2. **Scheduler-restart wire-id ABA.**  `new_job_id` is a process-local counter
   (`scheduler/scheduler.cpp:113`, `:727`) with no persistence.  After a
   scheduler restart, ids are reallocated from scratch; a delayed client
   holding a pre-restart `UseCS` can present a wire id that the new scheduler
   has already reused for the same worker, and neither the legacy client nor
   `CompileFileMsg` carries anything that distinguishes the generations
   (`AUDIT_DELTA.md`, "scheduler restart creates immediate wire-id ABA").

Facts about the existing wire that the design must respect:

- Protocol version is negotiated **per link** as the minimum of both ends
  (`services/comm.cpp:225-291`); `IS_PROTOCOL_VERSION(x, c)` is
  `(c)->protocol >= x` (`services/comm.h:52`).  There is no cluster-wide
  version scalar.
- A daemon that receives an unknown message type from the scheduler drops the
  scheduler connection (`daemon/main.cpp:6112-6121`), and losing the scheduler
  clears all daemon-side client state (`daemon/main.cpp:6254-6256`,
  `clear_children()`).  Send-site version gating is therefore load-bearing,
  not cosmetic: one ungated send tears down a worker.
- The daemon already treats one scheduler session as one world: client ids
  reset and clients are cleared when the scheduler link is lost.  Assignment
  state on F may therefore be scoped to the scheduler session without
  changing observable semantics.
- `Msg` enum values are implicit and sequential from `UNKNOWN = 'A'` (65)
  through `JOB_TIMING` (97) (`services/comm.h:61-133`).  Values 98..101 are
  free.
- `MsgChannel` serializes `uint32_t` big-endian (`htonl`,
  `services/comm.cpp:580`) and strings as `uint32 len (incl. NUL) + bytes`.
  There is no `uint64_t` primitive; 64-bit quantities are specified below as
  two `uint32_t` words, high word first.
- The landed dispatch-credit work debits at `UseCS` send for remote decisions
  only (`scheduler/scheduler.cpp:1931-1933`), credits at `JobBegin`
  (`:2111`), teardown (`:2765`, `:2818`), the 107 bounce, and the batch
  cancellation sweep (`:2220`).  The clamp is
  `min(max_outstanding_dispatches, farm_slots - 1)` (`:577-585`).  Dispatch
  replies are `SendNonBlocking | SendDeferrable` with a 30 s application-level
  deadline (`ICECC_DEFERRED_SEND_TIMEOUT_MSEC`, `services/comm.h:243`;
  enforcement `scheduler/scheduler.cpp:1711-1730`).
- The landed retention work (`94dd89b`) already detaches and retains a
  COMPILING job whose submitter disconnects (`scheduler/scheduler.cpp:2794-2803`).
- Farm envelope (issue #2 inventory, mickg10/icecream): one scheduler, 13 real
  workers / 792 advertised remote slots, ~741 submitting hosts, cold build of
  ~2,200 TUs fanned in from hundreds of submitters.

---

## 2. Identifiers

| Name | Type | Allocated by | Semantics |
|---|---|---|---|
| `epoch` | u64 | scheduler, once per process start | Unique token naming one scheduler incarnation.  Compared for **equality only**; no ordering is promised or consumed (Q1). |
| `wire_id` | u32 | scheduler, per job (`new_job_id`) | The existing scheduler job id.  Unchanged meaning; allocation hardened (section 6.3). |
| `assignment_nonce` | u64 | scheduler, per assignment | Fresh CSPRNG value drawn when the assignment is created.  Never reused; zero is reserved for "no nonce". |
| assignment | — | — | The triple `(epoch, wire_id, nonce)` plus the chosen worker.  One job has at most one live assignment. |

Proposed `epoch` construction, requiring no stable storage:
`(uint64(start_time_seconds) << 32) | random32`.  The high word gives humans
a readable ordering hint in logs; correctness uses only equality.  Collision
across restarts requires a same-second restart AND a 1-in-2^32 random match.
Whether persistence is nevertheless required is Q1.

---

## 3. New wire vocabulary

### 3.1 `Msg` enum additions

```cpp
// services/comm.h, appended after JOB_TIMING (= 97)
// S --> CS (worker role): install authorization for one assignment
ASSIGN_PREPARE,        // = 98
// CS --> S: authorization installed and enforceable
ASSIGN_READY,          // = 99
// S --> CS: withdraw an assignment that must not start
REVOKE_BEFORE_START,   // = 100
// CS --> S: linearized outcome of the revocation
REVOKE_RESULT          // = 101
```

`Msg::to_string()` gains the four names.  **Decode gate:** `get_msg()`'s
dispatch switch (`services/comm.cpp:1291`) must construct these classes only
when `IS_PROTOCOL_VERSION(49, this)`; on an older negotiated link the values
are treated as unknown (channel error), so a future upstream reuse of the
same numeric values can never be misparsed on a <49 link.  Sequential
allocation vs a fork-private numeric base is Q12.

### 3.2 Message layouts

All four messages are exchanged **only** on the persistent S-CS channel of a
daemon acting in the worker role, and only when that link negotiated >= 49.
Following house style, each class serializes the type word via
`Msg::send_to_channel` first.

```cpp
class AssignPrepareMsg : public Msg
{
public:
    // wire order after the type word:
    //   epoch_hi, epoch_lo          (uint32 x2)  scheduler incarnation
    //   wire_id                     (uint32)     scheduler job id the client will present
    //   nonce_hi, nonce_lo          (uint32 x2)  assignment nonce (0,0 = none issued)
    //   submitter_hostid            (uint32)     scheduler host id of the submitter (diagnostics)
    //   flags                       (uint32)     reserved, 0
    uint32_t epoch_hi, epoch_lo;
    uint32_t wire_id;
    uint32_t nonce_hi, nonce_lo;
    uint32_t submitter_hostid;
    uint32_t flags;
};

class AssignReadyMsg : public Msg
{
public:
    // wire order: epoch_hi, epoch_lo, wire_id   -- echo of the prepare
    uint32_t epoch_hi, epoch_lo;
    uint32_t wire_id;
};

class RevokeBeforeStartMsg : public Msg
{
public:
    // wire order: epoch_hi, epoch_lo, wire_id
    uint32_t epoch_hi, epoch_lo;
    uint32_t wire_id;
};

class RevokeResultMsg : public Msg
{
public:
    enum Result : uint32_t {
        REVOKED = 0,   // tombstone installed before this ack; no compile started, none will
        STARTED = 1    // the claim/compile won the race (or had already completed); ownership retained
    };
    // wire order: epoch_hi, epoch_lo, wire_id, result (uint32)
    uint32_t epoch_hi, epoch_lo;
    uint32_t wire_id;
    uint32_t result;
};
```

The nonce travels in `ASSIGN_PREPARE` from protocol 49 onward (it is S-F
internal state); **protocol 50 gates only the client-visible propagation**.
The metadata block is deliberately minimal (two u32); anything richer arrives
with its own version gate later.  The result alphabet {REVOKED, STARTED} and
its treatment of claimed-but-unstarted compiles is Q8.

### 3.3 `ConfCSMsg` extension (protocol 49)

`CS_CONF` is already sent at login and relogin (`scheduler/scheduler.cpp:2031`,
`:2060`) and is the natural carrier for per-session assignment discipline:

```cpp
// appended to ConfCSMsg, both directions gated:
if (IS_PROTOCOL_VERSION(49, c)) {
    *c << epoch_hi;      // uint32
    *c << epoch_lo;      // uint32
    *c << fence_mode;    // uint32: 0 = LEGACY, 1 = ADVISORY, 2 = ENFORCING
}
```

- `epoch`: the scheduler announces its incarnation.  The daemon flushes its
  assignment table and tombstones whenever the announced epoch differs from
  the stored one.
- `fence_mode` (per scheduler session, constant within it):
  - `LEGACY (0)` — S will not send `ASSIGN_PREPARE`.  F admits claims exactly
    as today, **except** that tombstoned ids are rejected (revocation is
    active in every mode).
  - `ADVISORY (1)` — S sends `ASSIGN_PREPARE` for every remote assignment but
    does **not** wait for `ASSIGN_READY` before `UseCS` (pipelined; zero added
    dispatch latency).  F validates claims against the table when an entry
    exists (nonce mismatch or tombstone → reject) and still admits unknown
    ids.  This is the safe pipelined form: the fence is not load-bearing, so
    the claim-races-prepare window merely misses hardening for that instance.
  - `ENFORCING (2)` — S sends `UseCS` only after `ASSIGN_READY`
    (READY-gated dispatch).  F rejects claims for unknown, mismatched, or
    tombstoned ids (default-deny).  Because `UseCS` is causally after
    `READY`, a legitimate claim can never precede its prepare, so **no
    park-and-wait logic exists at F in any mode** (this deliberately deletes
    the AUDIT_DELTA "waits briefly" rule; see Q3).

Mode applicability: only ENFORCING delivers the AUDIT_DELTA S'F'C row's
prepared-admission guarantee; ADVISORY delivers revocation plus nonce
checking at zero latency cost; the activation choice is a scheduler policy
flip requiring no daemon change and no version bump.

### 3.4 `UseCSMsg` extension (protocol 50)

```cpp
// appended after matched_job_id, both directions gated:
if (IS_PROTOCOL_VERSION(50, c)) {
    *c << epoch_hi;      // uint32
    *c << epoch_lo;      // uint32
    *c << nonce_hi;      // uint32
    *c << nonce_lo;      // uint32
}
```

Hop-by-hop propagation (each hop is an independent negotiation):

1. S → submitter daemon: fields present iff that link >= 50.
2. submitter daemon → client: `scheduler_use_cs` forwards the original
   message object on the client channel (`daemon/main.cpp:4805`), so the
   fields re-serialize under the client link's version automatically.  The
   two sites that construct fresh `UseCSMsg` copies
   (`daemon/main.cpp:4798`, `:4802`) must copy the new fields.
3. Client → F: the client copies `(epoch, nonce)` from the received `UseCS`
   into the `CompileJob` (alongside the existing
   `job.setJobID(usecs->job_id)`, `client/remote.cpp:417-420`) so
   `CompileFileMsg` can echo them.

If any hop is < 50 the fields degrade to absent, and the claim is a legacy
claim (section 4).  `NoCSMsg` is unchanged: local decisions reserve no farm
slot and bypass the fence entirely (section 4.4).

### 3.5 `CompileFileMsg` extension (protocol 50)

```cpp
// appended after outputFile/dwarfFissionEnabled, both directions gated:
if (IS_PROTOCOL_VERSION(50, c)) {
    *c << epoch_hi;      // uint32   (0,0 = no assignment identity carried)
    *c << epoch_lo;      // uint32
    *c << nonce_hi;      // uint32
    *c << nonce_lo;      // uint32
}
```

`CompileJob` (services/job.h) gains carrier fields for the tuple; semantics
are defined entirely by F's admission rules below.

---

## 4. Worker-side (F) state machine

### 4.1 Assignment table

Keyed by `wire_id` (unique per epoch by construction):

```text
AssignmentRecord {
    epoch          : u64
    nonce          : u64          // 0 = none issued
    state          : RESERVED | CLAIMED | STARTED
    installed_msec : u64          // CLOCK_MONOTONIC, lease accounting
    claimant       : client_id    // once CLAIMED
}
Tombstone {
    epoch          : u64
    revoked_msec   : u64
}
```

Lifecycle scoping: table and tombstones live for one scheduler session and
one epoch.  Flush on scheduler-link loss (consistent with the existing
`clear_children()` behavior) and on an epoch change announced in `CS_CONF`.
Within a session: RESERVED entries expire after a lease `T_lease`
(default proposal 10 minutes — must exceed worst-case environment transfer,
which precedes the claim on the same connection and cannot be associated with
the assignment until `CompileFile` arrives; Q11).  Tombstones expire after
`T_tomb` (default proposal 15 minutes) and are capped in count with eviction
telemetry (Q4).

### 4.2 Message handling (all idempotent)

- `ASSIGN_PREPARE(e, w, n, ...)`:
  - no record: install RESERVED, reply `ASSIGN_READY(e, w)`.
  - identical record exists: re-reply `ASSIGN_READY` (duplicate delivery).
  - record with different `(e, n)` for the same `w`: replace + log an
    invariant warning (S must not reuse a live wire id on the same F;
    seeing this indicates an allocator fault).
  - tombstoned `w` (same epoch): reply `REVOKE_RESULT(e, w, REVOKED)`
    instead of READY — the revocation already won; do not resurrect.
- `REVOKE_BEFORE_START(e, w)`:
  - record RESERVED or absent: install tombstone, then reply
    `REVOKE_RESULT(e, w, REVOKED)`.  (Tombstone-before-ack is the ordering
    the scheduler's release depends on.)  Absent-record revocation is the
    LEGACY/ADVISORY bread-and-butter: F can tombstone an id it never saw
    prepared.
  - record CLAIMED or STARTED, or the id already completed this session:
    reply `REVOKE_RESULT(e, w, STARTED)` (conservative; Q8).
  - already tombstoned: re-reply `REVOKED`.
- Replies ride the same S-F channel; per-channel FIFO means `JobBegin(w)`
  always precedes a `REVOKE_RESULT(w, STARTED)` produced after the compile
  child spawned, so the scheduler never learns STARTED before it learns
  `JobBegin`.

### 4.3 Claim admission

A claim is a remote client's `CompileFileMsg` presenting `wire_id` (and, on a
>= 50 client link, an `(epoch, nonce)` echo).  Decision procedure, evaluated
when the message is received (before `TOCOMPILE` queueing):

```text
1. tombstone[wire_id] exists              -> REJECT (close connection)
2. record exists:
   a. claim carries (epoch, nonce):
        exact match of both              -> ADMIT (state := CLAIMED)
        any mismatch                     -> REJECT
   b. claim carries nothing (legacy):
        record.epoch == current epoch    -> ADMIT (bounded legacy claim)
        else                             -> REJECT
   c. record already CLAIMED/STARTED     -> REJECT (single-claimant rule)
3. no record:
        fence_mode == ENFORCING          -> REJECT
        else (LEGACY, ADVISORY)          -> ADMIT (legacy admission, as today)
```

REJECT is a connection close: the C-F alphabet has no negative acknowledgement,
and the client's existing failure path falls back to a local compile
(`client/remote.cpp` throws `client_error`, the caller builds locally).  The
failure mode of every fence decision is therefore *local compilation*, never
wrong execution.  Each rejection increments a per-reason counter
(tombstoned / nonce-mismatch / unprepared / stale-epoch / duplicate-claim)
exposed alongside the existing daemon state output.

State advance: CLAIMED → STARTED when the compile child is spawned — the
existing `JobBegin` site (`daemon/main.cpp:5489`).  A claim can wait in
`TOCOMPILE` behind the load gate (`daemon/main.cpp:5464`) for an unbounded
time; during that window revocation answers STARTED even though no process
runs (Q8 asks whether the alphabet should distinguish this).

### 4.4 Exemptions

Fence admission applies only to claims entering the remote-compile
(`TOCOMPILE`) path.  `CLIENTWORK` claims — the local-decision path where the
client compiles on its own daemon with environment `__client`
(`daemon/main.cpp:5556-5563`) — are exempt: local decisions never debit
dispatch credit (`scheduler/scheduler.cpp:1931`) and never held a farm
reservation.  This exemption is closed under versioning: a daemon whose S-link
negotiated >= 49 is necessarily >= 37, so its local decisions arrive as
`NoCS`/`CLIENTWORK`, never as remote-shaped self-claims.

---

## 5. Scheduler-side (S) state machine

### 5.1 Per-assignment phases

A new assignment phase attaches to the job at dispatch; the existing
`Job::State {PENDING, WAITINGFORCS, COMPILING}` is unchanged and the phase
refines WAITINGFORCS/COMPILING:

```text
PREPARED        ASSIGN_PREPARE sent, ASSIGN_READY not yet received
READY           ASSIGN_READY received; UseCS sent (ENFORCING: sent now;
                ADVISORY: was already sent at PREPARED)
REVOKE_PENDING  REVOKE_BEFORE_START sent, REVOKE_RESULT not yet received
STARTED         JobBegin received (Job::COMPILING), or REVOKE_RESULT=STARTED
CANCELLED       REVOKE_RESULT=REVOKED processed; terminal
```

Transitions and actions:

- **Dispatch decision** (`empty_queue`, `scheduler/scheduler.cpp:1780`):
  - worker link >= 49 and fence_mode != LEGACY: draw nonce, send
    `ASSIGN_PREPARE` (`SendNonBlocking | SendDeferrable`, same 30 s deferred
    bound as dispatch replies), phase := PREPARED.
    - ADVISORY: send `UseCS` immediately after (hot path unchanged).
    - ENFORCING: hold `UseCS`; job remains WAITINGFORCS with the worker
      reservation and dispatch debit already taken.
  - worker link < 49 or LEGACY: exactly today's behavior.
  - **Dispatch credit is debited at the decision** (PREPARE send, or UseCS
    send when no PREPARE is used) — one debit per decision, unchanged
    stall-accounting semantics (Q7).
- **ASSIGN_READY**: PREPARED → READY; ENFORCING sends the withheld `UseCS`.
  READY for an assignment in REVOKE_PENDING or unknown is ignored (stale).
- **JobBegin**: unchanged handler (`scheduler/scheduler.cpp:2086`); phase :=
  STARTED, credit released as today.
- **Revocation triggers** (phase PREPARED or READY only):
  1. submitter teardown, job dispatched-not-started
     (`handle_end`, `scheduler/scheduler.cpp:2777-2824`): today deleted with
     the late-claim hazard documented at `:2789-2793`; now REVOKE first, keep
     a detached shadow of the job until the result arrives.
  2. batch pre-reply cancellation (`unknown_job_client_id` sweep,
     `scheduler/scheduler.cpp:2138-2238`): dispatched members on >= 49
     workers are revoked; the submitter's own 107 bounce still settles the
     credit as today (both paths are idempotent against each other via
     `dispatchOutstanding()`).
  3. stalled-dispatch reclamation (`scheduler/scheduler.cpp:1646-1698`):
     policy upgrade from "retain + report" to "revoke + reclaim after the
     stall bound", flag-guarded (Q9); the report stays.
- **REVOKE_RESULT(REVOKED)**: release the worker reservation
  (`removeJob`), credit the dispatch credit, notify monitors
  (`MonJobDone 255`, as the cancellation paths do today), erase the job.
  Phase := CANCELLED.
- **REVOKE_RESULT(STARTED)**: the compile exists; convert to the landed
  retention semantics (`94dd89b`): if the submitter is gone, keep the job
  detached until F's real `JobDone`; never double-release (per-channel FIFO
  guarantees `JobBegin` arrived first, so the job is already COMPILING).
- **Worker link death**: unchanged — jobs assigned to the dead worker are
  torn down (`handle_end` DAEMON case); F flushes its table on its own
  S-link loss, so no state leaks on either side.
- **Late messages** (`JobBegin`/`JobDone`/`REVOKE_RESULT` for erased ids):
  tolerated and counted, never treated as connection errors.  This wants the
  handler-return-enum cleanup from AUDIT_DELTA finding 7, since today an
  unknown-id `JobBegin` returns `false` and the drain loop reads that as
  "connection deleted" (`scheduler/scheduler.cpp:550-560`).

### 5.2 Restart and epoch

- Epoch allocated at start (section 2), announced in every `CS_CONF`.
- No scheduler state is persisted.  After a restart every daemon relogs in,
  workers flush tables on the epoch change, and no pre-restart assignment
  survives anywhere — by construction rather than by recovery.
- REVOKE_PENDING across an F relogin within one epoch: on relogin (fresh
  `LoginMsg` handshake) the scheduler re-issues `REVOKE_BEFORE_START` for
  assignments still in REVOKE_PENDING on that worker and re-issues
  `ASSIGN_PREPARE` for PREPARED/READY ones; all handlers are idempotent.
  (Today relogin follows a full connection replacement, so this path is
  rare; it exists for completeness.)

### 5.3 Wire-id allocation hardening (old-C bounded mode)

For claims that carry no nonce, cross-restart exactness is impossible; the
bounded rule (Q5) is:

- `new_job_id` starts at a uniformly random 32-bit value per epoch (skipping
  0, which remains the "no job" sentinel) instead of 0, and wraps as today.
- F flushes its table per epoch, so a stale claim can only collide with an
  assignment the *new* scheduler prepared **on the same worker** with the
  **same id**; the probability per delayed claim is
  `ids_prepared_on_that_F_during_the_delay / 2^32`.
- No quarantine window is enforceable without persistence; the residual risk
  statement above *is* the freshness rule unless the remote oracle requires
  more (Q5).

---

## 6. Mixed-version matrix

Notation: `S'` = scheduler >= 49; `F'` = worker whose S-link negotiated
>= 49; `C'` = client chain >= 50 end-to-end (S-submitter link, submitter-client
link, client-F link); `F`, `C` = anything older on that axis.  Rows assume
`fence_mode = ENFORCING` where prepared-admission is claimed; the ADVISORY
deltas are stated inline.  In every row, teardown of a STARTED job follows the
landed retention semantics (scheduler-local, version-independent).

### S'FC  (old worker, old client)

S-F negotiates <= 48: none of the new vocabulary exists on this link, and the
scheduler must not change behavior for jobs assigned to F.
**Guarantee:** exactly the p48 fork baseline — batch cancellation cuts staged
and queued members; dispatched-not-started members settle only via the
submitter's 107 bounce or teardown; a delayed claim can still be compiled by
F against an erased job (wasted work, tolerated unknown-id completion);
stalled dispatches are retained and reported, never reclaimed.
**Best-effort:** nothing new.  This row is the projection target: the new
scheduler's traces on this link must refine p48 (formal/README.md section 5).

### S'F'C  (new worker, old client)

**Guarantee (exact, S-F axis, current epoch):** F starts a remote compile
only for a wire id with a live, unrevoked authorization installed by this
scheduler incarnation.  Consequences: (a) submitter teardown of a
dispatched-not-started job reclaims the slot and credit in bounded time
(REVOKE → REVOKED), and a later claim for that id is rejected — the case the
scheduler-local fix deliberately left open is closed; (b) pre-reply batch
cancellation extends to dispatched members; (c) stall retention may become
bounded reclamation (policy, Q9); (d) never both "scheduler released the
slot" and "F started the compile" — the FARM Phase 4 acceptance invariant.
**Bounded (C axis, across restart):** a delayed pre-restart claim is excluded
only by the section 5.3 freshness rule (per-epoch flush + randomized
allocator + same-worker coincidence), not by proof.
**ADVISORY delta:** (a)-(d) hold whenever REVOKED/tombstone precedes the
claim; a claim that races ahead of its own PREPARE is admitted unvalidated
(legacy), so the exactness statement weakens to "for every id F has seen
prepared or revoked".

### S'F'C'  (new worker, new client chain)

**Guarantee (exact, both axes):** F admits only the registered
`(epoch, wire_id, nonce)` tuple; a delayed pre-restart claim carries the old
epoch/nonce and is rejected, closing the restart ABA.  The
`scheduler-restart-wire-id-aba` model counterexample becomes unreachable;
`scheduler-restart-fenced-client` is the positive check.
**Caveat:** this row's *assignment-side* exactness assumes no legacy claims
exist in the fleet — see the next row and Q6.

### S'F[CC']  (old worker, mixed clients)

S-F <= 48 dominates: identical to S'FC for both client generations.  A C'
may receive `(epoch, nonce)` in `UseCS` (its submitter chain is >= 50) but
the C-F link negotiates < 50, so the echo is never serialized; the fields are
inert.  **Guarantee:** p48 baseline.

### S'F'[CC']  (new worker, mixed clients)

Per-claim guarantees: a C' claim gets exact tuple admission; a C claim gets
the bounded legacy admission.  **Per-assignment caveat (important):** while
nonce-less claims remain admissible — and they must, because F cannot know a
given assignment's client generation in advance — an assignment whose real
client is a C' is protected *exactly* against misdirected C' claims but only
at the *bounded* level against a delayed old-C claim presenting a colliding
wire id.  The reviewer's matrix row "old C bounded-id mode; C' exact nonce
mode" is therefore a statement about claims, not assignments;
`FencedAssignment.tla`'s `NonceSpec` omits `LegacyClaim` from its `Next` and
so does not model this fleet (Q6).  Restoring assignment-side exactness
requires a strict mode (reject nonce-less claims) that is deployable only
once no old C remains — proposed as a third `fence_mode` value or a
`CS_CONF` flag, pending Q6.

---

## 7. Interaction with landed and in-flight work

- **Dispatch credit:** the debit moves to the dispatch *decision* (PREPARE
  send when the fence is in use), keeping "one debit per decision" and the
  oldest-unconfirmed stall bound meaningful across the new PREPARED window.
  New credit-release path: REVOKE_RESULT(REVOKED).  All release paths remain
  idempotent through `Job::dispatchOutstanding()`
  (`scheduler/scheduler.cpp:781-806`).
- **Deferred send / backpressure:** PREPARE and REVOKE use
  `SendNonBlocking | SendDeferrable` and inherit the 30 s deadline and
  removal semantics.  Open question Q7: whether a worker with an armed
  deferred-output backlog should be ineligible for new PREPAREs, mirroring
  `submitter_accepts_dispatch()` (`scheduler/scheduler.cpp:813-818`).
  ENFORCING mode converts worker ingest latency into dispatch latency
  (section 8's measurement gate and Q2).
- **Retention (`94dd89b`):** REVOKE_RESULT(STARTED) lands in the same
  retention semantics; the comment at `scheduler/scheduler.cpp:2789-2793`
  ("a future worker-side cancel exchange") is exactly the PR-1 hook.
- **Issue-2 cancellation sweep:** unchanged for staged/queued members;
  dispatched members gain the revocation leg on >= 49 workers.  The sweep
  remains a single unified cut; revocation replies arrive asynchronously and
  release per-member state idempotently.
- **Stall observability:** the `stall_report_after` warning and counters stay;
  reclamation is the added, flag-guarded reaction (Q9).
- **FARM Phase 1 witnesses:** every new transition (PREPARED, READY,
  REVOKE_PENDING, REVOKED-processed, late-claim-rejected) emits a durable
  transition record, satisfying the Phase 4 requirement to record request,
  response, and late-arrival rejection.

---

## 8. Staged rollout

Deployment reality: daemon upgrades (13 workers + 741 submitters) are the
expensive, slow axis; scheduler restarts are cheap.  The staging therefore
front-loads the complete daemon vocabulary once and stages all activation on
the scheduler side.

- **PR-1 — protocol 49 vocabulary + revocation in use (smallest valuable PR).**
  - Wire: four messages, `CS_CONF` epoch/fence_mode fields, decode gates,
    `PROTOCOL_VERSION` 49.
  - F side: complete — assignment table, tombstones, all four handlers,
    claim-admission procedure with all three modes, counters.  (F must be
    complete in PR-1 so later stages are scheduler-only.)
  - S side: epoch allocation + announcement (fence_mode=LEGACY); randomized
    `new_job_id` start; revocation wired into (1) submitter teardown of
    dispatched-not-started jobs and (2) the batch-cancel sweep's dispatched
    members; stall-reclaim behind a default-off flag.
  - Hot dispatch path: byte-identical to p48.  Old peers: byte-identical
    (all sends gated).
  - Negative control: reverting PR-1 must turn the new red/green trace
    (freeze C after `UseCS`, tear down the submitter, assert the worker slot
    is reclaimed and a late claim is rejected) red again.
- **PR-2 — advisory fence.**  S sends PREPARE pipelined with UseCS
  (fence_mode=ADVISORY).  No latency change; measures PREPARE traffic and
  table behavior at farm scale; nonce registration becomes live end-to-end
  on the S-F axis.
- **PR-3 — protocol 50 client echo.**  UseCS/CompileFile fields, client and
  submitter-daemon plumbing, F nonce verification (already implemented in
  PR-1).  Ships the S'F'C' row.
- **PR-4 — enforcing flip.**  fence_mode=ENFORCING (READY-gated dispatch).
  No wire change; guarded by the Tier B/C measurement gate: dispatch p95/p99
  latency and jobs/sec at 1k/5k/20k/40k with 13 workers must stay within the
  budget the remote oracle sets in Q2.  Rollback is a scheduler flag flip.
- **(Optional PR-5)** strict nonce mode for fully upgraded fleets (Q6).

Release-vehicle note: PR-1..3 may ship in a single release (jumping 48 → 50)
or two (49, then 50); the per-feature `IS_PROTOCOL_VERSION(49, ...)` /
`(50, ...)` gates keep both safe, and the matrix above depends only on the
negotiated minima, not on release packaging.

---

## 9. What this design does NOT require

- **No behavior change for old peers.**  Every new message and field is
  send-gated on the negotiated link version; a p43/p44/p48 daemon or client
  sees a byte-identical protocol.  (Load-bearing: an ungated scheduler send
  makes an old daemon drop its scheduler connection,
  `daemon/main.cpp:6112-6121`.)
- **No client change for p49.**  The fenced-assignment protocol is entirely
  S-F; old clients gain the S'F'C improvements without touching the 741
  submit hosts' compilers.
- **No stable storage on the scheduler.**  Epoch is random-unique; allocator
  hardening is randomization; nothing is recovered after restart — state is
  invalidated by construction (subject to Q1/Q5 answers).
- **No new connections, ports, or discovery changes.**  p49 rides the
  persistent S-F channel; p50 rides existing message paths.  Broadcast,
  LOGIN shape, environment transfer, and the monitor protocol (MON_*) are
  untouched.
- **No hot-path round trip until explicitly enabled.**  LEGACY and ADVISORY
  modes keep dispatch latency identical to p48; only the ENFORCING flip
  (PR-4, measured) introduces the READY wait.
- **No change to local-decision jobs.**  NoCS/CLIENTWORK paths are exempt
  from the fence and carry no new fields.
- **No first-class-batch dependency.**  This design composes with, but does
  not require, the Phase 2 batch/index refactor; revocation volume is bounded
  by outstanding dispatches (<= credit x submitters), not batch size.

---

## 10. Smallest first PR, restated

PR-1 above: **protocol 49 vocabulary + tombstoning revocation, fence dormant.**
It closes the two motivating gaps on S'F'C — submitter teardown of a
dispatched-but-not-started job, and cancellation of dispatched batch
members — with zero dispatch-latency risk, no client involvement, one daemon
fleet upgrade, and a clean negative control.  It also carries the epoch
announcement that every later stage depends on.  The reviewer's own
observation ("p49 materially improves S'F'C even without p50") holds for
this subset; the fence and the nonce then activate without any further
daemon-side change.

---

## 11. Deviations from the reviewer's sketch (deliberate)

1. **No park-and-wait at F.**  AUDIT_DELTA has claims for unknown ids "wait
   briefly for a causally earlier prepare".  In ENFORCING mode the causal
   order makes the case impossible; in ADVISORY mode unknown ids are admitted.
   The wait rule is dropped in both (Q3).
2. **`fence_mode` staging.**  The sketch implies prepared admission is a
   property of version 49 itself; this design makes 49 carry the vocabulary
   and `CS_CONF` carry the discipline, because a version number cannot be
   rolled back at 2 a.m. but a scheduler flag can.
3. **Claims carry `epoch` explicitly alongside the nonce** (p50).  The sketch
   says F admits "the registered (epoch, wire_id, nonce)" but has the client
   echo only the nonce; carrying both costs 8 bytes and buys precise
   rejection telemetry (stale-epoch vs nonce-mismatch) and a fast-path reject
   that needs no table lookup.
4. **Revocation of absent records is defined** (tombstone + REVOKED), which
   is what makes the revoke-only PR-1 subset coherent — the original
   `CANCEL_BEFORE_START` semantics from formal/README.md section 4 are
   preserved as the LEGACY/ADVISORY behavior.
5. **The claimed-but-unstarted phase is acknowledged** (TOCOMPILE queue
   behind the load gate) even though the result alphabet cannot express it
   yet (Q8).
