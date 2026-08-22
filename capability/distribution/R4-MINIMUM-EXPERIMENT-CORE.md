# R4 minimum experiment core

Status: implemented and locally gated

## Provenance and integration target

This slice starts from the accepted simulator/research head
`e45e5bd92924c05426a27e262d0fa7434fa40261` and implements the minimum
experiment contract reviewed in `doc/issue16-strategic-plan.md` at
`b759daf7aa9010c7156d9e307a568da66f23f501`. The first implementation commit is
`bd62cf81224ce5ba69ea27bb0e19beb930a6ecf2`; the independent-closure correction
core is `15bb47d47ba96e66d61a7c55ac023b26c4dc9631`.
The independent retained-evidence/cardinality correction is
`12a0fa6d934b3fee4037cb8ed9ebb049b29e34ef`; its static-policy closure follow-up
is `4c2b184d77bfc10c51b8ca1d189d36cc21d4f94a`.
The closed retained-stream validator source is
`4920b68995599eed94d8910930a803bd477b4803`; retained evidence records that
exact commit and the SHA-256 of its `run_scenario.py` blob. The subsequently
committed `simulator-source.json` sidecar preserves that binding in source
archives without a `.git` object database; worktrees additionally compare the
declared commit to the literal Git blob.

Those two source heads diverge after `c697107e`. This branch deliberately keeps
the accepted simulator lineage and does not import the product/cache endpoint
history. Its eventual integration base is the P48-derived unified P50 successor
after corrected M2, P49 assignment, P50 identity, and the inert cache
advertisement have converged. That product head is not frozen yet. In
particular, this work must not be integrated onto the older P44 product base.

## Frozen boundary

The v2 scenario is immutable and mode-neutral. It contains component versions,
selected capabilities, topology, workload identity, environment/cache initial
state, and expected outcome. It contains no execution mode, host, start time, or
runner identity. Its identity is the SHA-256 of canonical JSON serialization.

One separate `icecream-execution-v2` document selects `simulated` or `physical`
and records the realization: source and runner commits, host manifest, images,
compiler/library versions, codec executable digest, input manifest digests, and
host identities. `run_scenario.py` accepts only `mode=simulated`; the physical
launcher will validate the same header and consume the same scenario digest.

The checked schemas are:

- `experiment.schema.json`: mode-neutral scenario;
- `execution.schema.json`: one execution realization;
- `workload-input.schema.json`: canonical per-TU content and timing inputs;
- `event.schema.json`: one lifecycle event and its field-level provenance;
- `timeline.schema.json`: the exact canonical snapshot/gap envelope;
- `summary.schema.json`: the exact canonical v2 result field set;
- `route-trace.schema.json`: physical assignment/route trace rows.

`z3_long` and `z3_shared_long` are valid future product profile names.
`z3_shared_long_b1` exists only in `experiment_control`; it is the prebuilt
prefix comparison label, not a product profile. No codec for these three labels
is implemented in this R4 slice. The obsolete placeholder labels `stream_a` and
`stream_b` are not product profiles and are rejected.

Every v2 trace row declares `raw_sha256` and whether its compile duration is
`observed` or `modeled`. The workload digest is the canonical digest of all
parsed row identities, relative payload names, sizes, content digests, compile
durations, compile models, and duration provenance. Loading recomputes every
payload digest and that complete manifest. The execution header must name
exactly the same input-manifest digest set. Changing a payload without changing
its length therefore still refuses the run.
The parsed content manifest is the retained trace claim: v2 does not publish a
second checksum for an external trace pathname whose literal bytes are not in
the retained result bundle.

## Scored-release and environment rules

For every v2 job, `workload.release_policy` is exactly `all_at_once`. Every TU in
a build is available at that build's release boundary, so producer timing and
preprocessing do not enter source-transfer or makespan scores. A later warm
build may still be separated by the declared build barrier; the rule describes
input availability, not permission to overlap state-dependent build epochs.

