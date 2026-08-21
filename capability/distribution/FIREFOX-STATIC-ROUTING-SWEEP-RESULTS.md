# Firefox static-routing sweep results

## Decision

For the current source-transfer-plus-compile model, **P29 with a stable eight-F
frontier is the best joint operating point**. It sends 87,716,431 C-to-F bytes over
one cold and four unchanged warm Firefox builds, 71.642% less than the P29
round-robin control, while finishing 434,948,718 ns sooner. It also dominates P29
`k20`: 58,395,099 fewer C-to-F bytes and 283,361,206 ns less simulated time.

This is not yet an end-to-end result. The model has exact physical source bytes and
the corrected Firefox compile-duration trace, but it intentionally does not yet
schedule F decode, install, materialize, verification, or compiler-pipe work. It also
does not include compiled-result return traffic, optional environment setup, or
restart and eviction events. Physical codec CPU measurements are evidence about the
test run, not scheduled work in these rows.

## Experiment contract

Every cell uses the same corrected workload and machine model:

- 2,498 Firefox translation units per build;
- one cold build followed by four unchanged warm builds;
- 15,240,876,398 raw bytes per build and 76,204,381,990 raw bytes per cell;
- twenty available Fs, 200 compiler slots and 400 input-staging slots per F;
- one aggregate 1-Gbit/s C uplink;
- all translation units released at generation start, with no invented producer
  timing;
- resident compiler environments;
- a separately built, scenario-bound physical ledger for every routing/codec cell.

The seven routing rows are the existing dispatch-time round-robin control and static
stable rendezvous frontiers `k1`, `k2`, `k3`, `k4`, `k8`, and `k20`. The stable
identity excludes build number, so an unchanged compile identity returns to the same
F on every rebuild. Static rows do not spill outside their selected frontier.

Across both codecs, the sweep contains 174,860 simulated jobs and accounts for
1,066,861,347,860 raw input bytes. Every cell passed payload reconstruction,
scenario/binary binding, TU and relationship sequence closure, stable rebuild
destination checks where applicable, directional byte closure, JSONL event closure,
and exact wall/active-time tiling.

## Exact P29 results

Signed deltas use P29 round-robin as the baseline. Negative byte deltas are savings;
negative time deltas are faster.

| policy | populated Fs | C-to-F bytes | delta C-to-F bytes | delta C-to-F | F-to-C bytes | delta F-to-C bytes | delta F-to-C | makespan ns | delta makespan ns | delta makespan |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| round-robin | 20 | 309,315,644 | 0 | 0.000% | 5,503,885 | 0 | 0.000% | 117,011,001,133 | 0 | 0.000% |
| k1 | 1 | 38,179,250 | -271,136,394 | -87.657% | 1,206,051 | -4,297,834 | -78.087% | 319,816,951,074 | +202,805,949,941 | +173.322% |
| k2 | 2 | 47,664,064 | -261,651,580 | -84.590% | 1,389,321 | -4,114,564 | -74.757% | 196,270,996,496 | +79,259,995,363 | +67.737% |
| k3 | 3 | 55,609,787 | -253,705,857 | -82.022% | 1,535,618 | -3,968,267 | -72.099% | 155,966,728,723 | +38,955,727,590 | +33.292% |
| k4 | 4 | 62,925,377 | -246,390,267 | -79.657% | 1,664,879 | -3,839,006 | -69.751% | 134,144,661,273 | +17,133,660,140 | +14.643% |
| k8 | 8 | 87,716,431 | -221,599,213 | -71.642% | 2,091,358 | -3,412,527 | -62.002% | 116,576,052,415 | -434,948,718 | -0.372% |
| k20 | 20 | 146,111,530 | -163,204,114 | -52.763% | 3,069,588 | -2,434,297 | -44.229% | 116,859,413,621 | -151,587,512 | -0.130% |

The per-build C-to-F curve shows why stable routing matters to P29:

