# Assignment-fence targeted scale cutoffs

This branch is a scale campaign layered on the exact accepted formal source
`aaeea937a31c444ad53dba348007d205c44c0d27`. It does not change the accepted
transition relation. It adds only mapping operators and three configurations.

It is **not** a raw 4F/8C acceptance claim. The useful additional finite
coverage comes from shared capacity and queue contention:

| Row | Assignments | Workers | New witness support |
|---|---:|---:|---|
| `ScaleCapacity` | 3 | 2 (one idle) | capacity-two off-by-one (`k+1`) |
| `ScaleMixed` | 4 | 2 | two capacity-two workers plus mixed policies |
| `ScaleLiveness` | 3 | 2 | shared-worker contention under existing live-link fairness |

No row uses `SYMMETRY`, `VIEW`, a state constraint, or an action constraint.

## Safety cutoff argument

The interaction hypergraph has assignment and worker vertices. Ordinary actions
touch one assignment and at most its worker; FIFO consumption additionally
depends on a predecessor frame. The current invariant family has the following
minimal bad-state supports:

- pointwise lifecycle, causal, release, and terminal facts: one assignment;
- identity uniqueness and mixed-policy interference: two assignments;
- FIFO bypass: two queue entries;
- cross-worker contamination: two workers;
- worker-capacity overflow at finite capacity `k`: `k + 1` assignments on one
  worker.

The session-loss action updates all live assignments, but it releases rather
than creates capacity and its remaining safety facts are pointwise. A violating
state therefore retains a violating projection onto the causal predecessor
closure of its witness assignments. Removing unrelated queue entries preserves
the relative order of witness entries as a subsequence.

For the current finite invariant family, the meaningful cutoff is therefore
`max(2, k + 1)` assignments and two workers. This campaign checks `k = 2`.
The topology-general TLAPS theorem remains the authority for the unbounded
logical ownership and identity core.

Liveness is not claimed to follow automatically from this projection because
projection can change fairness. `ScaleLiveness` is checked directly without
symmetry.

## Run order

Use the exact pinned TLC 1.8.0-pre jar first. Substitute only the actual pinned
absolute jar path below and record its SHA-256.

```sh
set -euo pipefail
repo=/path/to/clean/icecream
formal="$repo/formal"
jar=/absolute/path/to/pinned/tla2tools-1.8.0.jar
out=/absolute/path/outside-the-checkout/assignment-fence-scale
mkdir -p "$out"
sha256sum "$jar" | tee "$out/tlc-1.8.0.sha256"

timeout 1800 /usr/bin/time -v -o "$out/S1.time.txt" \
  java -jar "$jar" -workers 1 \
  -metadir "$out/S1.states" \
  -config AssignmentFenceNetworkScaleCapacity.cfg \
  AssignmentFenceNetworkScale \
  >"$out/S1.tlc.log" 2>&1

timeout 1800 /usr/bin/time -v -o "$out/S2.time.txt" \
  java -jar "$jar" -workers 1 \
  -metadir "$out/S2.states" \
  -config AssignmentFenceNetworkScaleMixed.cfg \
  AssignmentFenceNetworkScale \
  >"$out/S2.tlc.log" 2>&1

timeout 1800 /usr/bin/time -v -o "$out/L1.time.txt" \
  java -jar "$jar" -workers 1 \
  -metadir "$out/L1.states" \
  -config AssignmentFenceNetworkScaleLiveness.cfg \
  AssignmentFenceNetworkScale \
  >"$out/L1.tlc.log" 2>&1
```

Run S1 first. Start S2 only after S1's result and resource figures are retained.
L1 may run independently after S1 parses successfully.

Initial per-row resource budget:

```text
wall:        30 minutes
peak RSS:    16 GiB
state files: 50 GiB
```

A budget stop is `RESOURCE_BOUND`, not a model failure. Retain the checkpoint,
state counts reached, depth, RSS, wall time, and disk use.

If a row completes with fewer than ten million distinct states and under 8 GiB
RSS, replay the same row with the exact pinned TLC 1.7.4 jar and compare:

- generated and distinct states;
- complete-search depth;
- zero states left on queue;
- named invariant/property result;
- absence of parser/runtime failures.

## Acceptance parsing

A fixed safety/liveness row is green only when its log contains all of:

```text
No error has been found
nonzero states generated
nonzero distinct states found
0 states left on queue
```

and contains none of:

```text
Invariant ... is violated
Temporal properties were violated
Parsing or semantic analysis failed
SANY Error
Exception in thread
TLC threw an unexpected exception
```

The liveness row must run exactly `FencedLivenessSpec` and
`NoPermanentQueuedFrame`; do not add fairness for compiler completion,
connection repair, or environment progress.

## Deliverable

Post exact branch SHA, command lines, jar hashes, logs and their SHA-256s,
state/depth figures, peak RSS, wall time, disk use, and the clean-checkout result
to issue #4. Do not merge this campaign into the canonical proof branch until
its configurations have executed and been independently reviewed.
