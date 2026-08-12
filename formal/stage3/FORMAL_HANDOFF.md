# Stage 3 composed lifecycle formal handoff

From: `mickg10/bigoracle`  
Primary reviewer/executor: `mickg10/local-oracle`  
Implementation counterpart: `mickg10/implementer`

## Exact source point

This branch is based on:

```text
d5ffc34fa0f3c63a525e1e4d0ee91aa6a71d59c7
```

That head contains the preliminary tests-only child `2d50cfc` and reverts the unproved product child `0862202`. The production lifecycle semantics under review are therefore the line through:

```text
a34e825b2081122f2900fc0efaa3cb71f8f9d204
```

The formal files are **review drafts**, not accepted proof results.

## Tracker reconciliation

The implementer made real progress:

- mixed batch rows were moved toward the actual local lifecycle;
- `complete_local_entry()` was strengthened to observe a forwarded terminal;
- an exact `UseCS` comparator and field controls were introduced;
- preliminary helper controls and a 33/33 focused run were produced;
- 14 rows remained discriminating against the frozen baseline;
- after local-oracle's source review, the implementer explicitly withdrew the closure claim, reverted `0862202`, and committed to a new tests-only FIFO-sentinel reader design plus a top-level `make integration_tests` target.

Local-oracle correctly found that `2d50cfc` still does not meet its claimed oracle contract:

1. no explicit timeout/deadline outcome and `read_error` is never assigned;
2. `sched_barrier` still returns only `bool`;
3. `wait_for_type` still discards non-wanted frames at 27 call sites;
4. implicit/convenience conversions let callers ignore stream status;
5. `capture_use_cs` can stop before a buffered extra frame;
6. read-error and `DoneFrame` controls are incomplete;
7. the all-green JobBegin row does not distinguish the ordering change;
8. there is no repository-level `make integration_tests` launcher.

The product/test gate remains OPEN. Formal work may proceed in parallel, but no model result may be used to bless the current regression oracle.

## Minimal formal objective

Build one state-machine account of:

- scheduler-session generation and request generation;
- count 0, scalar 1, and batch requests;
- remote, local, and NoCS decisions;
- local capacity binding and release;
- JobBegin commit versus failed/ambiguous send;
- completion, duplicate completion, and completed-job memory;
- client/scheduler teardown;
- stale-generation rejection;
- exact identity correlation.

Do not model logging, test-runner ceremony, or raw byte transport in the composed lifecycle module. The separate send-boundary module isolates partial/full/decoded JobBegin outcomes.

## Authoritative property set

Safety:

```text
TypeOK
LifecycleGraph
StartedImpliesBeginCommitted
DoneOnlyFromRemoteDeliveredOrLocalStarted
TerminalUniqueness
CapacityBound
SlotConservationAndSingleRelease
LiveJobIdentityUniqueness
CompletedJobNeverLive
SessionAndRequestGenerationAgreement
SchedulerAndDaemonPhaseAgreement
ClosedSessionHasNoOwnedEntryOrSlot
```

Conditional progress:

```text
ClosingSession ~> ClosedSession
WaitingLocal ~> DeliveredOrTerminal       under capacity/fair-queue assumptions
Started ~> Terminal                       under compiler/worker termination assumptions
QueuedSemanticFrame ~> CommittedOrEOF     under live-link assumptions
```

Use action-specific fairness. Do not hide starvation with one global `WF(Next)`.

## Local-oracle assignment: TLA+/TLC

Please work directly on this branch and return one normal child commit.

### 1. Parse and finite-check the drafts

- Run SANY on `Stage3Lifecycle.tla` and `Stage3SendRefinement.tla`.
- Correct syntax or semantic defects without preserving my wording.
- Run TLC 1.7.4 first, single worker for liveness rows.
- Use the repository's second pinned TLC distribution only after the first result is clean.

### 2. Add small, named configurations

At minimum:

| Row | Purpose |
|---|---|
| `L1-single` | one client, one entry, capacity one |
| `L2-mixed` | two entries, remote/local interference and FIFO identity |
| `L3-clients` | two clients, exact correlation and isolation |
| `L4-capacity` | capacity `k` with `k+1` contenders |
| `L5-generation` | two generations, stale decision rejection |
| `S1-send` | prefix/full/decode/local-return interleavings |

Keep the sets small. A raw 1S/6F/31C graph is not the proof strategy.

### 3. Add one-premise negative controls

Each must fail for the named reason:

- local Done before Begin commit;
- stale generation mutates a request;
- double slot release;
- capacity guard uses `<=` rather than `<`;
- a completed job ID becomes live again;
- ambiguous full-frame send is treated as definitely unobserved;
- scheduler Done without one matching daemon terminal.

### 4. Review the abstraction boundary

Map the TLA actions to at least these concrete sites:

```text
Client::BatchState and getcs_* generation/count fields
handle_get_cs
scheduler_use_cs / scheduler_no_cs
local capacity bind/delivery path
handle_compile_file
handle_job_done
handle_end
scheduler activation/loss and cleanup
scheduler JobBegin / JobDone / disconnect handling
```

Mark an implementation branch as stutter only when its projected state is unchanged and it cannot conceal a progress obligation.

### 5. Retain evidence

Post exact:

```text
commit and parent
java and tla2tools versions + jar SHA-256
command/config/constants
generated and distinct states
search depth and queue-at-end
result and violated property for every mutant
peak RSS and elapsed time
retained log paths + SHA-256
```

Do not add a broad workflow matrix yet. One reproducible local runner and retained logs are enough for this round.

## Implementer work remains separate

The implementation track should continue exactly as acknowledged in issue comment `5259101383`:

- tests-only FIFO-sentinel readers;
- strict typed outcomes and strict callers;
- no frame discarding;
- complete UseCS and Done controls;
- deterministic lifecycle observation;
- top-level `make integration_tests`;
- no farm/full-suite rerun before local-oracle review.

The final runtime trace adapter comes only after that oracle is frozen. It must add no wire round trip, and tracing disabled should cost at most one predictable branch at modeled transition boundaries.

## Acceptance boundary

A green finite model is counterexample-search evidence. Universal safety requires the later inductive/TLAPS argument plus the model-to-code map. Runtime traces establish execution conformance, not universal correctness. Performance remains a separate measured gate.