| policy | cold bytes | warm 1 | warm 2 | warm 3 | warm 4 |
|---|---:|---:|---:|---:|---:|
| round-robin | 148,281,081 | 48,300,619 | 40,436,003 | 37,037,024 | 35,260,917 |
| k1 | 37,157,314 | 302,143 | 239,931 | 239,931 | 239,931 |
| k2 | 46,642,367 | 301,937 | 239,920 | 239,920 | 239,920 |
| k3 | 54,588,388 | 301,639 | 239,920 | 239,920 | 239,920 |
| k4 | 61,904,496 | 301,121 | 239,920 | 239,920 | 239,920 |
| k8 | 86,696,829 | 299,842 | 239,920 | 239,920 | 239,920 |
| k20 | 145,092,018 | 299,752 | 239,920 | 239,920 | 239,920 |

After the first warm build, every stable frontier reaches essentially the same
approximately 0.240 MB/build floor. Wider frontiers mainly pay more cold teaching.
Round-robin does not preserve the same receiver placement and continues to send tens
of megabytes on every warm build.

The exact five-build P29 phase totals are:

| policy | Root | LINES | Fill | close | Need (F-to-C) | Ack (F-to-C) |
|---|---:|---:|---:|---:|---:|---:|
| round-robin | 11,728,828 | 292,373,190 | 4,226,916 | 986,710 | 4,829,425 | 674,460 |
| k1 | 1,648,967 | 35,007,821 | 535,752 | 986,710 | 531,591 | 674,460 |
| k2 | 2,025,754 | 43,989,281 | 662,319 | 986,710 | 714,861 | 674,460 |
| k3 | 2,299,594 | 51,560,974 | 762,509 | 986,710 | 861,158 | 674,460 |
| k4 | 2,519,798 | 58,570,609 | 848,260 | 986,710 | 990,419 | 674,460 |
| k8 | 3,207,901 | 82,404,139 | 1,117,681 | 986,710 | 1,416,898 | 674,460 |
| k20 | 4,643,975 | 138,762,176 | 1,718,669 | 986,710 | 2,395,128 | 674,460 |

`LINES` is the replication curve. The fixed per-TU close/Ack costs explain the stable
warm floor, while Root, Fill, and Need grow much more slowly than LINES.

## Exact GRZ results

Signed deltas use GRZ round-robin as the baseline. GRZ has no F-to-C phase in this
physical ledger revision, so all reverse-byte values and deltas are zero.

| policy | populated Fs | C-to-F bytes | delta C-to-F bytes | delta C-to-F | F-to-C bytes | makespan ns | delta makespan ns | delta makespan |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| round-robin | 20 | 434,443,557 | 0 | 0.000% | 0 | 117,046,611,916 | 0 | 0.000% |
| k1 | 1 | 300,722,540 | -133,721,017 | -30.780% | 0 | 319,655,627,483 | +202,609,015,567 | +173.101% |
| k2 | 2 | 327,211,477 | -107,232,080 | -24.683% | 0 | 196,242,679,377 | +79,196,067,461 | +67.662% |
| k3 | 3 | 344,490,945 | -89,952,612 | -20.705% | 0 | 156,035,378,571 | +38,988,766,655 | +33.310% |
| k4 | 4 | 361,619,065 | -72,824,492 | -16.763% | 0 | 134,410,872,407 | +17,364,260,491 | +14.835% |
| k8 | 8 | 397,423,960 | -37,019,597 | -8.521% | 0 | 116,658,914,256 | -387,697,660 | -0.331% |
| k20 | 20 | 351,900,503 | -82,543,054 | -19.000% | 0 | 117,341,920,199 | +295,308,283 | +0.252% |

The exact GRZ per-build C-to-F curve is:

