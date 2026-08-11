# G4 minimal formal audit

Base product revision: `a34e825b2081122f2900fc0efaa3cb71f8f9d204`  
Formal-only branch: `bigoracle/g4-minimal-formal`

This branch adds one composed TLA+ model and one TLC workflow. It does not add
protocol 49/50, a new wire token, a generator framework, or product code.

## Current audit verdict

The focused gate is open. The broad suite and farm replay are useful regression
evidence, but the batch fixture has demonstrated paths that report PASS after
the daemon rejected the modeled lifecycle transition. The repaired lossless
fixture must become the concrete trace oracle before model traces can certify
product behavior.

Concrete findings at the base revision:

1. `a34e825` correctly sends `JobBegin` before committing
   `LOCAL_COMPILE_STARTED`; the deterministic failed-send witness is still
   required.
2. The non-owned batch-local branch deletes the incoming `CompileJob` and
   returns success while retaining `LOCAL_DELIVERED_SLOT_CHARGED`, its slot, and
   the live client. It needs a named rejection followed by bounded teardown.
3. Protocol 21--23 can remain in `LOGIN_ATTEMPT`: the daemon activates only in
   `handle_cs_conf()`, while the scheduler emits `ConfCSMsg` only for protocol
   24 and newer and the minimum supported protocol remains 21.
4. `Clients::get_earliest_client()` uses “lower id AND lower niceness,” not the
   intended lexicographic `(niceness, client_id)` order.
5. Batch expansion has no explicit product `MaxBatch`; `GetCSMsg::count` is
   copied to `getcs_expected`, and the ledger grows until that count settles.
6. `find_by_client_id()` scans every client for each UseCS/NoCS reply. Normal
   reply dispatch therefore scales linearly with connected clients.
7. Local admission repeatedly scans clients while filling capacity, giving an
   `O(capacity * clients)` turn in the worst case.
8. Client UseCS delivery uses a blocking send in the daemon’s event loop.
   Preserve exact lifecycle accounting, but measure and bound management and
   scheduler latency under a slow reader.
9. The older assignment-fence branch is design material, not an accepted
   result: its final exact head has no successful complete same-SHA formal run
   and accumulated overlapping models and workflows.

## Model-to-code map

| Model action/state | Product seam | Required deterministic evidence |
|---|---|---|
| `Connect` / `LoginAttempt` | `Daemon::reconnect`, `scheduler_login_pending` | no application traffic before activation |
| `ActivateLegacy` | missing protocol-21--23 transition | protocol 23 activates without ConfCS; mutant waits forever |
| `ReceiveConf` | `Daemon::handle_cs_conf` | first pending ConfCS commits one generation |
| `SubmitZero` | `handle_get_cs(count == 0)` | no request state or scheduler frame; later count 1 succeeds |
| `SubmitValid` / `SubmitOverflow` | `getcs_expected`, scheduler publication | exact `MaxBatch` boundary; overflow rejects before growth |
| `AcceptLocal` / `AcceptRemote` | `scheduler_use_cs`, `scheduler_no_cs`, `getcs_batch` | exact full UseCS tuple and one entry per accepted id |
| `BindLocal` | `advance_batch_local` | at most one bound local entry per client |
| `DeliverLocal` | `handle_old_request` PENDING_USE_CS lane | delivery observed and exactly one slot charged |
| `CommitBegin` | batch branch of `handle_compile_file` | exact JobBegin observed before STARTED |
| `FailBegin` | failed `send_scheduler(JobBeginMsg)` | STARTED absent; client/session cleanup; capacity restored |
| `RejectNonOwned` | non-owned batch branch in `handle_compile_file` | named rejection and bounded teardown, no retained slot |
| `WrongDone` | rejected phases in `handle_job_done` | named rejection/no-model-change; fixture cannot discard it |
| `CompleteDecision` | accepted remote/local JobDone | exact forwarded JobDone; one terminal result |
| `DuplicateDone` | COMPLETED dedup path | named duplicate rejection; no second terminal |
| `LoseSession` / `CleanupLoss` | `close_scheduler`, `finish_scheduler_loss_if_needed`, `clear_children` | exactly-once cleanup before reconnect/capacity |
| `SelectClient` | `Clients::get_earliest_client` | lexicographic winner and conjunction mutation control |

## Checked properties

`CoreSafety` checks:

```text
TypeOK
ProtocolActivation
expected <= MaxBatch
accepted <= expected
accepted = Cardinality(non-ABSENT decisions)
slot occupancy <= Capacity
slotCharged(d) <=> phase(d) in {LOCAL_DELIVERED, LOCAL_STARTED}
LOCAL_STARTED(d) => JobBeginCommitted(d)
TERMINAL(d) <=> terminalCount(d) = 1
TERMINAL(d) => not slotCharged(d)
non-ABSENT(d) => ownerGeneration(d) > 0
live LOCAL_STARTED(d) => active matching generation
RejectNonOwned => terminal and uncharged
count 0 => no request state
selected = min_(niceness, client_id)(clients)
```

The LTL rows use weak fairness only for actions the daemon owns:

```text
[] (LOGIN_ATTEMPT => <> ACTIVE)
[] (lossPending => <> (~lossPending /\ slotOccupancy = 0))
```

They do not assume compiler completion, network repair, or an immortal client.

## TLC matrix

Every row runs with one worker on pinned TLC 1.7.4 and the exact v1.8.0
pre-release asset published on 2026-08-11; both downloads are digest-checked.

| Row | Required result |
|---|---|
| fixed protocol 23 | exhaustive safety, activation, and cleanup green |
| fixed protocol 48 | exhaustive safety, activation, and cleanup green |
| legacy-needs-Conf mutant | `ActivationProgress` violation |
| STARTED-before-JobBegin mutant | `StartedAfterBegin` violation |
| rejected-slot-retained mutant | `RejectedCleanup` violation |
| unbounded-batch mutant | `BatchBound` violation |
| conjunction-priority mutant | `PriorityMinimal` violation |

The workflow retains exact configs, tool digests, complete TLC output, state
counts, and revision. A green model run is not a product close verdict. Product
closure additionally requires the repaired focused fixture, broad suite, farm,
and measured performance/management-latency gates on one exact final head.

## Landing order

1. Finish the tests-only lossless reader/comparator/lifecycle fixture.
2. Land the smallest product-only fail-closed non-owned cleanup correction.
3. Land protocol-21--23 activation and the lexicographic selector as separate
   small corrections with deterministic witnesses.
4. Add a bounded `MaxBatch` and O(1) client-id lookup before calling the batch
   design scalable.
5. Re-run this model on the final product head, then the focused fixture, broad
   suite, farm, and performance gates.
