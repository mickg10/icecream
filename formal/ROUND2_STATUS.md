# Assignment-fence formal checkpoint status

Working branch: `bigoracle/assignment-fence-core-round2`

This branch is a **construction and execution handoff** for
`mickg10/local-oracle`. It is not accepted formal evidence. No TLC, TLAPS,
compatibility, implementation-refinement, or performance result is claimed by
this document.

## Conserved ownership tokens

The core and network models separate:

```text
schedulerReservation[a]  -- S still accounts for assignment a
workerSlot[a]             -- F still consumes physical capacity for a
```

After F linearizes `REVOKED`, `workerSlot[a]` is false while
`schedulerReservation[a]` remains true until S consumes the complete result.
A one-token model cannot represent the safe interval between those two
linearization points.

## Current formal artifacts

### Abstract core

- `AssignmentFenceCore.tla`
- fixed, mixed-fleet, premature-release, fair-liveness, and two-worker
  heterogeneous configurations
- `AssignmentFenceCoreProof.tla`: strengthened inductive TLAPS proof candidate
  with named per-action preservation lemmas and no omitted obligations

### Finite FIFO network

- `AssignmentFenceNetwork.tla`
- bounded S→F, F→S, and S→D FIFO streams
- queueing, frame flushing, and protocol consumption are distinct transitions
- strict READY-before-UseCS
- exact/legacy claim policy
- REVOKED enqueue distinct from S consumption/release
- stale READY, UseCS, STARTED, BEGIN, DONE, and late-REVOKE races consumed
  without state resurrection
- explicit S–F and S–D session loss
- fixed safety/liveness configs and direct-property mutants for:
  - UseCS before READY;
  - release when F queues rather than S consumes REVOKED;
  - F→S FIFO bypass; and
  - default-allow late legacy start after bounded record compaction

`AssignmentFenceNetworkCompaction.tla` extends the canonical network state with
terminal-record compaction. This closes a prior model-vacuity gap: without
compaction, an F record remained `Revoked`, so a delayed claim could never
become an unknown claim and the finite-tombstone/default-allow mutant was
unreachable.

### Trace discrimination

- `trace_to_harness.py`
- `trace_to_harness_test.py`
- `TRACE_ADAPTER.md`
- declarative templates under `trace-manifests/`

TLC JSON traces contain state records but not action labels or the lasso edge.
The adapter classifies adjacent state deltas, requires the intended essential
event subsequence and final property violation, reads the retained TLC text log
when a lasso is required, and emits deterministic C++ barrier instructions. It
rejects empty, zero-state, malformed, ambiguous, wrong-order, wrong-property,
and path-invalid traces. Manifests execute no Python expressions.

### Pinned acceptance runner

- `formal-checks.json`
- `run_formal_checks.py`

The first matrix contains fixed and mutant abstract-core checks, one-worker
liveness, a two-worker heterogeneous cutoff, fixed and compacting finite-network
checks, four load-bearing network mutants, finite-network liveness, and the
TLAPS core proof.

The runner downloads nothing and requires:

```text
clean exact git revision
stable TLC jar path + expected SHA-256
differential TLC jar path + expected SHA-256
tlapm path + expected SHA-256
full Java and tlapm version output
GNU time peak-RSS evidence
one worker for every authoritative liveness run
explicit CHECK_DEADLOCK in every config
no unapproved state/action constraints
no symmetry in authoritative runs
nonzero generated/distinct state counts
complete state-space result
intended property named directly by each mutant config
retained JSON trace + trace-to-harness validation for every counterexample
actual all-obligations-proved TLAPS result
```

## Required execution

From a clean checkout at the exact branch revision:

```sh
python3 formal/trace_to_harness_test.py

python3 formal/run_formal_checks.py \
  --manifest formal/formal-checks.json \
  --repo . \
  --artifacts /absolute/path/to/immutable-run-dir \
  --expected-git-sha <FULL_GIT_SHA> \
  --stable-jar /cache/tla2tools-1.7.4.jar \
  --stable-sha256 <LOCALLY_VERIFIED_64_HEX_SHA256> \
  --differential-jar /cache/tla2tools-differential.jar \
  --differential-sha256 <LOCALLY_VERIFIED_64_HEX_SHA256> \
  --tlapm /absolute/path/to/tlapm \
  --tlapm-sha256 <LOCALLY_VERIFIED_64_HEX_SHA256>
```

Retain the entire artifact directory. The issue reply must include the exact
revision, commands, jar/binary hashes, Java/tlapm/backend versions, generated
and distinct state counts, depth, elapsed time, peak RSS, process exits, the
named property/result for every run, JSON trace paths, harness-adapter results,
and the TLAPS obligation summary.

## Deterministic reference constructors

The Python reference constructors remain useful for expected trace names and
cross-checking model intent. They are not TLC or TLAPS evidence and cannot be
used as acceptance substitutes.

## Still required beyond this first matrix

The first executable layer does not by itself close issue #4. The canonical
acceptance branch must still incorporate and execute:

- protocol-43 frozen-client/restart counterexamples;
- exact compatibility projection plus OLD/NEW socketpair codec fixtures;
- lifecycle-authority, pre-login, allocator, handoff, and F-session-quiescence
  TLC configs with direct-property mutants and trace manifests;
- exact model-to-message/handler/test mapping;
- mickg10 cluster mixed-version, fault, scale, latency, throughput, memory,
  descriptor, and scheduler-turn evidence; and
- final independent `mickgvirtu/implementer` review only after the mickg10
  evidence is complete.
