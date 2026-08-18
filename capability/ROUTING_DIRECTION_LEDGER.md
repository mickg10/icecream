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
TU. They also split those bytes into `c_root`, `c_fill`, `c_control`,
`f_need`, and `f_control`. Hello and final Done/Ack controls are attributed to the first/last
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
    c_to_f == c_root + c_fill + c_control
    f_to_c == f_need + f_control

for the complete run:
    actual_socket == sum(TU.wire)
    C_to_F == sum(TU.c_to_f)
    F_to_C == sum(TU.f_to_c)
    actual_socket == C_to_F + F_to_C
```

The binaries publish `C_TO_F_FRAME_LEDGER`, `F_TO_C_FRAME_LEDGER`,
`DIRECTION_LEDGER`, `ROUTING_LEDGER`, and `C_TO_F_CURVE_SUMMARY`. Curves carry
per-TU and cumulative directional columns plus `direction_ok` and
`category_ok`.

## Executed gates

Retained root:

```text
/tanksmall/scratch/ictmp/issue16-routing-categories-5329795604
```

The unchanged instrumented state gate remains retained at
`/tanksmall/scratch/ictmp/issue16-routing-direction-5329465782/asan-ubsan/state.log`.

| Gate | Result |
|---|---:|
| warning-clean optimized batch and one-pass builds | PASS |
| focused header/transport/M2/M4/M5-state binaries | 5/5 PASS |
| complete smoke/evolution launcher | 45/45 exact |
| per-frame direction closure | PASS on every frame in every row |
| per-TU direction closure | PASS on every committed TU |
| per-TU Root/Fill/control and Need/control closure | PASS on every committed TU |
| rejection/rollback | 20 commits, 1 decode rejection, 2 prepared aborts; PASS |
| restart and late join | PASS |
| 1/4/8/16/32 relationship rows | PASS |
| batch/one-pass wire equivalence | PASS |
| retained hash manifest | 127/127 PASS |
| instrumented M5 state test | PASS |
| instrumented batch rejection/rollback | PASS |
| instrumented bounded one-pass removal/compaction | PASS |
| `git diff --check` | PASS |

The first no-carry artifact at
`/tanksmall/scratch/ictmp/issue16-routing-direction-5329465782` was compared
against the category artifact for all 45 scenarios. Every pre-existing
socket, direction, frame, component, and non-latency curve value is
byte-identical. Generated-fixture corpus fingerprints differ only because the
fingerprint intentionally includes the different artifact-root paths; fixture
bytes and measured rows match.

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

The global and per-TU ledgers now separate Root, Fill, C control, Need, and F
control exactly. Material-frontier and mixed-warmth curves can therefore be
replayed from explicit row fields without inferring ownership from frame type
or duplex bytes.

## Retained hashes

```text
809e80100bd66dd913adb81c9cdb97979fabe3197e3fc4c664774dd6dec5130d  m5-acceptance.tsv
e25d0cd7193ac655f6cf90e63c476fc1ffc74967d677afc6d8fe794a5a88de15  m5-acceptance.json
25ba4febbcffdf5444ab563ad1c1ae06065bca81c46f3333ef46fc4f67d431b2  SHA256SUMS
f5d0f19c98be8d8c56230c83f18f97c455e5d18534a5f700691cc7324a919970  logs/fill-reject-retry.log
7823f1295b9349d05e19acee4fba0267cad6e35e360f38cf0b63fe357a747964  logs/onepass-typed-bounded.log
a4903d65acb87741ebfa8151d49b758b1122fca06460558039be6d8dd22546b2  asan-ubsan/main-retry.log
bda104100554f0e7450e228c59190cdcfca85e8bc7bdf5b83f883262e56e8623  asan-ubsan/stream-bounded.log
02db067e5c7c8effa0701e7572865449b0981162f05d13456d5d65d73fab1244  ../issue16-routing-direction-5329465782/asan-ubsan/state.log
```

## Immediate use

Routing rows must now use `C_to_F`, not duplex `actual_socket`, as their byte
score.  `F_to_C` remains part of elapsed-time and CPU reporting.  The exact
frame split also removes the 195-byte-per-F undirected carry in the earlier
instrumentation and makes fixed-width comparisons directly auditable.
