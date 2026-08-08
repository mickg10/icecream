# Icecream temporal-ownership model

This directory is an executable design/audit layer for the scheduler protocol.
It is aimed at the topology

- one scheduler `S`;
- 10–50 fulfillment daemons `F`, each with one or more compile slots;
- thousands of compiler clients `C`, mediated by submitting daemons;
- rolling upgrades in which old (`F`, `C`) and new (`F'`, `C'`) peers coexist
  behind a new scheduler `S'`.

It is **not** a mutex model. The scheduler is mostly a single-threaded event
loop. The hard interleavings are distributed: independently buffered FIFO
channels, message segmentation, backpressure, delayed acknowledgements,
connection replacement, and identifier reuse.

## Run the executable model

```sh
python3 formal/icecc_temporal_model.py all
python3 formal/icecc_temporal_model.py all --json-dir /tmp/icecc-traces
```

The program has no dependencies beyond Python 3. It produces five expected
counterexamples and three bounded positive checks:

1. the pre-`441b022` staged-record early return leaves a same-key queued member;
2. the unified cancellation cut removes staged and queued members;
3. a frozen client after `UseCS`, before `JobBegin`, can retain a legacy-F slot
   forever;
4. an `S' -> F'` cancel-before-start tombstone closes that liveness hole;
5. aggregate dispatch credit passes while an eligibility-constrained workload
   has no compatible free slot;
6. a delayed `JobDone` can hit a new job after bounded-ID reuse;
7. polling can miss a real but short-lived contention interval;
8. protocol features project monotonically onto independently negotiated links.

Generated JSON traces are intended to be compiled into `schedbp` cases,
packet/fault scripts, or farm jobs.

## 1. Temporal ownership hypergraph

For every logical compile member `j`, define the color

```text
τ(j) = (scheduler_epoch, submitter_connection_generation,
        local_client_id, request_generation, member_index, scheduler_job_id)
```

Old wire protocols expose only a projection of this color, chiefly
`(connection, local_client_id, scheduler_job_id)`. New internal state should
retain the full color even when an old peer cannot see it.

The dynamic hypergraph has these primary places:

```text
unmaterialized -> staged -> queued -> dispatched -> started -> terminal
                              |            |
                              |            +-- dispatch-debt token
                              +--------------- scheduler ownership
                                           +-- fulfillment-slot token
```

A batch is a hyperedge joining all members of one request. The first selected
environment is batch state, not an incidental pointer from a “master” job to
all siblings.

There are also *index edges* (`jobs`, request group, score index, worker job
list, pending-expansion record). Index edges may be redundant; primary
ownership may not be. A live member must have exactly one primary place and
all indexes must agree with it.

### Petri-net P-invariants

For each member `j`:

```text
Σ place(j) = 1

debt(j) = 1 iff place(j) = dispatched
reservation(j) = 1 iff place(j) ∈ {dispatched, started}
```

For each submitter `d`:

```text
Σ debt(j), submitter(j)=d <= effective_credit(d)
```

For each fulfillment daemon `f`:

```text
Σ reservation(j), worker(j)=f <= advertised_capacity(f) + explicit_preload(f)
```

For each request `b`:

```text
unmaterialized(b) + Σ members_in_all_places(b) = requested_count(b)
```

The last invariant permits bounded incremental materialization; it does not
require all `count` members to exist simultaneously.

## 2. Cancellation is a temporal cut

Let `π(j)` map a member to the cancellation key visible on the negotiated
protocol. For the current wire this is normally
`(submitter connection generation, local_client_id)`.

A cancellation linearization is a cut of the scheduler-owned fibre:

```text
Cut(k) = { j | π(j)=k and place(j) ∈ {unmaterialized, staged, queued} }
```

The old defect cut only the staging component and returned. The fixed code
cuts staging and queued storage domains in one handler. A scalable design
should make the **logical cut O(1)**:

1. mark the first-class batch/fibre `CANCELLED`;
2. selectors reject cancelled members immediately;
3. reclaim physical jobs/indexes in bounded quanta on later loop turns.

This preserves the safety linearization without making a million-member
cancellation one uninterruptible scheduler turn.

## 3. Safety and liveness properties

A compact set is in [`properties.ltl`](properties.ltl). The most important
ones are:

