# TLC trace-to-harness adapter

`trace_to_harness.py` validates a retained TLC JSON counterexample before it is
used as an implementation-test scenario.

TLC's `-dumpTrace json` output is an array of state records. It does not include
the action labels or the lasso back-edge. The adapter therefore:

1. classifies adjacent state records with a declarative manifest;
2. requires the intended essential event subsequence;
3. checks final-state predicates for the named violated property;
4. rejects ambiguous, empty, malformed, zero-state, or incorrectly ordered
   traces;
5. reads the retained TLC text log when a lasso/back-edge is required; and
6. emits deterministic named harness steps for pipes, eventfds, or control
   barriers.

No Python expression from a manifest is evaluated.

## Authoritative workflow

Run TLC with an explicit config and retain both outputs:

```sh
java -jar "$TLA2TOOLS" -workers 1 \
  -config AssignmentFenceNetworkReleaseOnEnqueueMutant.cfg \
  -dumpTrace json traces/release-on-enqueue.json \
  AssignmentFenceNetwork \
  > traces/release-on-enqueue.tlc.log 2>&1
```

Copy the corresponding `*.manifest.template.json` to a
`*.manifest.json`, add paths relative to that manifest, and validate it:

```json
{
  "trace": "release-on-enqueue.json",
  "tlc_log": "release-on-enqueue.tlc.log"
}
```

```sh
python3 formal/trace_to_harness.py \
  --trace formal/traces/release-on-enqueue.json \
  --manifest formal/traces/release-on-enqueue.manifest.json \
  --tlc-log formal/traces/release-on-enqueue.tlc.log \
  --output formal/traces/release-on-enqueue.harness.json
```

Validate a retained corpus in one command:

```sh
python3 formal/trace_to_harness.py --check-all formal/traces
```

`--check-all` reads only files ending in `.manifest.json`; construction
templates end in `.manifest.template.json` and are intentionally ignored.

## Manifest conditions

An event contains `all`, `any`, and/or `none` arrays. A condition names a JSON
path using dot notation (`phase.a0`) or JSON Pointer (`/phase/a0`). Supported
checks include:

```text
from, to, eq
from_in, to_in
changed, unchanged
from_len, to_len, len_delta
delta, delta_gt, delta_ge, delta_lt, delta_le
exists
```

The default `event_mode` is `exclusive`: a transition matching two event
definitions is rejected rather than silently choosing one. Set
`require_all_transitions_classified` only when the manifest intentionally
covers every non-stuttering action in the trace.

## Liveness traces

The JSON state sequence does not encode TLC's lasso edge. A manifest with
`"require_lasso": true` must also provide the retained TLC text log. The
adapter accepts an explicit `Back to state N` or stuttering marker and emits it
in the harness JSON. Absence of the marker is a failure, not a passing finite
prefix.

## Self-test

```sh
python3 formal/trace_to_harness_test.py
```

The self-test covers a valid trace, missing/wrong-order events, ambiguity,
zero-state input, path typos, JSON Pointer/numeric deltas, lasso validation,
and relative-path corpus validation.

The adapter validates trace identity and sequence. It does not turn a parser
error, incomplete TLC run, simulation, depth limit, or unexpected property
failure into acceptance evidence. Tool version/hash, complete state-space
statistics, result, and process exit remain mandatory in the run manifest.
