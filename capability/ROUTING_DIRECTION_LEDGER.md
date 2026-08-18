# Protocol-50 routing direction ledger

## Scope

Issue 16 scores the sum of physical bytes written from C to every selected F.
The accepted M5 ledger previously closed the duplex socket total but did not
separate writes from reads.  Frame type alone cannot recover direction because
`Ack` is emitted by both C and F.

This change is observation-only.  It does not alter frame construction,
compression selection, socket ordering, cache state, assignment, or compiler
pipe behavior.  It adds direction at the point where each physical frame is
sent or received and carries that observation through both M5 harnesses and
the acceptance launcher.

## Accounting model

For each relationship and frame type, `FrameLedger` now records:

```text
total bytes/count
sent bytes/count       C -> F in the coordinator ledger
received bytes/count   F -> C in the coordinator ledger
```

The physical size remains the four-byte protocol header plus payload.  Every
send updates total and sent; every receive updates total and received.

The batch and one-pass curves record `c_to_f` and `f_to_c` for every committed
TU.  Hello and final Done/Ack controls are attributed to the first/last
committed TU on their relationship.  Controls for an entirely idle
relationship are assigned by physical direction to the final global row.  A
restart or retry retains its already observed directional bytes and charges
them to the subsequently committed retry.  There is no undirected carry.

The following invariants are independently enforced in C++, in the TSV curve,
and again by `run_m5_acceptance.py`:

```text
for every frame type:
    total_bytes == c_to_f_bytes + f_to_c_bytes
    total_count == c_to_f_count + f_to_c_count

for every TU row:
    wire == c_to_f + f_to_c

for the complete run:
    actual_socket == sum(TU.wire)
    C_to_F == sum(TU.c_to_f)
    F_to_C == sum(TU.f_to_c)
    actual_socket == C_to_F + F_to_C
```

The binaries publish `C_TO_F_FRAME_LEDGER`, `F_TO_C_FRAME_LEDGER`,
`DIRECTION_LEDGER`, and `C_TO_F_CURVE_SUMMARY`.  Curves carry per-TU and
cumulative directional columns plus `direction_ok`.

## Executed gates

Retained root:

```text
/tanksmall/scratch/ictmp/issue16-routing-direction-5329465782
```

| Gate | Result |
|---|---:|
| warning-clean optimized batch and one-pass builds | PASS |
| focused header/transport/M2/M4/M5-state binaries | 5/5 PASS |
| complete smoke/evolution launcher | 45/45 exact |
| per-frame direction closure | PASS on every frame in every row |
| per-TU direction closure | PASS on every committed TU |
| rejection/rollback | 20 commits, 1 decode rejection, 2 prepared aborts; PASS |
| restart and late join | PASS |
| 1/4/8/16/32 relationship rows | PASS |
| batch/one-pass wire equivalence | PASS |
| retained hash manifest | 127/127 PASS |
| instrumented M5 state test | PASS |
| instrumented batch rejection/rollback | PASS |
| instrumented bounded one-pass removal/compaction | PASS |
| `git diff --check` | PASS |

The accepted pre-change artifact at
`/tanksmall/scratch/ictmp/issue16-snapshot-bounds-P5YEOI/smoke` was compared
against the new artifact for all 41 scenarios with identical inputs.  Every
historical frame/component byte and every non-latency field in the old curve
is byte-identical.  The four evolution rows were excluded only because this
focused run deliberately used four evolution TUs instead of the retained
run's larger fixture.

Selected direction totals from the optimized smoke run:

| row | TUs | duplex bytes | C -> F | F -> C | C -> F share |
|---|---:|---:|---:|---:|---:|
| cold standard | 20 | 1,195,308 | 1,144,758 | 50,550 | 95.771% |
| rejection/retry | 20 | 4,897,534 | 4,803,280 | 94,254 | 98.075% |
| worker restart | 20 | 3,653,698 | 3,574,754 | 78,944 | 97.839% |
| worker late join | 20 | 2,951,503 | 2,881,201 | 70,302 | 97.618% |
| 24-way host row | 50 | 14,045,682 | 13,826,558 | 219,124 | 98.440% |
| 32-F round robin | 20 | 10,009,285 | 9,853,465 | 155,820 | 98.443% |
| one-pass grow | 20 | 3,073,809 | 3,001,230 | 72,579 | 97.639% |
| one-pass bounded | 20 | 10,139,545 | 9,561,486 | 578,059 | 94.299% |

The global C-to-F frame ledger already separates Root, Fill, and C control
exactly for a complete run.  The next routing artifact still needs per-TU
Root/Fill fields so material-frontier curves can be replayed without inferring
frame ownership.

## Retained hashes

```text
afb4480e2ef180b72e8ef07dd02278fe43c80a3db380ed01b4caa5e388355840  m5-acceptance.tsv
9c1adededc863947a74c3167c6d1fa324725efee8b4497262630f04c43de3620  m5-acceptance.json
693cbf036f51a59a513e0312006e4eb7304537c2a17789975f610244770f82a9  SHA256SUMS
a00fa083a7b9ad798f1a033467b77b464121e5de70f8cbc1751e66e9539d3b09  logs/fill-reject-retry.log
0b50aff0829d94ed6ade7b9020684a9894cf8b7877f91535e33af5fc93e2aa69  logs/onepass-typed-bounded.log
c91b2d03153c8fe5b58ed589dea3538bbf2878a5e0c5fdae284da96cee237f93  asan-ubsan/main-retry.log
67608b71ac400d83e30a407ed6f2b89a30a15e7b76f61eccc5f6ab9d0d4aecc1  asan-ubsan/stream-bounded.log
02db067e5c7c8effa0701e7572865449b0981162f05d13456d5d65d73fab1244  asan-ubsan/state.log
```

## Immediate use

Routing rows must now use `C_to_F`, not duplex `actual_socket`, as their byte
score.  `F_to_C` remains part of elapsed-time and CPU reporting.  The exact
frame split also removes the 195-byte-per-F undirected carry in the earlier
instrumentation and makes fixed-width comparisons directly auditable.
