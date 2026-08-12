# G4 projected transition trace schema

Schema name:

```text
icecream.g4.lifecycle.v2
```

Model boundary:

```text
scheduler-session/batch-ledger
```

This is a tests/debug projection. It changes no wire message and adds no round trip.

## Envelope

A trace is JSON Lines with one header, one or more transitions, and one footer.

```json
{"type":"header","schema":"icecream.g4.lifecycle.v2","commit":"<exact-sha>","model_boundary":"scheduler-session/batch-ledger"}
{"type":"transition","schema":"icecream.g4.lifecycle.v2","seq":1,"pre":{},"event":{},"post":{},"pre_digest":"...","post_digest":"..."}
{"type":"footer","schema":"icecream.g4.lifecycle.v2","records":1,"dropped_records":0,"final_digest":"..."}
```

The checker rejects sequence gaps, dropped records, state discontinuity, wrong source commit, and digest mismatch before accepting the lifecycle.

## Projected state

Global fields:

```text
protocol, max_batch, capacity, max_generation, max_request_generation
session, generation, conf_arrived, activated_by, loss_pending
priority[client] = {nice, client_id}
completed_tokens[] = [session_generation, client, request_generation, decision]
cancelled_requests[] = [session_generation, client, request_generation]
outputs[]
dropped_records
```

Per request:

```text
mode: None | Scalar1 | BatchN
phase: Idle | Waiting | Settling | Closed
settlement_cause: None | NormalEnd | AbnormalEnd | SessionLoss
generation
expected, accepted, delivered
tail_cancel_count
entries[]
```

Per entry:

```text
kind: None | Remote | Local | NoCS
phase: Absent | RemoteDelivered | LocalWaiting | LocalBound |
       LocalDelivered | LocalStarted | Terminal
owner_session_generation
owner_request_generation
delivered: boolean
charge_count, release_count
begin_status: None | Committed | NoCommit | Ambiguous
terminal_cause: None | Done | Cleanup
terminal_attempts, terminal_emits
```

Each semantic output records:

```text
kind: UseCS | Begin | Done | CleanupJob | CancelTail
client, decision
session_generation, request_generation
identity = [session_generation, client, request_generation, decision]
cause = None | Done | NormalEnd | AbnormalEnd | SessionLoss
```

Every delivered entry has exactly one matching `UseCS` output, including a remote decision delivered at acceptance. Terminal emission adds the exact entry identity to `completed_tokens`; a tail cancellation adds the exact request identity to `cancelled_requests`. A later admission using a completed identity is rejected. Output metadata must equal its full identity tuple.

`BeginAmbiguousFailure` is conservative: it records semantic Begin commit, emits one Begin, enters `LocalStarted`, and then triggers daemon-global scheduler-loss cleanup. `BeginNoCommitFailure` emits no Begin and cannot enter `LocalStarted`.

Historic outputs remain in the trace after a client slot is reset. The checker therefore correlates output counts by request generation; it must not attribute an old request's terminal or tail cancellation to a new request using the same client slot.

## Action registry

```text
Connect
ConfArrived
ActivateLegacy
ActivateConf
AcceptCount0
AcceptScalar1
AcceptBatchN
RejectBatchOverflow
AcceptRemote
AcceptLocal
AcceptNoCS
RejectStaleDecision
BindLocal
DeliverLocal
BeginCommit
BeginNoCommitFailure
BeginAmbiguousFailure
AcceptDone
RejectWrongDone
RejectDuplicateDone
NormalEnd
AbnormalEnd
SettleJob
CancelTail
CloseRequest
LoseSession
FinishLossCleanup
ResetClient
```

For every transition, `check_g4_trace.py`:

1. validates the pre-state;
2. runs action-specific candidate-post checks;
3. validates the candidate post-state;
4. calculates the unique projected post-state for that action;
5. requires exact equality.

Action-specific checks deliberately precede generic equality. Thus a candidate that tags `LocalStarted` before semantic Begin is rejected for that named reason, and an immediate-terminal NoCS candidate is rejected for the NoCS-lane reason.

## Product emission obligation

A future product adapter must emit one complete transition record per named lifecycle action. Unknown actions or states, impossible edges, generation regression, accepted-ledger regression, capacity mismatch, duplicate terminal, double release, output reordering, dropped records, or a mismatched final digest reject the entire trace.

A transition log is conformance evidence only after every deterministic integration scenario has been replayed and the deliberately corrupted controls have failed for their intended reasons.
