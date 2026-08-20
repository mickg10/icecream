# Ordered-relationship shared-uplink results

Date: 2026-08-20

This report is the first complete gate for the corrected simulator topology: one submitting box,
one C cache authority/GUID, one physical C uplink, one ordered relationship/arena per F, and one
active cache dialogue per relationship. Different F relationships progress concurrently; all
C-to-F bits consume the same aggregate source capacity. Compilation of a committed TU overlaps
the next transfer on that relationship.

The five scenarios are generated explicitly in `scenarios/firefox-c1f{1,2,3,4,20}-200b1g.json`
and launched by `firefox-f-width-suite.json`. Each executes the corrected 2,498-TU Firefox trace
five times with zero inter-build gap, 200 compiler slots and 400 input-staging slots per F, a
1-Gbit/s aggregate C ceiling, and 40-Gbit/s route/F/directional-fabric controls.

## Identities and ordering gate

Every C admission receives contiguous `TU_SEQ`. Routing creates an order-preserving projection on
each `(C,F)` relationship and numbers it with contiguous `REL_SEQ`. The simulator checks both
domains after every complete run. Physical-ledger replay also checks the selected F, `TU_SEQ`, and
`REL_SEQ` before beginning a transaction.

For the two-F physical GRZ control, global admission `[0,1,2,3]` becomes:

```text
F0: TU_SEQ [0,2], REL_SEQ [0,1]
F1: TU_SEQ [1,3], REL_SEQ [0,1]
```

The ledger is grouped by relationship, so its global `TU_SEQ` values are deliberately sparse in
each group. The parser checks that their union is exactly the contiguous C admission domain; it
does not incorrectly require file order to be global order.

## Capacity-only bounds

Every build and generation now reports three optimistic lower bounds:

```text
C-to-F floor = max(bytes charged to each configured C/F/fabric resource / capacity)
compiler floor = max(longest TU, total compiler work / all compiler slots)
overlap floor = max(C-to-F floor, compiler floor)
```

These bounds omit TU release timing, propagation, dependency round trips, codec CPU, and queue
order. `capacity_floor_efficiency = overlap_floor / observed_duration`; it is a measure of how
close the simulated schedule came to an impossible-to-beat capacity bound, not a throughput
claim about a live cluster.

## Compile-only control

The compile-only adapter transfers no bytes. It isolates placement and compiler occupancy.

| Fs | compiler slots | per-build observed | per-build capacity floor | observed/floor | efficiency |
|---:|---:|---:|---:|---:|---:|
| 1 | 200 | 63.867 s | 51.353 s | 1.244x | 80.4% |
| 2 | 400 | 39.210 s | 25.677 s | 1.527x | 65.5% |
| 3 | 600 | 31.352 s | 23.277 s | 1.347x | 74.2% |
| 4 | 800 | 27.060 s | 23.277 s | 1.163x | 86.0% |
| 20 | 4,000 | 23.277 s | 23.277 s | 1.000x | 100.0% |

Three Fs provide enough nominal slots to reach the selected trace's 23.277-second longest-TU
floor, but simple round-robin placement does not balance compiler work well enough to realize it.
Twenty Fs reach that diagnostic floor because all 2,498 TUs fit simultaneously and the longest TU
alone determines completion. This is a routing/load-balance result, not an argument for teaching
twenty cold caches.

## Raw one-uplink control

Raw sends each complete `.ii` as one ordered relationship dialogue. It is a Protocol-50 transport
control, not a compression result. Every width sends exactly 15,240,876,398 bytes per build and
76,204,381,990 bytes across five builds. At 1 Gbit/s, the exact byte floor is 121.927011184 seconds
per build and 609.635055920 seconds across five builds.

| Fs | outgoing bytes, five builds | per-build observed | per-build byte floor | observed/floor | efficiency |
|---:|---:|---:|---:|---:|---:|
| 1 | 76,204,381,990 | 137.577 s | 121.927 s | 1.128x | 88.6% |
| 2 | 76,204,381,990 | 136.948 s | 121.927 s | 1.123x | 89.0% |
| 3 | 76,204,381,990 | 137.264 s | 121.927 s | 1.126x | 88.8% |
| 4 | 76,204,381,990 | 135.781 s | 121.927 s | 1.114x | 89.8% |
| 20 | 76,204,381,990 | 140.447 s | 121.927 s | 1.152x | 86.8% |

