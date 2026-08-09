# Assignment-fence formal checkpoint status

Working branch: `bigoracle/assignment-fence-core-round2`

This branch is a **construction and execution handoff** for
`mickg10/local-oracle`. It is not accepted formal evidence.

Two real executions have been useful and correctly red:

1. the first exposed unsupported stable-TLC options, modern JSON-shape
   assumptions, incorrect mutant queue semantics, an over-broad liveness
   assumption, missing claim/revoke concurrency, and 13 unproved TLAPS
   obligations;
2. the canonical `c6e8053` rerun reached the third TLC check and exposed a
   stable text parser that appended TLC statistics to the last state value,
   plus backend “version” metadata that accepted nonzero error/usage output.

The current branch contains corrections for those findings. No post-correction
TLC or TLAPS result is claimed by this document.

## Conserved ownership tokens

The core and network models separate:

```text
schedulerReservation[a]  -- S still accounts for assignment a
workerSlot[a]             -- F still consumes physical capacity for a
```

After F linearizes `REVOKED`, `workerSlot[a]` is false while
`schedulerReservation[a]` remains true until S consumes the complete result.
A one-token model cannot represent that safe intermediate interval.

## Formal artifacts

### Abstract core

- `AssignmentFenceCore.tla`
- fixed, mixed-fleet, premature-release, and heterogeneous configurations
- `AssignmentFenceCoreProgress.tla`: weak fairness only for S consuming an
  already-linearized `REVOKED` result; compiler completion and worker loss are
  named environmental outcomes, not hidden product guarantees
- `AssignmentFenceCoreProof.tla`: small TLAPS ownership/identity proof candidate
  with explicit per-action preservation and standard `[Next]_vars`
  action-or-stutter induction

The TLAPS candidate covers logical properties that must not depend on a finite
cutoff: distinct scheduler/worker ownership tokens, release/terminal agreement,
prior preparation evidence, token-required exactness, claim/revoke exclusion,
and live full-identity uniqueness. `CapacityBound` remains dual-toolchain TLC
evidence and is not relabeled as an unbounded TLAPS theorem.

### Finite FIFO network

- `AssignmentFenceNetwork.tla`
- bounded S→F, F→S, S→D, and delayed C→F streams
- concrete wire/full-id/token claim correlation
- F claim/revoke decisions use F-visible state and received frames, not S’s
  instantaneous phase
- strict READY-before-UseCS
- REVOKED enqueue distinct from S consumption/release
- explicit `OWNED` result when a claim wins before F consumes queued REVOKE
- delayed concrete claim rejection when the fence wins first
- terminal compaction and concrete retained-claim default-allow mutant
- fixed-model reachability witnesses for both claim/revoke race orders

The two witness traces are mandatory so safety cannot pass by accidentally
disabling one distributed order:

```text
S queues REVOKE; F consumes the already-delivered claim first
F consumes REVOKE and installs the fence; delayed claim is rejected
```

### Trace normalization and discrimination

- `tlc_text_trace.py`
- `tlc_text_trace_test.py`
- `testdata/core-mixed-token-mutant-1.7.4.log`
- `normalize_tlc_trace.py`
- `normalize_tlc_trace_test.py`
- `trace_to_harness.py`
- `trace_to_harness_test.py`
- `TRACE_ADAPTER.md`
- declarative templates under `trace-manifests/`

TLA+ Tools 1.7.4 provides its counterexample in the text log. Newer TLC emits
graph-shaped JSON under `counterexample.state` as `[ordinal, state]` pairs.
Both are converted to one canonical state array before event classification.

The stable text parser now consumes continuation lines only while the current
TLA+ value is structurally incomplete. Once a variable value parses completely,
the first non-assignment top-level line ends the state block. The retained
mixed-token fixture places the statistics line immediately after state 3’s
`terminalCount`; its regression requires exactly three states and the exact
state-3 map.

The adapter requires the intended essential event subsequence and final
property violation, parses the retained lasso marker when required, and emits
deterministic implementation barrier instructions. Empty, malformed,
ambiguous, wrong-order, wrong-property, and path-invalid traces are red.