| policy | cold bytes | warm 1 | warm 2 | warm 3 | warm 4 |
|---|---:|---:|---:|---:|---:|
| round-robin | 122,717,639 | 76,174,103 | 76,151,849 | 77,501,881 | 81,898,085 |
| k1 | 61,708,295 | 59,641,689 | 59,741,656 | 59,772,362 | 59,858,538 |
| k2 | 67,687,383 | 64,753,294 | 64,838,232 | 64,915,229 | 65,017,339 |
| k3 | 72,885,598 | 67,933,604 | 67,775,218 | 67,938,872 | 67,957,653 |
| k4 | 77,607,991 | 70,983,231 | 70,888,360 | 70,937,924 | 71,201,559 |
| k8 | 91,206,336 | 77,057,140 | 76,452,427 | 76,404,988 | 76,303,069 |
| k20 | 120,315,807 | 1,042,901 | 111,739,340 | 19,615,879 | 99,186,576 |

GRZ does not turn stable repeats into P29's small missing-data floor. Its `k20` warm
curve is also strongly non-monotone even though stable destination retention passed:
1.043 MB, 111.739 MB, 19.616 MB, then 99.187 MB. The experiment establishes that
fact but does not isolate whether route-local ordering, bounded history, or adaptive
codec state causes it. A single warm build is therefore not a valid steady-state GRZ
estimate.

At `k8`, P29 sends 309,707,529 fewer C-to-F bytes than GRZ (77.929% less) and is
82,861,841 ns faster in the current model. The timing difference is only 0.071%
because compilation dominates and codec CPU/F materialization are not yet scheduled.
P29 and GRZ each win a few narrow timing comparisons at smaller frontiers because
their dialogue shapes differ; those sub-second differences must not be read as an
end-to-end codec ranking before the missing F stages are measured.

## Runtime resources for the evidence build

These are harness wall times and maximum resident set sizes from `/usr/bin/time -v`.
Seven codec builders were run concurrently on `tt-quietbox` (32 cores, 503 GiB RAM,
NVMe storage), so the wall values include shared-storage contention. They are not
codec throughput claims.

| policy | codec | physical builder wall | builder max RSS KiB | corrected simulator wall | simulator max RSS KiB |
|---|---|---:|---:|---:|---:|
| round-robin | P29 | 33:13.14 | 18,583,472 | 12:49.66 | 979,376 |
| k1 | P29 | 25:14.98 | 18,313,776 | 2:08.16 | 576,880 |
| k2 | P29 | 25:31.91 | 18,346,380 | 1:45.79 | 659,664 |
| k3 | P29 | 25:42.17 | 18,339,772 | 1:38.95 | 689,280 |
| k4 | P29 | 25:54.61 | 18,370,872 | 1:36.64 | 731,920 |
| k8 | P29 | 26:37.53 | 18,467,108 | 1:43.11 | 873,956 |
| k20 | P29 | 28:31.56 | 18,641,284 | 4:09.11 | 928,280 |
| round-robin | GRZ | 1:42:42 | 2,160,472 | 2:47.24 | 588,056 |
| k1 | GRZ | 2:00:21 | 2,422,684 | 1:23.72 | 475,860 |
| k2 | GRZ | 2:08:13 | 2,294,940 | 1:07.95 | 522,948 |
| k3 | GRZ | 2:04:39 | 2,245,788 | 1:03.79 | 553,776 |
| k4 | GRZ | 2:05:49 | 2,223,648 | 1:03.27 | 584,412 |
| k8 | GRZ | 1:47:46 | 2,181,648 | 1:23.87 | 629,476 |
| k20 | GRZ | 1:48:26 | 2,171,284 | 1:59.51 | 650,636 |

The GRZ physical gate is deliberately exhaustive: full route decode, deterministic
retry, and prefix reconstruction at selected cuts. It generated roughly 748--928 GB
of filesystem output per cell while checking a 76.2 GB raw cell. That verification
I/O explains much of the builder wall time and is separate from simulated transfer
time. The machine stayed well within memory and storage capacity; no swaps occurred
in any codec builder.

## Timeline correction and provenance

The physical builders originally ran from remote source snapshot
`59c92a00fbd7b32f1983c0091f132af94734c9a5`. A simulator-only defect rounded every
fractional interval upward independently while the summary rounded the cumulative
duration, allowing a one-nanosecond aggregate mismatch. The correction serializes
each interval as the difference between rounded cumulative endpoints. It cannot
change codec bytes, reconstruction, routing, or assignments.