The key invariant holds: adding Fs never reduces the aggregate outgoing byte floor because every
route shares the one C source resource. F4 is the best round-robin raw control here; F20 is 3.4%
slower than F4 because extra relationships change dialogue and compiler-tail placement without
adding source bandwidth. The exact causes can be inspected in each run's assignments, events,
builds, generations, JSONL, and HTML report rather than inferred from one makespan.

## Physical codec controls

Both physical adapters consume byte-exact ledgers produced by their real codec binaries and run
inside the same scheduler/network engine.

| codec | relationships | raw bytes | C-to-F bytes | F-to-C bytes | makespan | verification |
|---|---:|---:|---:|---:|---:|---|
| P29 | 1 | 9,982,878 | 322,080 | 6,831 | 13.288534 ms | exact reconstruction and typed directional replay |
| GRZ | 2 | 9,982,878 | 624,141 | 0 | 11.871302 ms | full route decode, identical retry, exact prefix cuts |

P29 retains its intra-TU fork/join graph. On logical TU 1, Root serialization completes at
1.312200 ms and releases LINES; Root delivery at 1.562200 ms releases Need; Need delivery at
1.860296 ms makes Fill ready; the ongoing LINES extent yields at 2.131400 ms and Fill immediately
takes the next writer quantum. The relationship commits at 4.386456 ms while compilation
continues to 10.668094 ms.

The P29 materializer still supports only one F. Running independent P29 learners per F would
duplicate C-global learning and is not an acceptable multi-F result. Its next codec milestone is
one global admission/catalogue pass followed by ordered, independent per-F projections and one
multi-route physical ledger. GRZ already materializes route projections independently.

## Exact-report scalability

The canonical F20 raw artifact contains 70,225 active snapshots, four compressed idle gaps, and
1,663,080 exact events. The first recorder retained rich snapshots and events as Python objects;
the full run reached roughly 4,045,236 KiB RSS by direct observation. The final runner spools both
streams, merges them into canonical JSONL once, retains at most 2,000 snapshots for HTML, and
removes both spools after successful output.

| recorder | peak RSS | host elapsed | simulated result |
|---|---:|---:|---|
| original in-memory observation | ~4,045,236 KiB | ~18m 16s for F20 | 702.236763900 s |
| timeline spooled, events retained | 1,406,008 KiB | 18m 21.50s | identical |
| timeline and events spooled | 597,112 KiB | 19m 00.93s | identical |

The final change reduces peak RSS by about 85% from the original observation without reducing
snapshot frequency or event detail. Remaining peak memory is bounded primarily by the 2,000-row
HTML view and live simulation/result state. The final report is 64,376,935 bytes; canonical JSONL
has 70,231 rows including descriptor and final summary. An independent streaming audit verified
all event sequence numbers from 0 through 1,663,079, record counts, byte totals, capacity floor,
and final reconciliation.

## Retained evidence

The current host retains the main evidence under:

```text
/tanksmall/scratch/ictmp/issue16-results/ordered-relationship-width-precommit/
/tanksmall/scratch/ictmp/issue16-results/ordered-relationship-streaming-both-f20/
/tanksmall/scratch/ictmp/issue16-results/ordered-relationship-physical-precommit/
```

Binding physical-ledger SHA-256 values:

```text
P29  1de0306aff30e4e3661df9a10f2936a6038360ff5c33cb0e426156fb64dcef04
GRZ  a81a247670a40a03eb6f179b2c673a801bd407aa291be11508b8d715883364e2
```

## What this closes and what remains

Closed by this gate:

- one environment means one C authority/GUID/uplink;
- all F routes share the configured C egress;
- `TU_SEQ` and order-preserving `REL_SEQ` projections are explicit and checked;
- raw and physical adapters use one active dialogue per relationship;
- a prior committed TU may compile while the next relationship TU transfers;
- P29 Need/LINES/Fill overlap and bounded writer priority remain executable;
- exact long-run reports no longer retain every snapshot/event as Python objects;
- F widths 1/2/3/4/20 have executable zero-gap controls and capacity-relative reports.

Still required before ranking cache codecs at farm width:

- shared-C, multi-F P29 materialization from one global catalogue;
- richer component/object ledgers so Need, Fill, residency, route-state digests, and codec CPU are
  generated dynamically by the common engine rather than only replayed as flattened exact bytes;
- live cache-channel framing, compile-job reference bytes, CPU stages, local pipe delivery, cache
  eviction, reconnect, and complete fallback accounting;
- bounded-sticky-frontier routing against the width sweep, compared with round-robin;
- live multiprocess acceptance through the normal `make integration_tests` launcher.
