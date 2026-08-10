# Strict actor-local early-cancel source candidate

Base revision:

```text
2334d9ae341bc23e7532fe2b24454908e300fe40
```

This directory is deliberately source-only. It contains no workflow, issue
writer, generated status comment, downloader, or repository mutation helper.
It models only the strict READY-before-UseCS path. Pipelined pre-PREPARE claim
admission is out of scope and remains blocked on identity-bearing pending
state.

## Theorem boundary

The fixed model separates scheduler and fulfillment state and checks:

- F consumes received PREPARE without consulting scheduler-local phase;
- a READY received after cancellation is stale and cannot expose UseCS;
- F frees its worker slot when it consumes REVOKE;
- S retains its logical reservation until it consumes REVOKED;
- early cancellation settles under weak fairness only for continuously
  enabled FIFO-head consumption.

The model does not assume compiler completion, client progress, connection
repair, peer failure, or scheduler restart.

## Direct rows

```text
AssignmentFenceEarlyCancel.cfg
    fixed safety

AssignmentFenceEarlyCancelFair.cfg
    fixed product-controlled early-cancel progress

AssignmentFenceEarlyCancelWitness.cfg
    fixed reachability witness: cancel before F consumes PREPARE, then settle

AssignmentFenceEarlyCancelPhaseOracleMutantFair.cfg
    liveness counterexample: F reads S phase and refuses queued PREPARE

AssignmentFenceEarlyCancelStaleReadyMutant.cfg
    safety counterexample: delayed READY resurrects canceled assignment

AssignmentFenceEarlyCancelReleaseMutant.cfg
    safety counterexample: S releases before consuming REVOKED
```

## Required execution

Use one worker for every authoritative liveness run. Retain complete text logs
for all runs and native JSON traces where the tool supports them.

```sh
FORMAL_DIR=$PWD/formal
OUT=/absolute/path/outside/checkout/strict-early-cancel
mkdir -p "$OUT"

run_tlc() {
    jar=$1
    module=$2
    cfg=$3
    name=$4
    java -jar "$jar" -workers 1 -config "$cfg" "$module" \
        >"$OUT/$name.tlc.log" 2>&1
}

run_tlc /pinned/tla2tools-1.7.4.jar \
    AssignmentFenceEarlyCancel AssignmentFenceEarlyCancel.cfg fixed-174
run_tlc /pinned/tla2tools-1.8.0.jar \
    AssignmentFenceEarlyCancel AssignmentFenceEarlyCancel.cfg fixed-180

run_tlc /pinned/tla2tools-1.7.4.jar \
    AssignmentFenceEarlyCancelProgress AssignmentFenceEarlyCancelFair.cfg fair-174
run_tlc /pinned/tla2tools-1.8.0.jar \
    AssignmentFenceEarlyCancelProgress AssignmentFenceEarlyCancelFair.cfg fair-180
```

Run the four witness/mutant configs on both jars as well. Expected results:

```text
fixed safety                         exit 0, complete state space
fixed fair progress                  exit 0, complete state space
completion witness                   direct NoEarlyCancelCompleted violation
phase-oracle mutant                  temporal violation with lasso
stale-READY mutant                   direct NoUseCSAfterCancel violation
release-on-enqueue mutant            direct ReleaseAfterRevokedConsumption violation
```

Normalize every counterexample with the repository's retained
`normalize_tlc_trace.py`, then validate it with `trace_to_harness.py` and the
matching manifest template. Stable and differential runs must agree on the
required event subsequence and emitted harness steps.

## Acceptance report

Post the exact source SHA and hashes for every file, both jar hashes and version
banners, commands, exits, generated/distinct states, depth, elapsed time, peak
RSS, raw and normalized trace hashes, lasso marker for the temporal mutant,
and adapter result. A parse error, zero-state result, depth limit, simulation,
or unvalidated trace is RED.
