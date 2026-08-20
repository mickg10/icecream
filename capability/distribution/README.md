# Distribution experiments

One scenario document owns the workload, C/F topology, timing, network and scheduler.
GRZ and P29 are codec adapters beneath that common experiment; they do not own separate
scheduler simulations.

[`ICECREAM-REAL-TRANSFER-ARCHITECTURE.md`](ICECREAM-REAL-TRANSFER-ARCHITECTURE.md)
maps the simulator onto the actual C client, C/F daemons, persistent stores, per-job TCP
connections, A/Need/Fill dialogue, and compiler stdin pipe.

[`SHARED-UPLINK-WIDTH-RESULTS.md`](SHARED-UPLINK-WIDTH-RESULTS.md) records the executable
1/2/3/4/20-F width gate, capacity-only lower bounds, raw and compile-only matrices, exact P29/GRZ
controls, and bounded-memory report measurements.

The initial scenario is `firefox-1c-20f.json`: one C, one corrected Firefox build,
twenty one-slot Fs, 1 Gbit/s per direction and a 10 Gbit/s shared fabric.  All TUs are
released at time zero for the first comparison.  A measured C-preprocessor release trace
is a later input using the same scenario schema.

`build_release: after-previous` is an explicit build barrier: rebuild `b+1` cannot release
until every TU in build `b` has completed.  `environment` identifies the C, while
`corpus_root` resolves the payload names in the compile trace.  The latter can be overridden
when the same scenario is replayed on another machine.

## Correcting the Firefox job set

Firefox's CompileDB describes constituent source files even when the compiler operand is
one shared `Unified_cpp_*.cpp`.  The old corpus18 manifest retained those aliases as if
they were separate compiler jobs.  `build_firefox_workload.py` groups by the real command
identity and keeps one captured `.ii` per invocation.

```bash
python3 capability/distribution/build_firefox_workload.py \
  --compile-commands /tanksmall/scratch/ictmp/src3/gecko-dev/obj-cc/compile_commands.json \
  --manifest /tanksmall/scratch/ictmp/corpus18/manifest.txt \
  --corpus-root /tanksmall/scratch/ictmp/corpus18 \
  --out-trace capability/distribution/firefox-corrected.compile-trace.tsv \
  --out-summary capability/distribution/firefox-corrected.summary.json
```

`firefox_compile_samples.tsv` retains the measurements used by the provisional compile
model.  The model linearly interpolates wall time by `.ii` size.  Three upper observations
ended at a compile error in the configure/export-only Firefox tree; they are deliberately
labelled `early-error`.  This is the selected distribution for the current simulation,
not a claim that a complete successful Firefox build timing census has already run.

The common runner must retain these outputs for every codec/policy run:

* resolved scenario;
* selected job trace;
* `(C, TU, F)` assignment trace;
* chronological message/compile event ledger;
* C-to-F and F-to-C byte totals kept separate;
* makespan and per-F utilization;
* exact reconstruction and prefix checks from the selected codec adapter.

`run_scenario.py` is the single event/scheduling core.  Its `compile-only` and `raw`
adapters are diagnostic references, not compression results.  A physical codec adapter
supplies a transaction graph of C-to-F/F-to-C extents for each dispatched TU; it does not get
its own scheduler.  Graph edges distinguish `sent` from `delivered`, and input readiness and
transaction commit are independent joins.  The core has separate F input-staging and compiler
pools, shares all configured bandwidth resources between active flows, and schedules bounded
writer quanta by frame priority.  A priority class may overtake an older queued extent only
`max_priority_burst_quanta` times (eight by default) before that oldest extent gets a turn.
Omitting `input_staging_slots` retains the historical
assign-and-reserve-compiler behavior for old scenarios.

```bash
python3 capability/distribution/run_scenario.py \
  capability/distribution/firefox-1c-20f.json \
  --codec compile-only \
  --out /tmp/firefox-compile-only
```

Every run now also writes `experiment.jsonl` and a self-contained `report.html`.  The first
JSONL row is the complete resolved experiment descriptor.  While work is active, subsequent
rows are 10-ms snapshots of scheduler, C, F, codec, and network state.  An idle interval is
represented by one explicit gap row rather than thousands of empty samples.  The final row
contains the complete result summary.  Exact events are attached to the first snapshot or gap
whose boundary contains them, so no transition is lost when the view is sampled.