### Pinned acceptance runner

- `formal-checks.json`
- `run_formal_checks.py` — canonical entry point
- `run_formal_checks_v2.py` — exact tool/backend hashes and differential matrix
- `run_formal_checks_v3.py` — real TLC trace shapes and expected-mutant semantics
- `run_formal_checks_v4.py` — successful backend identities and outside-checkout
  artifact preflight
- `run_formal_checks_v4_test.py`

The runner downloads nothing and requires:

```text
clean exact git revision
artifact directory outside the checkout
stable and differential TLC jars with distinct expected SHA-256 values
pinned tlapm SHA-256
exact Isabelle, Zenon, Z3, and LS4 executable hashes
successful backend-specific version probes
LS4 embedded ls4-1.0 marker plus GNU ELF build ID
positive GNU-time peak RSS
one worker for every authoritative liveness run
explicit CHECK_DEADLOCK in every config
no unapproved constraints or authoritative symmetry
nonzero generated/distinct state counts and reported depth
zero queued states for every passing exploration
nonzero exit + directly named violation for every expected counterexample
canonical nonempty trace + manifest validation for every counterexample
stable/differential state-count, depth, event, and harness-step agreement
all TLAPS obligations actually proved
```

Backend identity is fail-closed:

```text
isabelle version
zenon -v
z3 --version
LS4: pinned hash + embedded package marker + ELF build ID parsed in Python
```

Nonzero usage/error output is never recorded as version evidence.

An expected counterexample may naturally stop with unexplored states on TLC’s
queue. A passing run may not. This distinction is implemented by the canonical
runner.

## Required clean execution

Run from a clean checkout at the exact posted branch head. The artifact path
must be absent or empty and outside the repository.

```sh
export PYTHONDONTWRITEBYTECODE=1
python3 formal/trace_to_harness_test.py
python3 formal/tlc_text_trace_test.py
python3 formal/normalize_tlc_trace_test.py
python3 formal/run_formal_checks_v4_test.py

python3 formal/run_formal_checks.py \
  --manifest formal/formal-checks.json \
  --repo . \
  --artifacts /absolute/path/outside/checkout/formal-run \
  --expected-git-sha <FULL_GIT_SHA> \
  --stable-jar /cache/tla2tools-1.7.4.jar \
  --stable-sha256 <LOCALLY_VERIFIED_SHA256> \
  --differential-jar /cache/tla2tools-differential.jar \
  --differential-sha256 <LOCALLY_VERIFIED_SHA256> \
  --tlapm /absolute/path/to/tlapm \
  --tlapm-sha256 <LOCALLY_VERIFIED_SHA256> \
  --backend isabelle=/absolute/path/to/isabelle=<SHA256> \
  --backend zenon=/absolute/path/to/zenon=<SHA256> \
  --backend z3=/absolute/path/to/z3=<SHA256> \
  --backend ls4=/absolute/path/to/ls4=<SHA256>
```

Retain the complete artifact tree and report exact commands/exits, tool and
backend identities/hashes, generated/distinct states, depth, elapsed time,
positive peak RSS, raw and normalized trace hashes, adapter event/harness
sequences, stable/differential comparisons, and TLAPS obligation totals.

## Evidence status

The previous fixed/mutant TLC counts are useful partial construction evidence,
but the canonical matrix stopped at stable text conversion before network,
pre-login, or TLAPS completion. The current execution environment cannot run
the pinned farm tooling, so no local green claim is made here.

## Still required beyond this matrix

- protocol-43 frozen-client/restart counterexamples
- exact OLD/NEW socketpair codec fixtures and mixed-version rows
- lifecycle authority, allocator, exact handoff, and F-session-quiescence models
- exact model-to-message/handler/test mapping
- mickg10 cluster fault, scale, throughput, latency, memory, descriptor, and
  scheduler-turn evidence
- final independent `mickgvirtu/implementer` review after mickg10 evidence is
  complete
