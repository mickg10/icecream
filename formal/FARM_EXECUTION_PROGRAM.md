# Implementation and farm execution program

This program converts the temporal-ownership model into implementation work for
`mickg10` and independent acceptance work for `mickgvirtu`.

## Available envelope

The reported environment is unusually strong for this exercise:

- one production scheduler, with candidate schedulers runnable under isolated
  per-run netnames;
- 13 real fulfillment daemons and 792 remotely usable slots;
- roughly 741 submitting hosts, with a maximum observed host count around 754;
- containerized and clustered role matrices for 1.4.0/protocol 43,
  upstream 1.4.90/protocol 44, and the stage3 fork/protocol 48;
- deterministic socket backpressure, SIGSTOP/SIGCONT freezes, kill/restart,
  strace/perf/eBPF, ASan/UBSan, core dumps, and production control counters;
- demonstrated 40k single-group, 48k fairness, 30k sibling, and 21-group
  pending-output workloads;
- a real cold build of about 2,200 translation units fanned in from hundreds of
  submitters.

Production daemons on shared hosts must never be disturbed. Every experimental
scheduler and test population therefore uses a unique netname, and must remain
correct in the presence of unrelated scheduler broadcasts on the same segment.

## Roles and branch discipline

- `mickg10` pair: implement, review each other, and run red/green plus farm dry
  runs. Keep each semantic change in a separately revertible commit.
- `mickgvirtu`: do not inherit implementation assumptions. Compile the model's
  JSON traces into adversarial harness cases, mutate them, and own the final
  acceptance report.
- `audit/temporal-ownership-model`: specification and trace source. Do not mix
  production implementation commits into this branch.

Every product change must have a **negative control**: reverting only the
product commit must make its new gate fail for the intended reason.

## Phase 0 — freeze the baseline

Before refactoring, record these artifacts for current head and the pre-fix
commit:

1. `python3 formal/icecc_temporal_model.py all --json-dir traces` output;
2. `make check` and every existing scheduler stress mode;
3. current mixed-version 24-case matrix, including forced old-client/new-daemon
   and new-client/old-daemon edges;
4. scheduler control snapshots (`listcs`, `listjobs`, `listrequests`,
   `estimates`) before/during/after each trace;
5. CPU, maximum control-port latency, queue depth, jobs/sec, cancellation
   latency, and RSS at 1k/5k/20k/40k jobs.

The cancellation fix in issue #2 is the baseline, not the end of the audit.

## Phase 1 — durable transition witnesses and online invariants

Add a compact monotonic transition sequence in the scheduler. Suggested event
record:

```text
seq, monotonic_ns, scheduler_epoch, connection_generation,
wire_client_id, request_generation, member_index, job_id,
old_state, event, new_state, submitter_id, worker_id
```

Expose a bounded ring through a control command such as `transitions <seq>`.
This is diagnostic/control-plane state and does not change protocol 43–48.

Add assertions/counters for:

- one primary place per live member;
- queued iff exactly one request-group membership exists;
- assigned iff exactly one worker-job-list membership exists;
- dispatch debt iff `DISPATCHED_UNCONFIRMED`;
- reservation iff assigned and not terminal;
- no staged/queued member after its cancellation cut;
- no reference to a dead connection generation;
- requested count = unmaterialized + all materialized/terminal outcomes.

### Required test repair

Do **not** treat `fairness window unobservable on this host` as a passing proof.
Build a deterministic rendezvous:

1. create two long requests;
2. observe durable `BATCH_OPEN`/`BATCH_BACKLOG` events for both;
3. hold the interval open with explicit test-side gates rather than sleeps;
4. issue small requests;
5. release the gates;
6. assert the event ordering
   `backlog-open < small-dispatch < backlog-closed`.

A fast machine must exercise the same property, not skip it.

Acceptance: the observer-gap JSON trace fails the old sampling test and passes
only after the durable/barrier witness exists.

## Phase 2 — first-class batches and bounded cancellation

Introduce a `Batch`/request object with an internal generation independent of
the wire client id:

```text
Batch {
    submitter_generation;
    request_generation;
    wire_client_id;
    requested_count;
    next_member;
    state: OPEN | PINNED | CANCELLED | COMPLETE;
    pinned_environment;
    member/index ownership;
}
```

Implementation requirements:

- jobs point to the batch; remove the raw master/sibling star as the source of
  shared request state;
- materialize only a bounded window while preserving the batch-level pinned
  environment;
- cancellation linearizes in O(1) by marking the batch/fibre cancelled;
- selectors reject cancelled batches immediately;
- physical removal and monitor notifications run in bounded cleanup quanta;
- connection teardown uses the same cancellation/cleanup machinery;
- add direct indexes for cancellation key, `(submitter,niceness)` group,
  job-to-group, and daemon client-id lookup.

Acceptance:

- replay `cancel-legacy.json` against the old commit and show the queued ghost;
- replay it against the candidate and show the logical cut before any cleanup
  quantum;
- cancel 2k, 24k, 40k, and a test-only much larger count while proving bounded
  control latency and RSS proportional to the materialization window;
- kill the submitter during every cleanup quantum and reuse its fd immediately;
- show no post-cut dispatch and no reference to the old generation.

## Phase 3 — eligibility-graph admission oracle

Create eligibility signatures from platform, environment set, required
features, remote/local policy, and any other hard scheduling predicate. Build a
bipartite capacity graph from active signatures to fulfillment daemons.