`experiment.jsonl` always retains the complete 10-ms stream.  To keep a large self-contained
HTML file responsive, `report.html` embeds at most 2,000 evenly spaced active snapshots plus
every idle-gap row, and labels the retained/source counts in the page.  The JSONL remains the
canonical source for intervals omitted from the browser overview.  Foreground runners spool both
timeline rows and exact events during simulation, merge them into the canonical JSONL in one
streaming pass, retain only the bounded browser view in memory, and remove both temporary spools
after all outputs are complete.  Thus recorder RSS does not grow with snapshot or event count.

The compact primary scenario is `firefox-c1f20-200b1g.json`: one C, twenty Fs, 200 compile
slots per F, five zero-gap builds, a one-Gbit/s shared C ceiling, and ten-Gbit/s route, F, and
fabric ceilings.  Its report label is `C1F20_200B1G`.

The scenario's `environment` is deliberately one submitting box, one C authority/GUID, and one
physical C egress.  Multiple environments are independent boxes with independent GUIDs and
uplinks.  The first deployment does not model delegated egress or several producer boxes sharing
one authority.  `per_environment_bits_per_second` is therefore the one aggregate uplink ceiling
shared by all of that C's F relationships.

Each dispatched TU receives a contiguous `TU_SEQ` in its C admission order.  Routing projects
that order onto each destination F and assigns a contiguous `REL_SEQ`; every assignment and event
records both.  A relationship with TU sequence `[0, 3, 7]` therefore carries REL sequence
`[0, 1, 2]`.  Physical-ledger replay rejects a changed TU identity, destination, or projection.
Whole-TU raw, P29, and GRZ use one active dialogue per `(C,F)` relationship while different Fs
advance concurrently.  Once a TU's input commits, its compiler can continue while the next TU on
that relationship transfers.

`firefox-f-width-suite.json` is the corrected one-C routing-width gate.  It runs five zero-gap
builds at F widths 1, 2, 3, 4, and 20, always with 200 compiler slots and 400 input-staging slots
per F.  Every route, F ingress, and directional fabric is capped at 40 Gbit/s while the one C
authority remains capped at an aggregate 1 Gbit/s in each direction.  Adding Fs therefore adds
compiler capacity and independent cache arenas, never source bandwidth:

```bash
python3 capability/distribution/run_suite.py \
  capability/distribution/firefox-f-width-suite.json \
  --codec compile-only --codec raw --require-payload \
  --out /tmp/firefox-f-width
```

Each build and generation row reports input-ready elapsed time, compile elapsed time, the
capacity-only C-to-F floor, compiler floor, ideal overlap floor, actual/floor ratio, and capacity
efficiency.  These are intentionally optimistic lower bounds: they omit release timing,
propagation, codec CPU, dependency round trips, and queue order.  The gap between the bound and
the run is therefore visible rather than folded into a single makespan.

## Physical codec ledgers

The simulator accepts physical codec bytes only through
`icecream-physical-codec-ledger-v1`.  It rejects an incomplete reconstruction result, a
scenario hash mismatch, missing or repeated TUs, route-order drift, assignment drift, and byte
totals that do not tile the physical streams.  A physical route currently has one committed
dialogue at a time; distinct `(C,F)` routes still advance concurrently.

The current byte score covers the adapter's measured C-to-F phases.  It does not yet claim
bytes for the live compile-job reference or for the outer cache-channel envelope, because those
messages do not exist in the implementation being replayed.  The reverse adapter phases are
retained separately.  The live cache-channel stage must add both omitted C-to-F categories before
the field can be interpreted as every byte written by C.

Build and replay a shared-C P29 ledger:

```bash
python3 capability/distribution/build_p29_ledger.py SCENARIO.json \
  --codec /path/to/codec50-sink \
  --work /tmp/p29-codec --out /tmp/p29-ledger.jsonl
python3 capability/distribution/run_scenario.py SCENARIO.json \
  --codec p29 --ledger /tmp/p29-ledger.jsonl --out /tmp/p29-simulation
```

