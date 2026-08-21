# Protocol-50 formal lane

This directory is the canonical formal home for Protocol 50 because it lives beside the product identities, executable M1 state machine, action trace, and trace checker.

There are two deliberately non-overlapping models:

```text
Protocol50.tla
    cache transaction, immutable objects, Need/Fill, pinning,
    session fencing, replay, and lost-final-commit reconciliation

Protocol50JobLifecycle.tla
    retained exact input, compiler/job restart, cache-versus-legacy attempts,
    F-cache restart, and one-result arbitration
```

They compose at `INPUT_COMMITTED`: the cache model proves that exact input was committed; the job model governs attempts that consume the retained input. This is one protocol design, not two competing architectures.

The older experimental superset under `formal/protocol50/` on the capability branch is not canonical. It was found to retain the separate `ComputeNeed`/`PinClosure` race and to lack the claimed callback-token and running-attempt restart semantics. Do not use it as a merge or trace-refinement target.

## Cache-model rules

The cache model includes:

- one C-side active transaction and one F-side pending overlay;
- two possible F destinations, exercised one relationship at a time;
- immutable `Key64 -> content` binding;
- atomic Need computation that pins every already-present required object;
- immediate pinning of each requested object when it is completely installed;
- eviction only when an object is not transaction-pinned;
- complete overlay/pin release on disconnect or session replacement;
- history reset only when C and F have no active transaction and no unacknowledged durable commit;
- whole-current-TU replay;
- an F-durable/C-unacknowledged commit window;
- session-token fencing of close, control, object-worker, and commit callbacks.

The atomic Need/pin rule prevents this race:

```text
Need observes object X present
X is omitted from Need
X is evicted before materialization
no Fill for X exists
```

## Job-model rules

The job model includes:

- a restart lease that retains exact P50 input between compiler attempts;
- `ATTEMPT_ID` outside cache transaction identity;
- replacement attachment without retransmission or a second history commit;
- P50 and legacy as whole-attempt modes, never spliced;
- monotonic compiler authorization after exact input ownership is handed off;
- an F cache-sidecar restart cancelling waiting P50 attempts but not an already-authorized compiler;
- late losing results that remain observable but cannot win;
- exactly one accepted result for the logical job.

A full worker/host restart is distinct from the modeled cache-sidecar restart.

## Implementation correspondence

Current M1 traces cover the implemented subset. Future mutating boundaries should use these names or an explicitly documented one-to-one mapping:

```text
SESSION_OPENED
SESSION_REPLACED
SESSION_DISCONNECTED
HISTORY_RESET
TX_BEGIN
TX_ABORTED
DICT_COMPLETE
NEED_RECORDED
BODY_COMPLETE
OBJECT_APPLIED
INPUT_MATERIALIZED
INPUT_COMMITTED
COMMIT_ACCEPTED
ACTIVE_REPLAYED
LOST_COMMIT_ACCEPTED
EVICT_OBJECT
STALE_CLOSE_CALLBACK
STALE_CONTROL_CALLBACK
STALE_OBJECT_CALLBACK
STALE_ACK_CALLBACK

COMMITTED_INPUT_RETAINED
ATTEMPT_STARTED
ENVIRONMENT_READY
LEGACY_INPUT_READY
COMPILER_AUTHORIZED
COMPILER_FINISHED
ATTEMPT_CANCELLED
LATE_RESULT
RESULT_ACCEPTED
JOB_CANCELLED
COMMITTED_INPUT_EVICTED
F_CACHE_RESTARTED
```

A large implementation trace need not contain every modeled action, but every mutating implementation event must refine one modeled transition.

## Running TLC

Use TLA+ tools v1.7.4 or another explicitly recorded version:

```sh
java -jar /path/to/tla2tools.jar \
  -config cache/formal/Protocol50.cfg \
  cache/formal/Protocol50.tla

java -jar /path/to/tla2tools.jar \
  -config cache/formal/Protocol50JobLifecycle.cfg \
  cache/formal/Protocol50JobLifecycle.tla
```

The files on this review branch require retained TLC output before merge. Publishing or reviewing a model is not itself a successful model-check run.

## Deliberate exclusions

The formal lane does not model compression bytes, frame parsing, preprocessing time, scheduling policy, bandwidth, compiler-environment transfer internals, P29 grammar construction, or GRZ parsing. Those remain parser, codec, simulator, and farm-test responsibilities.
