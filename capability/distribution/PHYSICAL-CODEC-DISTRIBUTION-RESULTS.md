# Physical-codec distribution results

Date: 2026-08-20

This report measures P29 and GRZ through the same distribution simulator.  The codec builders
produce exact physical transaction ledgers; the common event engine then applies worker capacity,
ordered per-F relationships, propagation delay, and all configured bandwidth ceilings.  There is
no codec-specific timing simulator.

The primary workload is five consecutive executions of the corrected 2,498-TU Firefox trace:
12,490 transactions and 76,204,381,990 logical input bytes.  Build zero starts cold.  Builds one
through four start immediately after the preceding build and retain codec state.  Every result in
this report uses one submitting environment, one persistent C authority/GUID, one shared C uplink,
and one independent ordered cache arena for each selected F.

The retained C1F1 ledgers were first built from the same five-build payload/order with ten-minute
inter-build gaps.  The zero-gap suite used compatible-ledger replay: it rehashed every payload and
required identical destination, `TU_SEQ`, and `REL_SEQ` before using those byte extents with the
new timing.  Thus the measured codec bytes are reused, but the reported clock comes entirely from
the zero-gap scenarios named below.

## What the timing does and does not include

The simulated clock includes:

- the measured Firefox compile-duration model attached to each TU;
- assignment, per-F staging and compiler-slot limits;
- byte-exact codec phase extents and their dependency graph;
- one active transaction dialogue per `(C,F)` relationship;
- concurrent progress on distinct F relationships;
- route, F, fabric, and shared-C bandwidth ceilings;
- frame quantum scheduling and one-way propagation delay.

The simulated clock does not yet schedule codec construction CPU, decoder CPU, cache lookup CPU,
or compiler-pipe writing CPU.  It also does not yet model the compiled-object/result return leg.
Host-side ledger-build wall time is reported separately and must not be added to, or mistaken for,
the simulated cluster makespan.

The byte tables are exact for the adapters' current typed streams, but are not yet every byte of
the eventual live cache protocol.  The live compile-job reference and outer cache-channel envelope
do not exist in the replayed implementation and therefore are not charged.  The F-to-C columns
below contain codec Need/Ack traffic only, not compiled objects.  These omissions do not change the
source-codec comparison, but they prevent treating the totals as a complete deployed-job network
bill.

## Capacity floor

For every build the simulator computes:

```text
C-to-F byte floor = max(bytes charged to each constrained resource / its capacity)
compiler floor     = max(longest TU, compiler work / available compiler slots)
overlap floor      = max(C-to-F byte floor, compiler floor)
```

The five-build compiler floor in all four high-slot C1F1 scenarios is **116.383 seconds**.  It is
an optimistic capacity bound: it excludes propagation, transaction dependencies, queue order,
codec CPU, and decoder/compiler-pipe work.  Distance from this floor therefore exposes the
simulator's communication and scheduling overhead instead of hiding it inside a throughput ratio.

## Required C1F1 capacity/bandwidth matrix

The 10,000-slot and 1,000,000-slot cases are intentionally identical when bandwidth is equal:
each build contains only 2,498 TUs, so both configurations can stage and compile the entire build
at once.

| scenario | codec | C→F total | cold | four warm | summed input-ready | result | floor | excess | floor efficiency |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `C1F1_1000000B_BW1G` | P29 | 38.179 MB | 37.157 MB | 1.022 MB | 11.329 s | 116.581 s | 116.383 s | 197.555 ms | 99.831% |
| `C1F1_1000000B_BW1G` | GRZ | 300.723 MB | 61.708 MB | 239.014 MB | 5.528 s | 116.522 s | 116.383 s | 138.919 ms | 99.881% |
| `C1F1_1000000B_BW10000G` | P29 | 38.179 MB | 37.157 MB | 1.022 MB | 11.238 s | 116.553 s | 116.383 s | 169.750 ms | 99.854% |
| `C1F1_1000000B_BW10000G` | GRZ | 300.723 MB | 61.708 MB | 239.014 MB | 3.123 s | 116.431 s | 116.383 s | 47.509 ms | 99.959% |
| `C1F1_10000B_BW1G` | P29 | 38.179 MB | 37.157 MB | 1.022 MB | 11.329 s | 116.581 s | 116.383 s | 197.555 ms | 99.831% |
| `C1F1_10000B_BW1G` | GRZ | 300.723 MB | 61.708 MB | 239.014 MB | 5.528 s | 116.522 s | 116.383 s | 138.919 ms | 99.881% |
| `C1F1_10000B_BW10G` | P29 | 38.179 MB | 37.157 MB | 1.022 MB | 11.242 s | 116.554 s | 116.383 s | 170.551 ms | 99.854% |
| `C1F1_10000B_BW10G` | GRZ | 300.723 MB | 61.708 MB | 239.014 MB | 3.363 s | 116.440 s | 116.383 s | 56.642 ms | 99.951% |

