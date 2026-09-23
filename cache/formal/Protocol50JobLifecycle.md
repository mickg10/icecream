# Protocol-50 job-lifecycle model

`Protocol50.tla` models the cache transaction through `INPUT_COMMITTED`. `Protocol50JobLifecycle.tla` starts at that exact boundary and models compiler attempts consuming retained exact input. `Protocol50IncarnationBridge.tla` handles verified F-store replacement while an old transaction is in flight or a cache commit is durable but unaccepted.

The split is intentional, not a second architecture. A cache transaction has no `ATTEMPT_ID`; compiler restart creates a new attempt for the same committed `(C_STORE_GUID, TU_SEQ)` input.

## What it models

- two F cache incarnations and two compile attempts for one logical job;
- P50 and established/legacy input as mutually exclusive whole-attempt modes;
- attachment before or after exact input commit;
- compiler-environment readiness as an independent start condition;
- compiler/clone cancellation and replacement;
- a logical-job lease that keeps exact P50 input attachable before and between attempts;
- explicit independent input ownership at compiler authorization;
- a late result from a cancelled attempt;
- exactly one accepted result;
- committed-input eviction only after the logical job releases its restart lease;
- an F cache-sidecar/store restart, distinct from a full worker restart;
- fair progress of a replacement attempt without a second cache commit.

## Authorization is an ownership handoff

`COMPILER_AUTHORIZED` is not merely permission to start. It is the atomic boundary at which the compiler owns a complete immutable input source independently of the cache sidecar.

Acceptable implementations include:

```text
sealed immutable file with an independently held descriptor
immutable memory object whose lifetime is owned by the compiler attempt
fully transferred compiler input buffer
```

A compiler still draining a sidecar-owned pipe is **not** authorized for restart purposes.

The model therefore checks:

```text
attemptAuthorized = attemptOwnsInput
Running or Finished => authorized and owns input
accepted result => Finished, eligible, authorized, owns input
```

A direct mutant authorizes without transferring ownership and must violate `AuthorizedAttemptOwnsIndependentInput`.

## Cache restart rule

A cache-sidecar restart destroys retained P50 input on that F and changes the cache epoch.

```text
waiting P50 attempt:
    cancelled; replacement may be created after input is re-committed

already-authorized P50 compiler:
    may finish because it owns exact input independently

legacy attempt:
    unaffected by a cache-only restart
```

Authorization and input ownership are monotonic historical facts. Cancelling the logical job or attempt still makes its result ineligible. A full host/daemon restart that kills compiler children is a different event and is represented by attempt cancellation followed by ordinary replacement scheduling.

## Restart-safe retained input

`INPUT_COMMITTED` itself acquires the logical-job input lease. This is load-bearing: attachment may occur after commit, so exact input cannot be evicted merely because no compiler attempt exists yet.

Starting a P50 attempt confirms the same lease. Cancelling one attempt does not release it because a replacement may attach without retransmission or another route-history commit. The lease is released only when:

```text
one result is accepted for the logical job
or
the logical job is definitively cancelled
```

A direct mutant releases the lease on attempt cancellation and must violate `CommittedInputForOpenJobKeepsLease`.

Each attempt uses an independent read cursor or equivalent ownership reference. Once authorization transfers independent ownership, later release of the logical InputRecord lease does not invalidate that compiler's already-owned source.

## Executable restart progress

`RestartSpec` starts with:

```text
A0 cancelled before authorization
exact input already committed at F0
logical-job restart lease still held
environment ready
A1 not yet started
```

Only these transitions are available:

```text
StartAttempt(A1, F0, P50)
StartCompiler(A1)
FinishCompiler(A1)
AcceptResult(A1)
```

There is deliberately no `CommitExactInput` transition. Under weak fairness of those four product-controlled steps, `ReplacementAttemptEventuallyAccepted` proves that the replacement attaches to the same retained input and wins.

This is a scoped progress theorem, not a claim of liveness under infinitely recurring worker or network failures.

## Result arbitration

```text
cache attempt and legacy/replacement attempt may overlap
only one result is accepted
cancelled or losing attempts may report late results
late results remain observable but cannot win
```

The winner closes the logical job and releases all logical InputRecord leases. Losing already-authorized compilers may still hold independent input until their process/descriptor cleanup completes; that physical cleanup does not reopen result eligibility.

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

The cache model retains BODY/Need/Fill and commit actions. A trace checker joins
at `INPUT_COMMITTED`/`COMMITTED_INPUT_RETAINED`; neither model duplicates the
other's state.

## Run

```sh
ICEFARM_TMPDIR=/absolute/path/to/scratch \
TLA2TOOLS_JAR=/path/to/tla2tools.jar make protocol50-formal
```

The exact branch head requires a retained TLC run and independent reproduction before merge. Publishing the model is not a pass result.
