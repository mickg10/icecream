# Protocol-50 TLA+ safety model

This directory contains the deliberately small formal core of Protocol 50. It is not a network simulator and it does not encode P29 or GRZ byte streams. It models the state transitions whose composition can otherwise produce silent cache or retry bugs.

## Scope

The model includes:

- one C-store namespace and one or two independent F arenas;
- one active uncommitted cache transaction per C/F relationship;
- route-local `REL_SEQ`, independent of global `TU_SEQ` order;
- complete immutable object publication, separate from input/history commit;
- F-authoritative object presence, pinning, and eviction;
- whole-current-TU replay after disconnect;
- lost-final-`TX_COMMIT` reconciliation;
- destructive F-store restart and history-only reset;
- cache and legacy compiler attempts;
- job/clone restart by creating a replacement attempt for the same prepared TU;
- concurrent losing and replacement attempts, with at most one accepted result;
- environment readiness as one external compiler-start condition.

The model intentionally excludes:

- frame parsing and partial-object assembly;
- compression, Key64 bit packing, and byte accounting;
- preprocessing, scheduler policy, timing, and queue service rates;
- compiler-environment transfer internals;
- active C-store restart while old-namespace jobs are still consuming input.

Those are covered by parser tests, integration/farm scenarios, and the simulator. `RestartCQuiescent` models the safe namespace-flip boundary after active attempts and transactions drain.

## Why job restart is separate from cache restart

A cache transaction is identified by the C/F relationship and prepared TU. A compile attempt is a consumer of that exact input. The model therefore permits:

1. a cache input to commit;
2. an attached compiler attempt to die;
3. a replacement cache attempt to attach to the same committed input without retransferring it;
4. a legacy retry to run while an older cache attempt may still have a late result;
5. exactly one attempt result to be accepted for the logical job.

`ATTEMPT_ID` is intentionally absent from the cache transaction identity and digest.

## Commit domains

`ApplyObject` is the abstract `OBJECT_APPLIED` transition. It makes one completely validated immutable object visible. The object survives transaction abort and reconnect. A conflicting value closes the session without changing the installed value.

`CommitInput` is the abstract `INPUT_COMMITTED` transition. It requires:

- complete DICT and BODY;
- the exact required closure present and pinned;
- matching route pre-state and `REL_SEQ`;
- exact raw-input materialization/digest.

Only `CommitInput` advances route history. `AckCommit` or `ReconcileLostAck` advances C's acknowledged cursor.

## Restart model

The model implements the four product cases:

1. **Exact state match:** reconnect and replay the retained current TU.
2. **Lost final acknowledgement:** F is exactly one matching commit ahead; C adopts that commit.
3. **F-store replacement:** new F identity means a cold object arena and a new route history.
4. **History mismatch with objects intact:** reset only history and retain immutable objects.

There is no frame-level resume, acknowledgement window, or log repair.

## Running TLC

Using a local `tla2tools.jar`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC \
  -config formal/protocol50/Protocol50.cfg \
  formal/protocol50/Protocol50.tla

java -cp /path/to/tla2tools.jar tlc2.TLC \
  -config formal/protocol50/Protocol50_2F.cfg \
  formal/protocol50/Protocol50.tla
```

The one-F configuration explores two route commits and two compiler attempts. The two-F configuration explores independent F stores and rerouting with one commit per relationship to keep the state space bounded.

## Implementation trace correspondence

The in-memory M1 implementation should emit these action names at the mutating boundary:

```text
Prepare
OpenSession
Route
ReceiveDict
ComputeNeed
ReceiveBody
ApplyObject
RejectConflictingObject
PinClosure
CommitInput
AckCommit
Disconnect
ReplayActive
ReconcileLostAck
AbandonUncommitted
RestartF
AcceptColdArena
ResetHistory
EvictObject
EnvironmentReady
StartAttempt
DetachAttempt
StartCompiler
AcceptResult
RestartCQuiescent
```

A trace checker may collapse frame-level events into these actions, but it may not invent a state mutation with no corresponding model action.

## Required implementation tests beyond this model

The following are deliberately tested in code rather than represented as extra TLA state:

- every split of the 4-byte frame header and representative bodies;
- short/excess DICT, BODY, NEED, and FILL streams;
- incomplete object records never becoming visible;
- same Key64 with different content being session-fatal;
- stale old-session frames after fencing;
- disconnect after each frame byte and protocol boundary;
- exact byte-ledger closure across retries;
- environment install/verify overlap and failure;
- process death and descriptor cleanup;
- bounded queues, memory credits, and backpressure;
- real P29 cross-route COPY rejection;
- GRZ retained-window and overlapping-copy correctness;
- old/new protocol fallback and late losing results.

The model should remain small. Add state only when an implementation bug or unresolved semantic choice cannot be expressed by the existing actions and invariants.