First ship an **observe-only** Hall/min-cut oracle:

```text
for every active class set X:
    reservations(X) <= capacity(N(X)) - protected_slack(X)
```

The oracle records when aggregate credit admits a reservation that violates a
Hall cut. It does not initially change scheduling.

Farm scenarios must include heterogeneous neighborhoods, for example:

- two A-only workers plus one B-only worker;
- overlapping A/B and B/C workers;
- rare architecture/environment workers;
- one very slow or wedged eligible worker;
- submitters whose local slots are not remotely usable.

Acceptance: the `eligibility-blind-credit.json` counterexample must be detected
by the oracle while the existing global `slots-1` rule reports success. After
policy activation, the constrained request must retain a feasible path without
starving flexible work.

## Phase 4 — protocol-49 pre-start revocation

Specify a new capability negotiated only on the persistent `S' <-> F'` link:

```text
S' -> F' : CANCEL_BEFORE_START(epoch, job_id)
F' -> S' : CANCEL_BEFORE_START_RESULT(epoch, job_id, CANCELLED | STARTED)
```

Worker behavior:

- `CANCELLED`: install a tombstone before acknowledging; reject any later
  client presenting that `(epoch,job_id)` before compiler start;
- `STARTED`: compilation/JobBegin won the race; scheduler retains normal
  ownership until completion;
- duplicate requests and duplicate responses are idempotent;
- reconnect/restart behavior and tombstone retention are explicit.

Scheduler behavior:

- release dispatch debt/reservation only after `CANCELLED`;
- never pretend an old F can revoke;
- record a durable transition for request, response, and late-arrival reject.

Compatibility gate:

| Topology | Expected behavior |
|---|---|
| `S'FC` | protocol-old behavior; no bounded pre-start reclaim promise |
| `S'F'C` | revocation works even though the compiler client is old |
| `S'F'C'` | revocation works |
| `S'F[CC']` | old-F behavior for both client generations |
| `S'F'[CC']` | revocation works for jobs assigned to F' |

The client need not understand the new message because the existing compile
request already carries the scheduler job id to F.

Acceptance: freeze at each edge—before UseCS delivery, after delivery, before
F accept, after accept/before compiler start, after start, and before each
ack. The invariant is never both “scheduler released the slot” and “F started
the compile.”

## Phase 5 — epoch and reuse discipline

Add a scheduler epoch for new peers and make the internal identity at least:

```text
(epoch, submitter_generation, request_generation, member_index, job_id)
```

For old peers, document and test the no-reuse/quarantine assumption. Add a
test allocator with a 2–3 value job-id space to force wrap immediately.

Acceptance: `bounded-id-aba.json` must kill the old implementation and become
impossible under the new identity/tombstone checks. Include stale JobBegin,
JobDone, revocation response, and monitor messages across scheduler/F restart.

## Phase 6 — generated trace corpus

Write a small compiler from the model JSON schema to the existing harness. A
trace action maps to concrete primitives such as:

- connect/login role and protocol generation;
- send GetCS/count/cancel;
- wait on or release a durable transition barrier;
- stop/resume/kill a process;
- stop reading or shrink buffers;
- restart and force fd/id reuse;
- assert invariant/control snapshots.

Use partial-order reduction: actions on different cancellation keys commute,
so retain one canonical ordering unless they share a resource, connection,
worker, or capacity cut. Then mutate each canonical trace at every legal TCP
message boundary while preserving per-channel FIFO.

## Phase 7 — scale and release gate

Run three tiers.

### Tier A: every commit

- Python model;
- deterministic logic tests;
- `make check`;
- generated small traces;
- ASan/UBSan;
- negative-control revert for the touched property.

### Tier B: nightly/ad-hoc isolated matrix

- all role/version combinations over p43, p44, p48, and candidate p49;
- 1k/5k/20k/40k and 48k workloads;
- 21+ simultaneous pending-output groups;
- freezes, reset/half-close, scheduler/F restart, fd and tiny-id reuse;
- heterogeneous eligibility graph;
- p95/p99/max control latency, jobs/sec, CPU, RSS, cleanup backlog.

### Tier C: release candidate farm dry run

- candidate scheduler under unique netname;
- A/B against deployed 1.4.90;
- real cold 2,200-TU build with the production fan-in;
- selected 13-F/792-slot run where operationally safe;
- long soak with transition/invariant counters continuously scraped;
- no production daemon interruption.

## Final independent acceptance (`mickgvirtu`)

The final report must answer each item with trace IDs and artifacts:

1. Which safety invariants were checked online and at what maximum state size?
2. Which liveness claims are unconditional, and which require fairness,
   failure, or bounded-delay assumptions?
3. Which generated counterexamples kill the previous implementation?
4. Did every new protocol path pass duplicate, loss, reconnect, restart, and
   mixed-version projections?
5. Did a fast host actually exercise fairness, rather than skip an absent
   witness?
6. Did Hall-slack tests include overlapping and rare eligibility sets?
7. Were stale identifiers forced, not merely waited for?
8. Are cancellation and teardown control latencies bounded independently of
   batch size?
9. Do old topologies retain their old externally visible traces, including
   failure/divergence behavior?
10. Does reverting each product fix make its specific gate red?

Release is blocked by any unexplained invariant delta, unbounded scheduler
turn, new permanent wait in an old-version projection, or test whose claimed
window is merely inferred from polling.