```text
G(cancel_linearized(b) -> G(no_future_dispatch_of_unassigned_member(b)))
G(debt(j) <-> dispatched_unconfirmed(j))
G(reservation(j) -> assigned_to_that_worker(j))
G(connection_dead(g) -> F(no_reference_to_generation(g)))
G(pending_batch(b) -> F(completed(b) | cancelled(b) | owner_disconnected(b)))
```

The tempting property

```text
G(debt(j) -> F settled(j))
```

is **false on the current legacy path**. A client can freeze after `UseCS`
while its daemon and TCP connection remain healthy. No `JobBegin`, 107 bounce,
`JobDone`, or connection failure is then required to occur.

### Indistinguishability lemma: timeout-only reclaim is impossible

Consider two executions that are identical to `S` through an arbitrary time
`T`:

- `slow`: the client is delayed and reaches `F` after `T`;
- `dead`: the client vanished and will never reach `F`.

If `S` releases the reservation at `T` to guarantee bounded progress in
`dead`, it makes the same decision in `slow`. With the existing alphabet, `F`
has received no revocation and can accept the late compile, so the scheduler
has either double-allocated the slot or lost accounting ownership. If `S`
never releases, `dead` violates bounded liveness.

Therefore safe bounded reclamation requires at least one of:

- a stronger failure/delay bound;
- a new acknowledgement/revocation handshake;
- a fulfillment-side lease/tombstone that rejects late arrivals.

This is an observation-equivalence argument, not a claim about thread locks.

## 4. Backward-compatible `CancelBeforeStart` extension

The least invasive extension needs **new `S'` and new `F'`, but not new `C'`**.
The existing compiler request already carries the scheduler job id to `F`.

Proposed protocol-49 exchange on the persistent `S' <-> F'` channel:

```text
S' -> F' : CANCEL_BEFORE_START(job_id)
F' linearizes one of:
    CANCELLED   -- no compile has started; install a tombstone
    STARTED     -- JobBegin/compile already won the race
F' -> S' : CANCEL_BEFORE_START_RESULT(job_id, result)
```

`S'` releases the reservation only after `CANCELLED`. A late old or new client
that presents the tombstoned `job_id` is rejected before the compiler starts.
Normal dispatch has no extra round trip; only exceptional reclamation does.

Compatibility behavior:

| Topology | Negotiated `S-F` | Bounded pre-start reclaim |
|---|---:|---|
| `S'FC` | 43 | no; retain legacy ownership |
| `S'F'C` | 49 | yes; old client is acceptable |
| `S'F'C'` | 49 | yes |
| `S'F[CC']` | 43 | no |
| `S'F'[CC']` | 49 | yes for jobs assigned to `F'` |

Tombstones must be scoped against scheduler restart and job-id reuse. A new
64-bit scheduler epoch is ideal for new peers. Old peers require a no-reuse
window, persistent/randomized allocation, or an operational restart bound.

## 5. Compatibility as trace refinement

Let `Σ_v` be the message/field alphabet of protocol version `v`, and let
`π_v` erase fields and internal transitions introduced after `v`.

For every independently negotiated link, require:

```text
decode_v(encode_v(m)) = π_v(m)
```

For whole-system behavior, use weak/alternating trace refinement:

```text
π_43(Traces(S' || F || C)) subseteq Traces(S_43 || F_43 || C_43)
```

Internal staging, bounded cleanup, and a new-F revocation exchange are silent
steps under the old projection. Failure/divergence observations also matter:
a new implementation that preserves successful replies but introduces a new
permanent wait does not refine the old protocol.

The negotiated version is per link, never a single cluster-wide scalar.

## 6. Eligibility graph and Hall-slack admission

Model fulfillment capacity as a bipartite `b`-matching graph:

- left vertices: waiting job/submitter eligibility signatures;
- right vertices: fulfillment daemons or individual slot copies;
- edge `(q,f)`: `f` can ever run `q` (platform, environment, feature, policy);
- right-side capacity: advertised slots plus explicitly modeled preload.

The current aggregate credit clamp can leave one *global* slot free while
occupying every slot in a constrained job's neighborhood. Individual
per-signature limits are still insufficient when neighborhoods overlap.

The exact feasible region is a transversal polymatroid. For every set `X` of
active demand classes:

```text
Σ reservations(X) <= capacity(N(X)) - protected_slack(X)
```

A min-cut is a separation oracle for violated Hall inequalities. With only
10–50 fulfillment daemons, an incremental max-flow/matching check over
**eligibility signatures**, not every job, is practical as a correctness
oracle and can later be approximated by cached neighborhood credits.

