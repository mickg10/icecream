# Audit delta: started-job ownership and restart fencing

This note supersedes the first-pass assumption that a bare
`CANCEL_BEFORE_START(job_id)` is sufficient for all new-S/new-F topologies.
It records the highest-priority findings from the subsequent code-level pass.

## P0: do not delete STARTED jobs when the submitter disconnects

After `JobBegin`, the fulfillment daemon owns the physical execution and the
scheduler has already returned the submitter's dispatch credit. The current
`handle_end(submitter)` still deletes every job whose `submitter()` matches the
dead daemon, removes it from a different live worker's job list, and frees the
scheduler reservation. That does not stop F's compiler process.

Required partition:

```text
STAGED / QUEUED       -> cancel immediately
DISPATCHED, no begin  -> revoke through new F, or retain on legacy F
STARTED               -> detach submitter; retain through F Done/F failure
```

A STARTED job must not retain a raw lifetime dependency on the dead
`CompileServer*`. Snapshot the submitter metadata needed for statistics and
monitoring. The first new red/green harness trace should freeze F after
`JobBegin`, disconnect D, assert the job and F reservation remain, then deliver
F's real `JobDone` exactly once.

Executable model traces:

- `submitter-teardown-delete-started` — counterexample;
- `submitter-teardown-detach-started` — bounded positive check.

## P0: scheduler restart creates immediate wire-id ABA

Old C presents only the scheduler's 32-bit job id to F. A delayed C that
received `UseCS` from scheduler epoch A can arrive after restart, when epoch B
has reused the same id for the same F. Neither old C nor the legacy compile
request identifies the epoch. F can report `JobBegin(id)` to epoch B for the
wrong physical request.

This means S-F revocation is necessary but not sufficient for exact arbitrary-
delay safety across restart/wrap.

Executable model traces:

- `scheduler-restart-wire-id-aba` — counterexample;
- `scheduler-restart-fenced-client` — nonce-fenced positive check.

## Revised staged protocol

### Protocol 49: Fenced Assignment Protocol on S-F

```text
S -> F  ASSIGN_PREPARE(epoch, wire_id, metadata)
F -> S  ASSIGN_READY(epoch, wire_id)
S -> F  REVOKE_BEFORE_START(epoch, wire_id)
F -> S  REVOKE_RESULT(epoch, wire_id, REVOKED | STARTED)
```

F installs authorization before `READY`. An incoming C for an unknown id waits
briefly for a causally earlier prepare or is rejected; it must not start merely
because the integer id is well formed. `REVOKED` installs a tombstone before
acknowledgement. Operations and responses are idempotent. S exposes `UseCS`
only after `READY`, unless a pipelined form is separately proved safe.

For old C, p49 provides bounded fencing only under a stated wire-id freshness /
quarantine rule. Persisting or randomizing the next allocator value makes
restart collision unlikely but is not a proof against arbitrary delay.

### Protocol 50: exact C-F assignment capability

For new C, `UseCS` carries an epoch-scoped random `assignment_nonce`, and
`CompileFile` echoes it. F admits only the registered
`(epoch, wire_id, nonce)` tuple. This is the exact generation fence.

| Topology | Guarantee |
|---|---|
| `S'FC` | p43 legacy behavior |
| `S'F'C` | p49 prepare/revoke; exactness bounded by old-C id freshness |
| `S'F'C'` | p49+p50 exact fencing |
| `S'F[CC']` | old-F projected behavior |
| `S'F'[CC']` | old C bounded-id mode; C' exact nonce mode |

## Additional concrete code findings

1. `GetCS.count` is a full `uint32_t`; the implementation eventually retains
   and atomically activates all siblings. Admission slicing does not bound
   memory or the activation/cancel/teardown turn.
2. The batch environment selected with the computed matching platform is later
   pinned by searching the worker's host platform. Compatible non-identical
   platform pairs can lose or change the pin.
3. Inbound-connectivity backoff uses `sizeof(array)` as an element count and
   can index beyond its 12 entries; it also uses wall-clock deadlines.
4. The legacy scheduler broadcast stores `strlen(netname)` in signed `char` and
   uses inconsistent length/copy semantics.
5. Decoded `minimal_host_version` and `niceness` cross unsigned-to-signed
   conversions before validation/clamping.
6. `StatsMsg.client_count` exists and is consumed by scheduler policy, but is
   not serialized by `StatsMsg`.
7. `handle_relogin()` leaves the connection alive but returns the boolean that
   the drain loop documents as “connection deleted.” Replace handler booleans
   with an explicit result enum.
8. Queue, cancellation, and teardown work remains superlinear/unbounded at the
   741-submitter/40k–48k scale. First-class batches and direct indexes are
   required, not merely higher constants.

## Fairness witness

The independent checker observed big reply streams progressing while the two
big submitters were absent from every `listcs` sample. A capacity inequality is
useful only when those ephemeral rows are visible. The primary gate should
construct, not sample, the interval:

```text
BACKLOG_OPEN(big1), BACKLOG_OPEN(big2)
    < SMALL_DISPATCH
    < BACKLOG_CLOSED(big1), BACKLOG_CLOSED(big2)
```

Use durable transition sequence numbers and test-side barriers. A fast host
must exercise the property rather than skip it.

## Checker commands

```sh
git switch audit/temporal-ownership-model
python3 formal/icecc_temporal_model.py all --json-dir /tmp/icecc-traces
python3 formal/icecc_temporal_delta.py
```

Run TLC/Apalache on both `TemporalOwnership.tla` and `FencedAssignment.tla`.
The Python models were executed; the TLA+ models have not yet been checked by
TLC/Apalache in the authoring environment.
