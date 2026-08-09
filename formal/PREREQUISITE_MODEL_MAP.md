# Protocol-neutral prerequisite model-to-product map

This map is intentionally narrower than the later fenced-assignment protocol.
It identifies the existing product seams that must realize or replay each
checked transition. It does not authorize PREPARE/READY/REVOKE behavior.

## Shared scheduler-visible job IDs

| Model transition/property | Product seam | Existing or required executable gate |
|---|---|---|
| Allocate a remote or local-monitor ID from one namespace | Scheduler job-ID allocator and every remote/local allocation wrapper in `scheduler/` | `jobidalloctest`; tiny domain through `ICECC_TEST_JOB_ID_DOMAIN` |
| `GlobalLiveUniqueness` | Live scheduler Jobs, local monitor Jobs, and allocator registry | Cross-namespace one-ID mutation must become red |
| Duplicate local Begin is idempotent | Local-job Begin handler and local-client-to-global-ID map | `schedbp duplocal`: same global ID, exactly one issue, no orphan/release violation |
| Disconnect releases the exact owner once | Central job removal/release helper and daemon-generation teardown | Unknown/double release mutation increments exactly one violation and never restores false conservation |
| Batch exhaustion publishes nothing | `PendingExpansion`, staged Jobs, allocator reservation, and fallback transition | `schedbp exhaust`: full rollback, one explicit fallback, allocator live count returns to baseline |

The formal manifests are the source of truth for deterministic barrier names.
No test may infer success from a missing management row or an incomplete
`200 done` response.

## STARTED lifecycle terminal authority

| Model transition/property | Product seam | Executable gate |
|---|---|---|
| `Waiting -> Started` linearizes once | Scheduler `JobBegin` handler and monitor Begin emission | Fixed Started reachability witness; duplicate Begin mutation |
| Detach submitter generation | `Job::detachSubmitter`, stable identity snapshot, submitted-job accounting | Disconnect after Begin, complete management snapshots, scheduler remains live |
| `DetachedTerminalAuthority` | Scheduler `JobDone` origin/generation/assigned-worker checks | Replacement submitter Done and wrong-worker Done leave the real worker reservation live |
| Worker Done / worker-session loss | Assigned worker terminal handler and worker teardown | Exactly one terminal monitor event and reservation release |
| `StableDetachedIdentity` | `dump_job` / management observability | Detached identity remains visible without dereferencing a dead connection object |

## Exact UseCS handoff

| Model transition/property | Product seam | Executable gate |
|---|---|---|
| Persist exact assignment before client output | Daemon scheduler-UseCS handler and client handoff state | `cut_usecs_before_first_byte` must still select exact settlement |
| Partial frame is not committed | Message-channel framing and deferred output | Header, body, and final-byte-short cuts never become delivery uncertainty |
| `ExactAbortPending` | Daemon client record and scheduler-facing settlement state | Exact scheduler ID remains retained until terminal frame acceptance or scheduler-session loss |
| Abort frame accepted/consumed | Ordered daemon-to-scheduler output and POLLOUT drain | Complete exact JobDone/abort frame; no later message bypass |
| Abort transport failure | Scheduler-channel terminal failure/session-loss path | Injected send failure either preserves pending exact settlement or closes the scheduler session |
| `NoLocalAliasSettlement` | Cancellation/terminal message identity selection | Local client ID is never substituted for an already-known scheduler assignment ID |

The four cut witnesses abstract a frame to four bytes only to distinguish the
four product injection positions. They do not assert that the wire frame is
four bytes long.

## Old fulfillment-session child quiescence

| Model transition/property | Product seam | Executable gate |
|---|---|---|
| Register exact compiler child ownership | Daemon child registry: pid, pgid, kind, scheduler-session generation, assignment | Registry observation hook identifies compiler separately from writer/environment children |
| W-first waitability | State-writer child plus one blocked compiler | `make_state_writer_waitable_before_session_loss`; old wait-any mutation reaps W and must fail |
| Reap exact compiler PID | Exact-pid wait and compiler occupancy record | No `waitpid(-1)` as the session quiescence proof |
| Prove process-group absence | `killpg`/group probe after direct child reap or ECHILD | Parent-reaped/descendant-survives mutation remains red |
| One absolute deadline | Whole old-session quiescence loop | Reaping one child or entering group verification does not restart the deadline |
| Fail closed on residue | Reconnect/login/capacity advertisement gate | Unknown occupancy is never reset to zero; daemon withholds capacity or terminates explicitly |
| Advertise replacement session | Scheduler reconnect/login path | No new slot is accepted or advertised before every prior-session compiler group is absent |

## Acceptance linkage

Each expected-counterexample manifest must produce the named essential event
subsequence and emitted barrier steps on both pinned TLC toolchains. The C++
fixture must use the same barrier names or a checked one-to-one translation.
A sleep-only reproduction is supplementary and cannot satisfy this map.
