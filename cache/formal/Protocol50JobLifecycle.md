# Protocol-50 job-lifecycle model

`Protocol50.tla` models the cache transaction through `INPUT_COMMITTED`. `Protocol50JobLifecycle.tla` starts at that exact boundary and models compiler attempts consuming a retained input. `Protocol50IncarnationBridge.tla` handles verified F-store replacement while a cache commit is durable but unaccepted.

The split is intentional, not a second architecture. A cache transaction has no `ATTEMPT_ID`; compiler restart creates a new attempt for the same committed `(C_STORE_GUID, TU_SEQ)` input.

## What it models

- two F cache incarnations and two compile attempts for one logical job;
- P50 and established/legacy input as mutually exclusive attempt modes;
- attachment before or after exact input commit;
- compiler-environment readiness as an independent start condition;
- compiler/clone cancellation and replacement;
- a logical-job lease that keeps exact P50 input attachable before and between attempts;
- a late result from a cancelled attempt;
- exactly one accepted result;
- committed-input eviction only after the logical job releases its restart lease;
- an F cache-sidecar/store restart, distinct from a full worker restart.

## Cache restart rule

A cache-sidecar restart destroys retained P50 input on that F and changes the cache epoch.

```text
waiting P50 attempt:
    cancelled; replacement may be created after input is re-committed

already-authorized P50 compiler:
    may finish because it independently owns exact input

legacy attempt:
    unaffected by a cache-only restart
```

Authorization is monotonic after `StartCompiler`. Later cache loss does not revoke that historical fact. Cancelling the logical job or attempt still makes its result ineligible.

A full host/daemon restart that kills compiler children is a different event.

## Restart-safe retained input

`INPUT_COMMITTED` itself acquires the logical-job input lease. This is load-bearing: attachment may occur after commit, so the exact input cannot be evicted merely because no compiler attempt exists yet.

Starting a P50 attempt confirms/retains the same lease. Cancelling one attempt does not release it because a replacement may attach without retransmission or another route-history commit.

The lease is released only when:

```text
one result is accepted for the logical job
or
the logical job is definitively cancelled
```

The implementation may back the record with immutable memory, a sealed file, or another re-readable exact source. Each attempt needs an independent read cursor.

## Result arbitration

```text
cache attempt and legacy/replacement attempt may overlap
only one result is accepted
cancelled or losing attempts may report late results
late results remain observable but cannot win
```

## Trace/action vocabulary

The later product trace should expose:

```text
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

The cache model retains DICT/Need/Fill and commit actions. A trace checker joins at `INPUT_COMMITTED`/`COMMITTED_INPUT_RETAINED`; neither model duplicates the other’s state.

## Run

```sh
TLA2TOOLS_JAR=/path/to/tla2tools.jar TLC_WORKERS=1 make protocol50-formal
```

The exact branch head requires a retained TLC run and independent reproduction before merge. Publishing the model is not a pass result.
