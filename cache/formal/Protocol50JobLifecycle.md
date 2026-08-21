# Protocol-50 job-lifecycle model

`Protocol50.tla` models the cache transaction, immutable-object publication, session fencing, replay, and the durable-F/lost-C-acknowledgement window. `Protocol50JobLifecycle.tla` is deliberately separate: it starts at `INPUT_COMMITTED` and models the compiler-attempt layer that consumes an exact retained input.

This split is intentional, not a second protocol architecture. The cache transaction has no `ATTEMPT_ID`; a compiler restart creates a new attempt for the same committed `(C_STORE_GUID, TU_SEQ)` input.

## What it models

- two F cache incarnations and two compile attempts for one logical job;
- P50 and established/legacy input as mutually exclusive attempt modes;
- attachment before or after exact input commit;
- compiler-environment readiness as an independent start condition;
- compiler/clone cancellation and replacement;
- a restart lease that keeps exact P50 input attachable between attempts;
- a late result from a cancelled attempt;
- exactly one accepted result;
- committed-input eviction only after the logical job releases its restart lease;
- an **F cache-sidecar/store restart**, distinct from a full worker restart.

## Cache restart rule

A cache-sidecar restart destroys the retained P50 input on that F and changes its cache epoch.

```text
waiting P50 attempt:
    cancelled; replacement may be created after input is re-committed

already-started P50 compiler:
    remains authorized and may finish because it already owns exact input

legacy attempt:
    unaffected by a cache-only restart
```

The model records authorization monotonically in `attemptAuthorized` when `StartCompiler` succeeds. Later cache loss does not revoke that historical fact. Cancelling the logical job or the attempt still makes its result ineligible.

A full `iceccd`/host restart that kills compiler children is a different product event and is not represented by `RestartFCache`.

## Restart-safe retained input

Starting a P50 attempt acquires an input restart lease on its F. Cancelling one attempt does not release that lease: a replacement attempt may attach to the same `INPUT_COMMITTED` record without retransmission or another route-history commit.

The lease is released when:

```text
the logical job accepts one result
or
the logical job is definitively cancelled
```

This is the simplest safe V1 lifetime. The implementation may back the record with immutable memory, a sealed file, or another re-readable exact source, but each attempt needs an independent read cursor.

## Result arbitration

```text
cache attempt and legacy/replacement attempt may overlap
only one result is accepted for the logical job
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

The cache model retains its own DICT/Need/Fill and commit actions. A trace checker joins the models at `INPUT_COMMITTED`/`COMMITTED_INPUT_RETAINED`; neither model duplicates the other one's state.

## Run

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC \
  -metadir /tmp/icecream-p50-job-tlc-state \
  -config cache/formal/Protocol50JobLifecycle.cfg \
  cache/formal/Protocol50JobLifecycle.tla
```

The model and configuration on this review branch require a retained TLC run before merge. No model-check result is claimed merely from publishing the files.