The milestone clocks below are sums of the five active build intervals.  They are alternative
views of each run, not sequential components to add together.

| bandwidth | codec | pure C→F byte floor | all inputs ready | all transactions committed | all compiles complete |
|---:|---|---:|---:|---:|---:|
| 1 Gbit/s | P29 | 0.305434 s | 11.329438 s | 11.330690 s | 116.580555 s |
| 1 Gbit/s | GRZ | 2.405780 s | 5.528280 s | 5.528280 s | 116.521919 s |
| 10 Gbit/s | P29 | 0.030543 s | 11.241843 s | 11.243093 s | 116.553551 s |
| 10 Gbit/s | GRZ | 0.240578 s | 3.363078 s | 3.363078 s | 116.439642 s |
| 10,000 Gbit/s | P29 | 0.000031 s | 11.237754 s | 11.239004 s | 116.552750 s |
| 10,000 Gbit/s | GRZ | 0.000241 s | 3.122741 s | 3.122741 s | 116.430509 s |

GRZ transmits **7.88× as many C-to-F bytes** as P29 over the five builds.  GRZ nevertheless
finishes 58.636 ms earlier at 1 Gbit/s and 122.241 ms earlier at effectively infinite bandwidth.
The reason is visible in the transaction graphs: GRZ has one current-TU frame and no reply,
whereas P29 retains the exact Root/LINES/Need/Fill/close/Ack dialogue.  At these very high compiler
capacities, the extra dependency depth is more important than either codec's already-small byte
serialization time.

The 10,000-Gbit/s points are useful causal controls.  With byte serialization essentially removed,
P29 remains 169.750 ms above the compiler-only floor and GRZ remains 47.509 ms above it.  Those
residuals include modeled propagation, per-route ordering, dialogue dependencies, and queue order;
they do not include codec CPU.

## Physical byte breakdown

### P29

| generation | Root | LINES | Fill | close | C→F total | Need | Ack | F→C total |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| cold | 1,416,399 | 35,007,821 | 535,752 | 197,342 | 37,157,314 | 491,735 | 134,892 | 626,627 |
| warm 1 | 104,801 | 0 | 0 | 197,342 | 302,143 | 39,856 | 134,892 | 174,748 |
| warm 2 | 42,589 | 0 | 0 | 197,342 | 239,931 | 0 | 134,892 | 134,892 |
| warm 3 | 42,589 | 0 | 0 | 197,342 | 239,931 | 0 | 134,892 | 134,892 |
| warm 4 | 42,589 | 0 | 0 | 197,342 | 239,931 | 0 | 134,892 | 134,892 |

The cold build teaches almost all material.  By warm build two, each TU is almost entirely a
small Root recipe plus transaction close/Ack framing.  Across all five builds P29 sends
38,179,250 C-to-F bytes and 1,206,051 F-to-C bytes, a logical-input/C-to-F ratio of 1,995.96×.

### GRZ

| generation | current-TU frames |
|---|---:|
| cold | 61,708,295 bytes |
| warm 1 | 59,641,689 bytes |
| warm 2 | 59,741,656 bytes |
| warm 3 | 59,772,362 bytes |
| warm 4 | 59,858,538 bytes |

GRZ sends 300,722,540 C-to-F bytes and no codec reply bytes.  Its 1-GiB rolling history does not
span the 15.24-GB Firefox build, so repeated builds remain close to 60 MB rather than collapsing
like P29's persistent object catalogue.

## Raw and compile-only controls

| scenario | adapter | C→F total | result | capacity floor | excess | floor efficiency |
|---|---|---:|---:|---:|---:|---:|
| `C1F1_1000000B_BW1G` | compile-only | 0 | 116.383 s | 116.383 s | 0 | 100.000% |
| `C1F1_1000000B_BW1G` | raw | 76,204.382 MB | 687.886 s | 609.635 s | 78.251 s | 88.624% |
| `C1F1_1000000B_BW10000G` | raw | 76,204.382 MB | 116.432 s | 116.383 s | 49.442 ms | 99.958% |
| `C1F1_10000B_BW1G` | raw | 76,204.382 MB | 687.886 s | 609.635 s | 78.251 s | 88.624% |
| `C1F1_10000B_BW10G` | raw | 76,204.382 MB | 155.948 s | 116.383 s | 39.565 s | 74.629% |