The P29 builder takes the common simulator assignment, admits the complete TU sequence into one
logical C catalogue, and forms one ordered projection per populated F.  Every projection gets an
independent receiver store and contiguous `REL_SEQ`; canonical Block IDs and the global plan
digest remain common.  The capability harness currently repeats the deterministic global/route
preparation for each output route and requires the resulting digest and Block count to agree
before it combines any bytes.  This repeats host-side harness CPU, but it does not create separate
logical learners or count shared state more than once.

For every F, the builder runs the real codec, parses the typed C-to-F and F-to-C streams into
causal Root/Need/LINES/Fill/close/Ack extents, requires the selector and component ledgers to
agree, and reruns the codec's directional sink replay.  LINES is released after Root
serialization, Need after Root delivery, Fill after Need delivery, and close joins every
material branch.  Fill can move ahead of an unfinished LINES extent at the next configured
writer quantum.  Compiler input becomes ready after close delivery; codec state commits after
Ack delivery.  Ledger rows are then restored to global `TU_SEQ` order for the common simulator.

Build and replay a multi-route GRZ ledger:

```bash
python3 capability/distribution/build_grz_ledger.py SCENARIO.json \
  --codec /path/to/grz2g \
  --work /tmp/grz-codec --out /tmp/grz-ledger.jsonl
python3 capability/distribution/run_scenario.py SCENARIO.json \
  --codec grz --ledger /tmp/grz-ledger.jsonl --out /tmp/grz-simulation
```

The GRZ builder uses the common simulator assignment, partitions TUs by `(C,F)`, and creates
one persistent G2 stream per route with one complete current-TU frame per transaction.  It
requires full route reconstruction, an identical retry, and exact selected prefix decodes.
The resulting current-TU frame is a one-node transaction graph and is then scheduled by the
same event engine used by every other adapter.

Ledger generation currently starts from the compile-only assignment and replay refuses any
placement drift.  That is exact for the primary `C1F20_200B1G` case because all 2,498 TUs fit in
the 4,000 available slots at each build barrier.  Slot-constrained dynamic-placement studies need
an explicit assignment trace or a converged assignment/codec loop before their physical timing is
reportable.

Run the deterministic integration gate from the repository root:

```bash
make integration_tests
```

This current gate covers the scenario engine, fork/join causality, independent readiness and
commit, writer-quantum priority, split staging/compiler capacity, simultaneous directional
bandwidth ceilings, active-time JSONL/report generation, physical-ledger refusal rules, and
P29/GRZ ledger parsing.  The later live-process launcher described in the architecture document
will extend this same target with scheduler, daemon, cache-sidecar, and compiler-pipe scenarios.

## Cold plus four-warm topology suite

`firefox-5build-topology-suite.json` is the one-command suite for the four requested
topologies.  Each C executes five complete Firefox builds: build 0 starts with cold codec
state, then builds 1 through 4 reuse the same adapter state.  A warm build is released only
after that C's previous build completes and a further 600 seconds elapse.  Cs advance their
own build sequences independently while competing for the common F pool and network.

The workload is the complete corrected 2,498-TU trace on every build.  Compile duration
remains paired with its source TU; this suite does not draw durations from an unrelated
synthetic distribution.

| scenario | Cs | Fs | slots/F | build epochs | TU executions |
|---|---:|---:|---:|---:|---:|
| giant F | 1 | 1 | 1,000,000 | 5 | 12,490 |
| 20-F pool | 1 | 20 | 50 | 5 | 12,490 |
| 50-F pool | 1 | 50 | 50 | 5 | 12,490 |
| ten concurrent Cs | 10 | 50 | 60 | 50 | 124,900 |

The million-slot case uses a sparse allocator, so simulator memory grows with slots that
actually run work rather than with the declared million-slot capacity.  The ten-C scenario
uses environment-round-robin ready selection so one C cannot occupy the whole initial
queue merely because its trace appeared first in JSON.

Run the complete diagnostic suite with:

```bash
python3 capability/distribution/run_suite.py \
  capability/distribution/firefox-5build-topology-suite.json \
  --codec compile-only --codec raw --require-payload \
  --out /tmp/firefox-5build-topology-suite
```