## 7. Complexity audit and first-class batches

Current shapes to remove before farm scale:

- cancellation scans the global `jobs` map and, for every match, scans request
  groups: approximately `O(total_jobs + batch_size * submitter_groups)`;
- each enqueue scans request groups: `O(batch_size * submitter_groups)`;
- selection/removal/reinsertion scans groups on each dispatch: `O(groups)` per
  job, with worse paths for unschedulable candidates;
- activation of a completed expansion enqueues the entire batch in one turn;
- cancellation and daemon teardown physically delete an arbitrary number of
  jobs in one turn;
- daemon `find_by_client_id` is linear in the number of local clients.

A first-class `Batch` object solves several issues together:

```text
Batch {
    internal_generation;
    wire_client_id;
    requested_count;
    next_member;
    state: OPEN | PINNED | CANCELLED | COMPLETE;
    pinned_environment;
    intrusive/member index;
}
```

Jobs point to `Batch`; they do not form a raw-pointer master/sibling star.
The first dispatch pins the batch environment exactly once. Later members can
be materialized and dispatched incrementally while preserving that pin, so
memory is `O(window)` instead of `O(count)`. Cancellation marks the batch and
cleanup walks its own member list in bounded quanta.

Use direct indexes for:

- `(submitter, niceness) -> request group`;
- `job -> owning request group`;
- cancellation key -> batch/fibre;
- `client_id -> Client` in the daemon.

## 8. Identifier freshness is an assumption today

The production daemon increments `client_id` during one scheduler connection
and resets after clearing clients on scheduler loss. The issue-2 regression
that reuses one client id on one live connection is therefore a robustness
case, not the normal daemon trace.

The wire does not encode a request generation. If reuse is allowed, exact
“cancel only the later request” semantics are impossible: both requests have
the same observable cancellation key. The implementation must either:

- state and validate freshness as a protocol assumption;
- cancel all aliases, as the current robust sweep does; or
- add a request nonce for new peers.

The same argument applies to 32-bit scheduler job-id wrap. A delayed old
`JobDone` and a completion for the newly reused id are indistinguishable.

## 9. Partial-order reduction and cutoffs

Independent transitions on different `(connection generation, client id)`
keys commute. Treat executions as Mazurkiewicz traces/happens-before DAGs,
not every byte/thread interleaving.

Small cutoffs cover the known classes:

- cross-domain cancellation: one submitter, two same-key batches, one worker;
- backpressure blast radius: one frozen and one healthy submitter;
- eligibility starvation: two eligibility signatures and three slot groups;
- ABA: a three-value abstract identifier space;
- mixed-version refinement: versions 43, 48, and proposed 49.

Farm scale is then for performance constants, rare kernel/network behavior,
and long-run identifier/churn tests—not for discovering the basic logical
counterexample.

## 10. Implementation and independent acceptance program

### Implementation team (`mickg10`)

1. Add scheduler-internal generation/batch identity and compact transition
   events; do not change the wire yet.
2. Add online assertions for ownership, queue/index, debt, and reservation
   invariants.
3. Introduce first-class batches and O(1) logical cancellation with bounded
   cleanup.
4. Add direct queue/client indexes and benchmark 1k/5k/20k submitting clients.
5. Add the Hall-slack oracle in diagnostic mode; compare its decisions with
   aggregate credit under heterogeneous farms.
6. Specify and implement protocol-49 `CANCEL_BEFORE_START` for `S' <-> F'`.
7. Add a real mixed-version matrix built from protocol-43 and current/new
   binaries.

### Independent checker (`mickgvirtu`)

Acceptance is counterexample-oriented:

- replay every JSON trace against pre-fix, current, and candidate branches;
- mutate message cut points and ordering while preserving per-channel FIFO;
- freeze exactly between `UseCS`, local delivery, F connect, `JobBegin`, and
  `JobDone`;
- force duplicate login, scheduler/F restart, fd reuse, and job-id reuse in a
  tiny test allocator;
- use heterogeneous eligibility neighborhoods, not only interchangeable fake
  workers;
- verify old clients on new F and old F on new S independently;
- reject wall-clock sleeps as proof of liveness when a durable transition
  counter or barrier can witness the property.

The final gate is not “all tests happened to finish.” It is that every
accepted trace satisfies the invariants and every promised liveness property
has its assumptions stated and exercised.