The raw controls demonstrate why byte-floor and observed completion must both be retained.  At
10 Gbit/s, aggregate raw serialization is below the compiler floor, but one ordered route feeds
the compiler gradually and still finishes 39.565 seconds above the ideal-overlap bound.

## Common F-width controls

The corrected width suite runs the same five-build workload at 1, 2, 3, 4, and 20 Fs.  Each F has
200 compiler slots and 400 input-staging slots.  All relationships share the same one-Gbit/s C
uplink, so this sweep changes compiler capacity and assignment width without changing source
capacity.

| Fs | total compiler slots | compile-only result | compiler floor | compile efficiency | raw result | raw byte floor | raw efficiency |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 200 | 319.336 s | 256.766 s | 80.406% | 687.886 s | 609.635 s | 88.624% |
| 2 | 400 | 196.048 s | 128.383 s | 65.486% | 684.740 s | 609.635 s | 89.032% |
| 3 | 600 | 156.758 s | 116.383 s | 74.244% | 686.320 s | 609.635 s | 88.827% |
| 4 | 800 | 135.300 s | 116.383 s | 86.019% | 678.905 s | 609.635 s | 89.797% |
| 20 | 4,000 | 116.383 s | 116.383 s | 100.000% | 702.237 s | 609.635 s | 86.813% |

The compiler-only result exposes the available parallelism; twenty Fs can start all 2,498 TUs
and reach the longest-TU floor.  Raw never benefits from destination width because all
76,204,381,990 bytes still cross the same C resource.  F4 happens to be the best raw round-robin
schedule here, while F20 is 3.4% slower than F4 from relationship ordering and compiler-tail
placement.

## Twenty independent F arenas

`C1F20_200B1G` gives each of twenty Fs 200 compiler slots and 400 input-staging slots.  Route,
per-F, and fabric ceilings are 10 Gbit/s; every route shares the C authority's one-Gbit/s uplink.
The scheduler uses round-robin assignment.  Since one build has 2,498 TUs and
`2498 mod 20 = 18`, the route offset moves between builds: this is deliberately a non-sticky
control, not a claim that the same TU returns to the same F.

Both multi-F ledgers passed their complete independent reconstruction gates.

| build | temperature | P29 C→F | GRZ C→F |
|---:|---|---:|---:|
| 0 | cold | 148.281 MB | 122.718 MB |
| 1 | warm | 48.301 MB | 76.174 MB |
| 2 | warm | 40.436 MB | 76.152 MB |
| 3 | warm | 37.037 MB | 77.502 MB |
| 4 | warm | 35.261 MB | 81.898 MB |
| **total** | | **309.316 MB** | **434.444 MB** |

P29's 309.316-MB C→F total consists of 292.373 MB LINES, 11.729 MB Root, 4.227 MB Fill,
and 0.987 MB close frames.  The reverse direction is 4.829 MB Need plus 0.674 MB Ack.
Thus duplicated or newly encountered line material—not Root recipe syntax—is the dominant
twenty-arena cost.

P29's C→F total grows by 8.10× from one F to twenty; GRZ grows by 1.44×.  P29's byte advantage
over GRZ therefore narrows from 7.88× in the single-arena experiment to **1.40×** here.  P29 is
still learning—the per-build teaching cost falls from 48.301 MB to 35.261 MB—but the moving
round-robin projection exposes each independent arena to material it has not retained before.
GRZ's smaller growth is consistent with its per-route streams making the one-GiB rolling history
more useful than in the single-route run; the current ledger proves the bytes, not that causal
attribution by itself.

The pure shared-uplink byte floors are 2.474525 seconds for P29 and 3.475548 seconds for GRZ.
The compiler-only control improves from 319.335624 seconds at one 200-slot F to 116.383 seconds at
twenty Fs, a 202.952624-second capacity/placement gain before communication.

| codec | C→F floor | all inputs ready | transactions committed | result | compiler floor | excess | floor efficiency |
|---|---:|---:|---:|---:|---:|---:|---:|
| P29 | 2.474525 s | 2.644808 s | 2.646060 s | **117.011001 s** | 116.383 s | 0.628001 s | 99.463% |
| GRZ | 3.475548 s | 3.488030 s | 3.488030 s | **117.046612 s** | 116.383 s | 0.663612 s | 99.433% |

P29 is only **35.611 ms faster**.  GRZ wins the cold generation by 51.645 ms; P29 wins the four
warm generations by 87.256 ms, producing that small net difference.  Twenty independent routes
shorten P29's serialized dialogue depth enough that its lower byte count narrowly offsets the
Root/Need/Fill/Ack chain.  Compiler tails dominate both results, so this is an effective makespan
tie despite the remaining 1.40× byte difference.

