# Assignment-fence formal checkpoint status

Working branch: `bigoracle/assignment-fence-core-round2`

This branch is a **construction and execution handoff** for
`mickg10/local-oracle`. It is not accepted formal evidence. The first real
execution against an earlier branch revision was correctly red: it exposed
runner/trace incompatibilities, thirteen unproved TLAPS obligations, an
over-broad core fairness assumption, and missing claim/revoke concurrency in
the finite network model. The branch now contains corrections for those
findings, but no post-correction TLC or TLAPS result is claimed here.

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
- fixed, mixed-fleet, premature-release, and two-worker heterogeneous
  configurations
- `AssignmentFenceCoreProgress.tla`: the accepted liveness experiment assumes
  weak fairness only for S consuming an already-linearized `REVOKED` result;
  compiler completion and worker loss are named separately as environmental
  outcomes and are not silently assumed
- `AssignmentFenceCoreProof.tla`: a smaller TLAPS ownership/identity proof
  candidate with explicit per-action preservation lemmas and no omitted
  obligations

The TLAPS theorem covers the logical facts that must not depend on a finite
cutoff: distinct scheduler/worker ownership tokens, release/terminal agreement,
claim preparation evidence, token-required exactness, claim/revoke exclusion,
and live full-identity uniqueness. `CapacityBound` remains checked by both
pinned TLC toolchains; it is not relabeled as an unbounded TLAPS theorem.

### Finite FIFO network

- `AssignmentFenceNetwork.tla`
- bounded S→F, F→S, S→D, and delayed C→F FIFO streams
- queueing, frame flushing, and protocol consumption are distinct transitions
- concrete claim correlation fields: wire id, full assignment id, and token
- F claim/revoke decisions depend on F-visible state and received frames, not
  the scheduler's instantaneous phase
- strict READY-before-UseCS
- REVOKED enqueue distinct from S consumption/release
- explicit `OWNED` response when a concrete claim wins before F consumes a
  queued REVOKE
- delayed concrete claim rejection when F installs the fence first
- stale READY, UseCS, OWNED, BEGIN, DONE, and late-REVOKE frames consumed
  without state resurrection
- explicit S–F and S–D session loss
- fixed safety/liveness configs and direct-property mutants for:
  - UseCS before READY;
  - release when F queues rather than S consumes REVOKED;
  - F→S FIFO bypass; and
  - default-allow late legacy start after bounded record compaction

Two direct fixed-model reachability witnesses are mandatory:

```text
S queues REVOKE; F consumes an already-delivered concrete claim first
F consumes REVOKE and installs the fence; delayed concrete claim is rejected
```

Each witness has a direct invariant, trace manifest, and deterministic barrier
instructions. This prevents a safety result from passing because either side
of the distributed race was accidentally disabled.

`AssignmentFenceNetworkCompaction.tla` extends the canonical network state with
terminal-record compaction. The default-allow mutant now consumes the actual
legacy claim frame produced by `DReceiveUseCS` and retained in C→F; it is not a
spontaneous start action.

### Trace normalization and discrimination

- `normalize_tlc_trace.py`
- `normalize_tlc_trace_test.py`
- `tlc_text_trace.py`
- `tlc_text_trace_test.py`
- `trace_to_harness.py`
- `trace_to_harness_test.py`
- `TRACE_ADAPTER.md`
- declarative templates under `trace-manifests/`

TLC toolchains do not emit one common trace shape. TLA+ Tools 1.7.4 provides
its counterexample in the text log; newer TLC emits graph-shaped JSON under
`counterexample.state` as `[ordinal, state]` pairs. The normalizer converts
both forms to one canonical state array. The adapter then classifies adjacent
state deltas, requires the intended essential event subsequence and final
property violation, reads the retained TLC log when a lasso is required, and
emits deterministic C++ barrier instructions. It rejects empty, zero-state,
malformed, ambiguous, wrong-order, wrong-property, and path-invalid traces.
Manifests execute no Python expressions.

### Pinned acceptance runner