All original GRZ simulator output is preserved under each cell's
`pre-timeline-fix/` directory. The seven expensive physical ledgers were then replayed
from corrected source commit `8afcbfc766f46e1b4c9c888241d55fbbd299cbaa`. The
corrected `run_scenario.py` SHA-256 is
`1efae1a899af5733a6ef6ed330ad1c3abe8bb3b7a9e6c2caf80c8596f02169e8`; the sweep
harness SHA-256 is
`eebe2993c44ccb70b2f6fc7a12034a78ce0864677c83bd06caeeeffe3f1afa4a`.

The physical codec binaries are fixed by digest:

- P29: `c79d7fa3ba9f2b28a5dca896c661b37229e864250a028ebe1d264539493781d5`;
- GRZ: `4561ffcb1e6abb829e7fbf9507ec4524d4476e67c30f928f799a0f1968a66004`.

The corrected Firefox trace SHA-256 is
`9fa7124f63212ccfd78f139dc05dcc5b2737ef868dc3e6878e64f609e079e960`.
The original experiment manifest SHA-256 is
`2786bb2eabca8b68c3e49178820980a6edab4bee36b3a8715b78f2ee6b875af8`.
The final matrix and generated report hashes are:

- `matrix.json`: `a2fa25b1d5366f1b61e9953078ad0174771164d8d48979041f51dbcebb886f0d`;
- `matrix.tsv`: `178040a6b233a6380a5c7cddbb979938b4e75e497bf90bd8e02cddf1c5768a19`;
- generated Markdown: `3334408ac2964463eb8bf80bf6a504cbe48652a63461f762054885a90bf141f8`.

The source archive is
`/tanksmall/scratch/ictmp/issue16-simulator-research-8afcbfc.tar.gz`, SHA-256
`0c4e80245e757797adc957cdabfd9c3c983f255052a954eaa219557103ffe96d`.

The complete remote evidence, including physical codec work trees and preserved
pre-fix replays, is retained at:

```text
/home/ttuser/issue16-results/firefox-static-routing-sweep-0c94eab/
```

The curated local evidence, excluding the large codec work trees and pre-fix replay
copies, is retained at:

```text
/tanksmall/scratch/ictmp/issue16-results/firefox-static-routing-sweep-8afcbfc/
```

The primary generated report is
`FIREFOX-STATIC-ROUTING-SWEEP.md`; `matrix.json` and `matrix.tsv` are the canonical
machine-readable and flat result tables. Every cell retains its scenario JSON,
physical ledger, builder command/log/time files, corrected simulator
command/log/time files, JSONL timeline, TSV projections, summary, and HTML report.

## Gates

The final local source branch passes:

```text
make -f Makefile.am integration_tests
46 tests PASS; compileall PASS

ruff check capability/distribution
PASS

ruff format --check capability/distribution/run_scenario.py \
  capability/distribution/test_run_scenario.py \
  capability/distribution/run_firefox_static_routing_sweep.py
PASS

git diff --check
PASS
```

The sweep report independently re-read and reconciled all fourteen scenarios,
ledgers, assignment tables, summaries, and JSONL timelines before it wrote the
matrix.

## Next coherent experiment

Implement the ruling's next stage without changing routing or physical source bytes:

1. Add explicit F receive/decode, receiver-state install, materialize, byte-verify,
   and compiler-pipe stages.
2. Bind measured per-TU work to those stages; compile may start only after the exact
   materialized payload has entered the compiler pipe.
3. Emit stage start/finish events and per-F queue/active state into JSONL, while
   retaining exact wall and active-time tiling.
4. Run P29 `k8` as the primary row, P29 round-robin as its routing control, and GRZ
   `k8` as the frame-stream comparison. Keep the unusual GRZ `k20` warm curve as a
   separate order/history diagnostic rather than broadening the first stage patch.
5. Require unchanged source byte totals and route assignments, exact materialized
   payload hashes, zero unfinished work, and byte/time/stage closure.

Do not add preprocessing to this model. After these F stages close, add exact
compiled-result return traffic as the next separate slice.