`compile-only` isolates scheduling and compiler occupancy.  `raw` sends every `.ii` byte
and exercises route serialization, propagation, shared-fabric allocation and compile
overlap.  They are controls, not compression candidates.  A stateful GRZ or P29 adapter
will use the same simulator instance for all five builds; it must key C state by
`environment` and F relationship state by `(environment, worker)`.

Every run writes `resolved-scenario.json`, `summary.json`, `assignments.tsv`, `events.tsv`,
`workers.tsv`, `builds.tsv`, and `generations.tsv`.  The suite additionally writes
deterministic `matrix.tsv`, `builds.tsv`, `generations.tsv`, and `suite-summary.json`; host
runner timings are deliberately kept separate in `runner-timings.tsv`.

The primary elapsed-time score removes the deliberate idle time between builds.  For build
generation `g`, take the earliest release across the participating Cs and the latest compile
completion across those Cs, then sum those five intervals:

```text
summed_generation_time = sum_g(max_g(compile_finish) - min_g(release))
```

This preserves overlap between concurrent Cs inside each generation, but does not charge the
four configured 600-second gaps.  The wall makespan is retained as a secondary scheduling
check.

The corrected full replay produced:

| topology | compile-only generation sum | raw generation sum | wall makespan (compile / raw) | raw C-to-F bytes |
|---|---:|---:|---:|---:|
| 1C / 1F / 1,000,000 slots | 116.383 s | 684.792 s | 2,516.383 / 3,084.792 s | 76,204,381,990 |
| 1C / 20F / 50 slots | 122.896 s | 154.908 s | 2,522.896 / 2,554.908 s | 76,204,381,990 |
| 1C / 50F / 50 slots | 116.383 s | 156.821 s | 2,516.383 / 2,556.821 s | 76,204,381,990 |
| 10C / 50F / 60 slots | 234.893 s | 684.550 s | 2,634.893 / 3,084.550 s | 762,043,819,900 |

Across both controls there are 130 per-C build rows: 26 cold and 104 warm.  Every noninitial
row has an observed 600,000,000,000 ns completion-to-release gap.  Each run also has exactly
five generation rows, and their durations sum exactly to its primary matrix value.

The combined 33 MiB ledger is retained at
`/tanksmall/scratch/ictmp/issue16-results/firefox-5build-topology-suite-v3/`.
SHA-256 values for `suite-summary.json`, `matrix.tsv`, `builds.tsv`, and `generations.tsv`
are respectively
`397e7cddabac50fa841df7f053f6fe1788f11a62a93eebd5d98b2a35c69fe673`,
`f2ca26e0c157079f2d8b98f1aac6a7015a242ee280c8f0ccc9a856bfd5c6908c`,
`04c40680e5543675ed42e9b1ba19ac9806e8971e7c575c59e351af9fce999715`, and
`5d7a9d7e6849d39f9a5677e344daafc882e3644496b9890096812a891f67792f`.

## First measured replay

The corrected 2,498-job trace contains 15,240,876,398 raw bytes and
10,270.631607299 seconds of compiler work.  With 20 one-slot Fs and dynamic
free-slot round-robin placement:

| diagnostic adapter | scored C-to-F | makespan | jobs/F | compile utilization/F |
|---|---:|---:|---:|---:|
| compile-only | 0 | 521.527208272 s | 109-140 | 97.86%-100% |
| raw TU | 15,240,876,398 B | 527.782199264 s | 104-140 | 96.68%-98.83% |

The raw control is only 6.255 seconds slower because its transfers fan out over 20
one-Gbit routes behind a ten-Gbit shared fabric and overlap compiler work.  It is a topology
and timing check, not evidence for either compression candidate.  The corresponding static
`logical % 20` compile-only bound is 606.626438963 seconds; that policy queues work at busy
Fs and is not the scenario's free-slot scheduler.

Detailed ledgers from this run are retained under
`/tanksmall/scratch/ictmp/issue16-results/firefox-1c-20f/`.  The compile-only/raw summary
SHA-256 values are respectively
`218802479d20126f4b76a64f2aa74f735d30d2f5348964ec2b8d9b3174e38945` and
`5b357d4a6f36125b360cac52db8d54f4af2806c8c9b2d74538fbfe040a9ea91e`.
