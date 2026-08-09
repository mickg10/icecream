# TLC trace-to-harness adapter

`trace_to_harness.py` validates a canonical JSON state sequence before an
expected TLC counterexample is used as an implementation-test scenario.

TLC toolchains do not emit one common trace shape:

- TLA+ Tools 1.7.4 has no `-dumpTrace` option; its complete counterexample is
  retained in the ordinary text log.
- Newer TLC emits a graph-shaped JSON document whose states are under
  `counterexample.state` as `[ordinal, state]` pairs.

`normalize_tlc_trace.py` converts either form into the canonical JSON array
consumed by the adapter. Normalization is syntax-only: it does not evaluate
TLA+ expressions or infer actions.

The adapter then:

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

### Toolchain with `-dumpTrace json`

Retain TLC's raw graph-shaped JSON and text log:

```sh
java -jar "$TLA2TOOLS" -workers 1 \
  -config AssignmentFenceNetworkReleaseOnEnqueueMutant.cfg \
  -dumpTrace json traces/release-on-enqueue.raw.json \
  AssignmentFenceNetwork \
  > traces/release-on-enqueue.tlc.log 2>&1

python3 formal/normalize_tlc_trace.py \
  --input traces/release-on-enqueue.raw.json \
  --output traces/release-on-enqueue.json \
  --metadata traces/release-on-enqueue.normalization.json
```

### TLA+ Tools 1.7.4

Run TLC without the unsupported option, retain the text log, and normalize that
log directly:

```sh
java -jar "$TLA2TOOLS_174" -workers 1 \
  -config AssignmentFenceNetworkReleaseOnEnqueueMutant.cfg \
  AssignmentFenceNetwork \
  > traces/release-on-enqueue-174.tlc.log 2>&1

python3 formal/normalize_tlc_trace.py \
  --input traces/release-on-enqueue-174.tlc.log \
  --output traces/release-on-enqueue-174.json \
  --metadata traces/release-on-enqueue-174.normalization.json
```

The canonical acceptance runner performs these steps automatically after
preflighting each pinned toolchain's supported options.

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

The canonical JSON state sequence does not encode TLC's lasso edge. A manifest
with `"require_lasso": true` must also provide the retained TLC text log. The
adapter accepts an explicit `Back to state N` or stuttering marker and emits it
in the harness JSON. Absence of the marker is a failure, not a passing finite
prefix.

## Self-tests

The direct commands below leave the checkout clean; bytecode artifacts are
ignored and the acceptance runner also sets `PYTHONDONTWRITEBYTECODE=1`.

```sh
python3 formal/trace_to_harness_test.py
python3 formal/tlc_text_trace_test.py
python3 formal/normalize_tlc_trace_test.py
```

The normalizer test contains the real modern TLC wrapper shape and a stable
text trace containing finite functions, records, and sequences. Adapter tests
cover a valid trace, missing/wrong-order events, ambiguity, zero-state input,
path typos, JSON Pointer/numeric deltas, lasso validation, and relative-path
corpus validation.

Normalization and adapter validation do not turn a parser error, incomplete
TLC run, simulation, depth limit, or unexpected property failure into
acceptance evidence. Tool version/hash, state counts, result, process exit, and
positive peak-RSS evidence remain mandatory in the run manifest. A passing
state-space exploration must leave no states queued; an expected
counterexample instead requires the directly named violation and a
manifest-valid nonempty trace.