- `formal-checks.json`
- `run_formal_checks.py` — canonical entry point
- `run_formal_checks_v2.py` — tool/backend pinning and differential matrix
- `run_formal_checks_v3.py` — corrections layered over v2 after the first real
  red run

The runner downloads nothing and requires:

```text
clean exact git revision
artifact directory outside the checkout
stable TLC jar path + expected SHA-256
differential TLC jar path + expected SHA-256
tlapm path + expected SHA-256
exact pinned Isabelle, Zenon, Z3, and LS4 backend paths/hashes
full Java, TLC-help/capability, tlapm, and backend version output
positive GNU time peak-RSS evidence
one worker for every authoritative liveness run
explicit CHECK_DEADLOCK in every config
no unapproved state/action constraints
no symmetry in authoritative runs
nonzero generated/distinct state counts and reported depth
complete state-space result for every passing run
intended property named directly by each expected counterexample config
nonzero mutant exit plus the directly named violation
canonical nonempty trace + trace-to-harness validation for every counterexample
stable/differential state-count, depth, essential-event, and harness-step agreement
actual all-obligations-proved TLAPS result
```

An expected counterexample may naturally stop with unexplored states on TLC's
queue. The runner no longer confuses that with a truncated passing run; it
instead requires the direct named violation and a manifest-valid trace. A
passing run must leave no states queued.

## Required clean execution

Run from a clean checkout at the exact branch revision. Put the artifact
directory outside the repository and start with it absent or empty.

```sh
export PYTHONDONTWRITEBYTECODE=1
python3 formal/trace_to_harness_test.py
python3 formal/tlc_text_trace_test.py
python3 formal/normalize_tlc_trace_test.py

python3 formal/run_formal_checks.py \
  --manifest formal/formal-checks.json \
  --repo . \
  --artifacts /absolute/path/outside/checkout/formal-run \
  --expected-git-sha <FULL_GIT_SHA> \
  --stable-jar /cache/tla2tools-1.7.4.jar \
  --stable-sha256 <LOCALLY_VERIFIED_64_HEX_SHA256> \
  --differential-jar /cache/tla2tools-differential.jar \
  --differential-sha256 <LOCALLY_VERIFIED_64_HEX_SHA256> \
  --tlapm /absolute/path/to/tlapm \
  --tlapm-sha256 <LOCALLY_VERIFIED_64_HEX_SHA256> \
  --backend isabelle=/absolute/path/to/isabelle=<64_HEX_SHA256> \
  --backend zenon=/absolute/path/to/zenon=<64_HEX_SHA256> \
  --backend z3=/absolute/path/to/z3=<64_HEX_SHA256> \
  --backend ls4=/absolute/path/to/ls4=<64_HEX_SHA256>
```

Retain the entire artifact directory. The issue reply must include the exact
revision, commands, jar/binary hashes, Java/TLC/tlapm/backend versions,
generated and distinct state counts, depth, elapsed time, positive peak RSS,
process exits, named property/result for every run, raw and normalized trace
paths/hashes, harness-adapter results, stable/differential comparison, and the
TLAPS obligation summary.

## Evidence status

The deterministic Python reference constructors remain useful for expected
trace names and cross-checking model intent. They are not TLC or TLAPS evidence
and cannot be used as acceptance substitutes.

This execution environment could not clone the public repository because
outbound DNS is disabled. Therefore no local syntax, TLC, or TLAPS pass is
claimed from the edits above. `mickg10/local-oracle` must execute the exact
branch head with the already pinned tool inventory.

## Still required beyond this first matrix

The first executable layer does not by itself close issue #4. The canonical
acceptance branch must still incorporate and execute:

- protocol-43 frozen-client/restart counterexamples;
- exact compatibility projection plus OLD/NEW socketpair codec fixtures;
- lifecycle-authority, allocator, handoff, and F-session-quiescence TLC configs
  with direct-property mutants and trace manifests;
- exact model-to-message/handler/test mapping;
- mickg10 cluster mixed-version, fault, scale, latency, throughput, memory,
  descriptor, and scheduler-turn evidence; and
- final independent `mickgvirtu/implementer` review only after the mickg10
  evidence is complete.
