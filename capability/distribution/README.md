# Distribution experiments

One scenario document owns the workload, C/F topology, timing, network and scheduler.
GRZ and P29 are codec adapters beneath that common experiment; they do not own separate
scheduler simulations.

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
supplies a sequence of C-to-F/F-to-C phases for each dispatched TU; it does not get its own
scheduler.  The core reserves real F slots, shares the configured fabric between active
flows, applies per-route link capacity, and dispatches another TU only after a slot becomes
free.

```bash
python3 capability/distribution/run_scenario.py \
  capability/distribution/firefox-1c-20f.json \
  --codec compile-only \
  --out /tmp/firefox-compile-only
```

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
`workers.tsv`, and `builds.tsv`.  The suite additionally writes deterministic `matrix.tsv`,
`builds.tsv`, and `suite-summary.json`; host runner timings are deliberately kept separate
in `runner-timings.tsv`.

The first full replay produced:

| topology | compile-only makespan | raw makespan | raw C-to-F bytes |
|---|---:|---:|---:|
| 1C / 1F / 1,000,000 slots | 2,516.383 s | 3,084.792 s | 76,204,381,990 |
| 1C / 20F / 50 slots | 2,522.896 s | 2,554.908 s | 76,204,381,990 |
| 1C / 50F / 50 slots | 2,516.383 s | 2,556.821 s | 76,204,381,990 |
| 10C / 50F / 60 slots | 2,634.893 s | 3,084.550 s | 762,043,819,900 |

These makespans include four 600-second inter-build gaps.  Across both controls there are
130 per-C build rows: 26 cold and 104 warm.  Every noninitial row has an observed
600,000,000,000 ns completion-to-release gap.

The combined 33 MiB ledger is retained at
`/tanksmall/scratch/ictmp/issue16-results/firefox-5build-topology-suite-v2/`.
SHA-256 values for `suite-summary.json`, `matrix.tsv`, and `builds.tsv` are respectively
`64c50672b9cdd3ddd7682f50d1948af46a73b91013ea4b671e8e90a5251279a7`,
`1b15f27bec60e42094d1539242e7084fa36ad520da41bf165bcf3ba5c09ff169`, and
`04c40680e5543675ed42e9b1ba19ac9806e8971e7c575c59e351af9fce999715`.

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
