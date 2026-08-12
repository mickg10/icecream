# G4 lifecycle transition trace schema

Schema id: `icecream.g4.lifecycle.v1`

This is a tests/debug trace. It changes no wire message and adds no scheduler
round trip. With tracing disabled, the intended production cost is one
predictable branch at each modeled transition boundary.

The checker is `formal/check_g4_trace.py`. It validates the complete projected
state chain and replays every transition through one named action. It rejects
unknown actions/states, impossible edges, generation regressions, accepted-
ledger regressions, slot/accounting mismatches, more than one bound local
entry, duplicate terminals, gaps, dropped records, and a bad final digest.

## Header

One JSON object on the first line:

```json
{
  "type": "header",
  "schema": "icecream.g4.lifecycle.v1",
  "commit": "<40-hex product commit>",
  "run": "<unique run id>",
  "role": "daemon",
  "instance": "<stable process instance>",
  "initial_state": { "...": "projected state" }
}
```

## Projected state

```json
{
  "protocol": 48,
  "max_batch": 3,
  "client_priority": {
    "client-17": {"nice": 0, "client_id": 17}
  },
  "session": "Disconnected | LoginAttempt | Active",
  "generation": 0,
  "conf_arrived": false,
  "activated_by": "None | Legacy | Conf",
  "loss_pending": false,
  "capacity": 2,
  "requests": {
    "client-17": {
      "state": "Idle | Waiting | Closed",
      "generation": 0,
      "expected": 0,
      "accepted": 0,
      "decisions": {
        "entry-1": {
          "kind": "None | Remote | Local | NoCS",
          "phase": "Absent | RemoteDelivered | LocalWaiting | LocalBound | LocalDelivered | LocalStarted | Terminal",
          "owner_generation": 0,
          "owner_request_generation": 0,
          "slot_charged": false,
          "begin_committed": false,
          "terminal_count": 0
        }
      }
    }
  }
}
```

The product adapter may include additional diagnostic fields in a separate
`detail` object. It must not omit or reinterpret projected fields.

## Transition

```json
{
  "type": "transition",
  "seq": 1,
  "action": "Connect",
  "client": null,
  "decision": null,
  "cause": "scheduler socket connected",
  "pre": {"...": "complete projected state"},
  "pre_digest": "sha256(canonical pre)",
  "post": {"...": "complete projected state"},
  "post_digest": "sha256(canonical post)"
}
```

Sequences are gap-free and process-local. The next record's `pre` must equal
the prior record's `post` byte-for-byte after canonical JSON encoding.

Accepted action names:

```text
Connect
ConfArrived
LegacyActivated
ConfActivated
ZeroNoop
BatchAccepted
BatchOverflowRejected
LocalAccepted
RemoteAccepted
NoCSAccepted
StaleDecisionRejected
LocalBound
LocalDelivered
BeginCommitted
BeginNoCommitFailure
BeginAmbiguousFailure
NonOwnedObserved
NonOwnedRejected
WrongDoneRejected
Completed
DuplicateDoneRejected
SessionLost
SessionCleaned
RequestClosed
ClientReset
```

Identity-bearing actions carry `client` and, where applicable, `decision`.
`BatchAccepted` carries `count`. `StaleDecisionRejected` carries
`incoming_session_generation` and `incoming_request_generation`.
`BeginNoCommitFailure` carries `commit_class: "none"`.
`BeginAmbiguousFailure` carries `commit_class: "ambiguous"`; it must never be
silently rewritten to no-commit.

## Footer

```json
{
  "type": "footer",
  "schema": "icecream.g4.lifecycle.v1",
  "records": 123,
  "dropped_records": 0,
  "final_digest": "sha256(canonical final state)",
  "final_state": {"...": "optional complete final state"}
}
```

A missing footer, nonzero dropped count, sequence gap, or digest mismatch makes
the run incomplete.

## Required negative controls

The checker test suite must reject at least:

- unknown action;
- sequence gap;
- `LocalStarted` without `BeginCommitted`;
- stale session/request generation;
- a second bound-local entry;
- capacity overflow or double release;
- a second terminal;
- accepted-counter/ledger regression;
- partial session cleanup;
- wrong priority selection;
- incorrect begin failure classification;
- bad footer digest.