Primary scenarios use `environment.initial_state=resident`. They emit no
environment traffic and pay no install delay. An explicit stress scenario may
use `absent`. The first assignment on each `(C_STORE_GUID,F)` route starts one
environment transfer; concurrent transactions join that single flight. Input
traffic proceeds independently, and compile starts only after both the immutable
input record and environment are ready. The stream emits `env_transfer`,
`env_install_verify`, and `environment_ready` transitions. Environment bytes
are included in total network and utilization ledgers but excluded from the
source-codec score.

## Identity and event contract

Each exact event contains the applicable value, or JSON `null`, for:

```text
logical_job_id, attempt_id
C_STORE_GUID, physical_endpoint, RouteLaneId, F_STORE_GUID, session_serial
HISTORY_NONCE, REL_SEQ, TU_SEQ, transaction_digest, raw_digest
negotiated_profiles, route_state_profiles, InputRecord_identity
actor, start_ns, end_ns, duration_ns, field-level provenance, byte_account
c_to_f_byte_delta, f_to_c_byte_delta
resource_byte_delta, queue_byte_delta
```

The stable simulated identifiers are domain-separated hashes of the scenario,
logical input, C/F route, sequence numbers, raw digest, negotiated profiles, and
route-state profiles. `InputRecord_identity` is independently recomputed from
that transaction digest and the raw-content digest.
Trace replay preserves the physical C/F/session/route identities instead.
Legacy event names remain for v1 readers; the additive `stage` field carries the
canonical lifecycle vocabulary. A single closed event table drives both the
producer and validator, including event/stage/actor/transition/phase and exact
instant-event shapes. Provenance is not one blanket label: timing,
byte delta, route identity, raw digest, compile-duration input, and derived
resource/queue deltas are classified independently. Simulator event timing is
always modeled even when a physical codec supplied observed byte counts.

## Exact ledgers

Directional bytes are charged once, at completed serialization, by direction,
route, and account (`source`, `environment`, or future `result`). Source C-to-F
bytes remain the score. Environment C-to-F and all F-to-C bytes are reported
separately and enter network time.
Flow IDs are globally allocated exactly once as a contiguous range. Their full
transaction, route, profile, phase, direction, size, and account descriptor is
immutable from queue through delivery; only the matching `flow-sent` event may
charge its exact directional byte extent.

The engine also records byte credits and debits for:

```text
resources: prepared input, immutable InputRecord, propagating network bytes
queues: scheduler-ready, route-input-pending, compiler-input, network-outstanding
```

A debit may never make a balance negative. Every transient balance must be zero
at completion. The engine validates closure before returning a result, and
`validate_experiment_jsonl()` validates the emitted execution and event schemas,
recomputes the embedded scenario and workload digests, reconciles execution
inputs and selected outcomes, and independently replays event count, per-account
and per-route directional totals, every credited/debited/peak/final transient
summary, route identities, and replay totals. It also derives TU, build, job,
route, and relationship cardinalities from the manifest, workload rows, and
events; checks contiguous wall/active timeline records and chronological event
containment; and proves that every v2 TU release has zero offset from its
derived build boundary.

Global chronology is not treated as a substitute for TU-local causality. The
validator independently groups every event by TU, validates each network flow's
queued/start/yield/resume/sent/finish state machine, and enforces two partially
ordered lifecycle chains: release/assignment/dialogue/input-ready/compiler start/
compiler finish, and dialogue/relationship commit/dialogue finish. Both must join
exactly once at transaction completion, which must be that TU's final event. The
compiler-finish timestamp minus compiler-start timestamp must equal that TU's
manifest `compile_ns` exactly. Optional environment transfer/install/ready events
form their own ordered sub-lifecycle with exact interval seams. Every TU waiting
on an initially absent route must start compilation after that route's single
readiness event. Every input-contributing C-to-F source flow must be delivered
before input readiness, every source flow must be delivered before relationship
commit/dialogue finish, and transaction completion is exactly the maximum of
compiler finish and transaction commit. This
allows arbitrary cross-TU interleaving and fork/join transfer overlap without
allowing a single TU to skip, repeat, or reverse a represented stage. `input-ready`
is the current materialization boundary; the minimum core does not invent separate
source decode/install CPU stages that the simulator does not yet model.