Relative to the one-F/200-slot compiler-only control, P29 realizes 202.324623 seconds (99.691%)
and GRZ realizes 202.289012 seconds (99.673%) of the 202.952624-second parallelism gain available
at F20.  This result supports opening enough arenas to meet the compile envelope, then choosing
among placements by incremental teaching bytes; it does not support opening every available F
without measuring the compiler benefit.

## Exact codec and harness observations

The retained P29 ledger contains all 12,490 transactions and passes exact encode plus an
independent typed-stream replay.  Its immutable input preparation is shared across all route
projections.  For the C1F1 build, the complete builder gate took 24 minutes 41.68 seconds and
peaked at 18,006,724 KiB RSS.  That gate includes two complete preparations/materializations and
is a capability harness measurement, not a production encode latency.

The retained GRZ ledger passes complete reconstruction, an identical retry, and exact build-prefix
decodes.  The first encode processed 76,204,381,990 logical bytes in 112.249 seconds
(647.4 MiB/s) and the identical retry took 125.764 seconds (577.9 MiB/s).  The complete harness,
including all decode and prefix gates, took 19 minutes 59.15 seconds and peaked at 2,425,628 KiB.

The exact twenty-route gates measured as follows.  Their work includes encode, independent replay
or retry, reconstruction, prefix checks, hashing, and ledger construction; it remains separate
from the simulated clock.

| codec | harness elapsed | user CPU | system CPU | peak RSS |
|---|---:|---:|---:|---:|
| P29 | 31m 55.10s | 1,818.79 s | 88.71 s | 18,583,632 KiB |
| GRZ | 26m 14.39s | 834.90 s | 717.74 s | 2,161,884 KiB |

The C1F20 simulator/reporter itself took 999.229 seconds for P29 and 209.754 seconds for GRZ;
total suite-runner times were 1,051.116 and 211.852 seconds respectively.  Those are host-side
research-tool costs, not modeled cluster time.  P29 produces many more dependency transitions
and a much larger exact event stream.  The retained ten-millisecond timeline is intentional, but
this 4.76× simulator-runtime difference is a tooling optimization opportunity before much larger
sweeps.

## Retained evidence

```text
C1F1 suite:
  /tanksmall/scratch/ictmp/issue16-results/c1f1-capacity-bandwidth-physical-1d06f95/
  suite report SHA-256 779fd1b7face34ce44c97197aae95a84c0118f20ecedc95768854687d5bb3129

P29 ledger:
  /tanksmall/scratch/ictmp/issue16-results/p29-firefox-c1f1-canonical/ledger.jsonl
  SHA-256 85d0b9400ee64500771558db16eb100c5235225e7ad5d7bd81721787a2bf90d2

GRZ ledger:
  /tanksmall/scratch/ictmp/issue16-results/grz-firefox-c1f1-canonical/ledger.jsonl
  SHA-256 d94a29aa3632ed27455e0612cd0587fb1699e327015724739ad5e342c1b5c6ae

Raw/compile-only suite:
  /tanksmall/scratch/ictmp/issue16-results/c1f1-capacity-bandwidth-diagnostic-4e17e25/

F-width raw/compile-only suite:
  /tanksmall/scratch/ictmp/issue16-results/firefox-f-width-suite-1d06f95/
  suite report SHA-256 0cf7216580fe77f500efe5ac8e050a29886a94fdf96fae9d12eee7380d8526a4

P29 C1F20 ledger:
  /tanksmall/scratch/ictmp/issue16-results/p29-firefox-c1f20-canonical/ledger.jsonl
  SHA-256 7207dc46ef15c61b7688b42a3abace7c8f09e78caf04ba161d6bc60dfb993033

GRZ C1F20 ledger:
  /tanksmall/scratch/ictmp/issue16-results/grz-firefox-c1f20-canonical/ledger.jsonl
  SHA-256 52e133f3c4d9d109ff50f29367cf4d93b5448028cc39c24c1b40ff2f4670ab38

C1F20 physical suite:
  /tanksmall/scratch/ictmp/issue16-results/c1f20-physical-1d06f95/
  suite report SHA-256 d93c32396de456d9e908a4de16eddb9f54d09dc8d2b7ac826ba71e1345fb00a8
```

Every scenario directory retains its resolved input, assignment trace, exact event ledger,
10-ms state stream, generation/build tables, summary, and self-contained HTML timeline.  The
suite root adds the cross-scenario matrix, physical phase table, runner timing table, and compact
HTML comparison page.