## Assignment replay

`topology.assignment_source=route_trace` requires a complete physical trace.
Every logical job and attempt must occur exactly once. `TU_SEQ` is contiguous per
C, `REL_SEQ` is contiguous per C/F relationship, and worker, route lane,
physical endpoint, session, per-TU C-to-F bytes, and per-TU F-to-C bytes must all
match. A trace header must name the selected codec and observed/modeled route
provenance. C, F, lane, session, and nonce identities cannot drift on an existing
route because this minimum schema contains no identity-transition record. The
final report closes each route exactly, including environment traffic; the
source score remains a separate account.

`assignment_source=policy` deliberately reruns the named scheduling policy. When
an exact physical codec ledger is supplied, TU reconstruction and aggregate
directional bytes remain exact, but a small timing change may choose another F.
The report therefore claims aggregate directional closure only; it does not
mislabel a different assignment as an exact route replay.

Every v2 run writes `route-trace.jsonl` as retained assignment evidence. Policy
runs write the realized route trace. Exact replay runs retain the input trace
byte-for-byte, and the execution record names its relative path and SHA-256.
Validation reloads those literal bytes, recomputes their SHA-256, validates the
trace header/assignments/summary, and compares every applicable event and exact
route-summary identity and source-byte total directly with the retained rows.
An exact replay input must carry the digest of the already-frozen route-trace
scenario and its selected codec.

A physical codec result also retains `physical-ledger.jsonl`. The execution
record binds its literal digest; validation compares its descriptor and summary
with codec metadata, checks every exact TU row against the workload content
manifest, and reconciles each TU's directional phase bytes with the event
ledger. Diagnostic adapters cannot carry this evidence or claim a physical
result.

## Retained acceptance fixture

`samples/r4-minimum/` contains a two-TU, C1F2 exact-route replay:

- scenario digest:
  `9411bba01333a1ff6340d8a5de141e0e246467c7e9a8c3ae90d156587abafa51`;
- workload content-manifest digest:
  `5cb0ecacfd3f84e48ff80aff99738d2232964c797b583caa586582de5b9f5636`;
- input route-trace SHA-256:
  `340ef69297e1c8b05cfa3eb6adcb7191b0cab7085017e470cbef0658df639c3d`;
- retained route evidence: `route-trace.jsonl`, byte-identical to the input
  trace above;
- retained `sample-experiment.jsonl` SHA-256:
  `14797f57983bb9f2cdaaa639753a63413f8cf90f191aabd755c8cfa363b898fd`;
- closure: 30 events, 37 source C-to-F bytes, zero F-to-C bytes, two exact
  routes, and every resource/queue balance returned to zero.

The retained JSONL stores only manifest-relative/logical input identities; two
otherwise identical checkout roots reproduce the same bytes. Snapshot state and
rate metrics live under `noncanonical_display`: they remain available to the
HTML report but are explicitly outside the canonical event/accounting result.

Reproduce it from the repository root:

```bash
python3 capability/distribution/run_scenario.py \
  capability/distribution/samples/r4-minimum/experiment.json \
  --execution capability/distribution/samples/r4-minimum/execution.json \
  --codec raw --require-payload --out /tmp/r4-minimum
```

Then validate the canonical stream:

```bash
python3 capability/distribution/validate_experiment.py \
  /tmp/r4-minimum/experiment.jsonl
```

## Local gates and deferred seams

The implementation gate is:

```bash
make -f Makefile.am integration_tests
ruff check capability/distribution/run_scenario.py \
  capability/distribution/test_run_scenario.py
black --check capability/distribution/run_scenario.py \
  capability/distribution/test_run_scenario.py
```

The minimum core does not add product endpoints, the physical launcher, GUI
catalog work, preprocessing, F decode/install/materialize/compiler-pipe CPU
models, result-return traffic, or restart/eviction/reroute events. The last
three simulator additions remain ordered follow-ups. The most likely textual
integration conflict is `run_scenario.py` if later F-stage work lands first;
schemas and fixtures are additive. Product/cache endpoint branches should have
no file-level conflict with this change.
