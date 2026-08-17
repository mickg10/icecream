# P29 stage-performance profile

## Result

The approximately 1.0 GB/s P29 result is a cold Godot material-path result, not
the speed of the whole design.  On the designated Zen 4 host, the ordinary P29
path reaches 2.40--4.92 GB/s at C and 6.67--16.96 GB/s at F for DuckDB and
LLVM.  Godot alone activates the embedded-object path and falls to 1.04 GB/s at
C and 1.30 GB/s at F.

The largest isolated Godot costs are:

- 2.92 seconds at F to recreate the exact original zlib members;
- 1.59 seconds in all zstd encode calls;
- 1.11 seconds at C to probe and inflate embedded-object candidates;
- approximately 2.77 seconds of remaining C classification, serialization,
  allocation, and copying.

Increasing the exact zlib-recreation pool from 8 to 32 workers improves F fill
from 1.30 to only 1.36 GB/s.  More workers do not remove this limit.

## What the stages are

```text
external preprocessor stdout                                      outside this run
        |
        v
  C ingest/load  ->  Line interning + Region formation  ->  S1 superblock discovery
                                                               |
                                                               v
                    ROOT / Block names  ----------------------> F
                                                               |
                                                        derive NEED set
                                                               |
                                                               v
  C FILL construction <----------------------------- missing Region/Block IDs
    - classify first-use Region material
    - encode marker, literal, shared-Line, and byte-array operations
    - probe/inflate complete embedded zlib members where present
    - MO-factor admitted catalog material where present
    - zstd-compress the independent streams and frame them
        |
        v
  transport  -------------------------------------------------> F FILL install
                                                               - zstd decode
                                                               - MO decode
                                                               - exact zlib recreation
                                                               - install Regions/Blocks
                                                                        |
                                                                        v
                                                               expand ROOT to exact .ii
                                                                        |
                                                                        v
                                                               remote compiler stdin
```

The current `codec50` executable simulates both endpoints in one process.  It
loads and interns the complete corpus before running the chronological codec.
The operations are causal, but the harness does not yet measure the intended
streaming overlap or the real process boundaries.

## Measurement definition

Unless a row explicitly says `native`, throughput is:

```text
complete original .ii bytes / wall seconds spent in that stage
```

The units are decimal GB/s.  This common raw-input denominator makes stage
costs directly comparable.  It is not wire throughput and is unrelated to the
fixed 125 MB/s payload rate used to calculate ideal 1-Gbit transfer time.

The zstd `native` rates instead divide the logical bytes entering or leaving
zstd by zstd wall time.  Those bytes are already reduced material/control
streams, so their native rate must not be compared directly with raw `.ii`
GB/s.

The timed boundaries are non-overlapping at the top level:

1. filesystem load;
2. interning plus Region formation;
3. S1 longest-prior-factor/superblock construction;
4. ROOT construction, F requirement derivation, and NEED roundtrip simulation;
5. C FILL classification, serialization, and compression;
6. F FILL decompression, material reconstruction, and cache installation;
7. F ROOT expansion and exact `.ii` emission;
8. harness-only comparison against the original and ledger accounting.

Nested counters separately time zstd encode/decode, embedded-object
probe/inflate, MO encode/decode, and exact canonical zlib recreation.  Nested
times therefore explain their enclosing top-level stage and must not be added
again to obtain total wall time.

## Full cold measurements

Host: `tt-quietbox2`, AMD EPYC 8124P (Zen 4), 16 cores / 32 threads.  Codec work
was pinned to CPUs 0--15 for the binding 8-worker run.  P29 used zstd-3 for the
ordinary streams and zstd-9/LDM with four workers, 5 MiB jobs, and overlap-log
3 for the selected embedded-object stream.

| stage, raw-input denominator | LLVM | DuckDB | Godot |
|---|---:|---:|---:|
| filesystem load | 1.51 GB/s | 1.53 GB/s | 1.53 GB/s |
| intern + Region formation | 5.32 GB/s | 3.08 GB/s | 2.61 GB/s |
| S1 superblock discovery | 13.22 GB/s | 20.73 GB/s | 16.64 GB/s |
| conversation + NEED | 41.04 GB/s | 35.47 GB/s | 55.85 GB/s |
| C FILL/classify/compress | **4.92 GB/s** | **2.40 GB/s** | **1.04 GB/s** |
| F FILL/decompress/install | **16.96 GB/s** | **6.67 GB/s** | **1.30 GB/s** |
| F ROOT expand/emit | 17.44 GB/s | 13.81 GB/s | 12.97 GB/s |
| harness exact comparison | 28.73 GB/s | 27.55 GB/s | 26.26 GB/s |

All three runs reconstruct every input byte exactly.  Their complete wire
endpoints remain the retained P29 endpoints.

The filesystem row is a property of this file-backed harness.  The product
path receives preprocessor output through a pipe, so it needs a separate
preprocessor-to-C streaming measurement.

## Godot deep split

The 8-worker full Godot run covers 2,207 TUs and 5,932,762,185 original bytes.

| enclosing or nested operation | wall seconds | relevant rate |
|---|---:|---:|
| C conversation + NEED | 0.106 | 55.85 raw GB/s |
| C FILL total | 5.682 | 1.04 raw GB/s |
| all zstd encode calls | 1.591 | 0.124 native input GB/s |
| C embedded-object probe + inflate | 1.106 | 5.37 raw GB/s contribution rate |
| C MO encode/commit | 0.210 | about 0.54 GB/s of selected MO member input |
| remaining C FILL work, approximate | 2.775 | 2.14 raw GB/s contribution rate |
| F FILL total | 4.567 | 1.30 raw GB/s |
| all zstd decode calls | 0.226 | 0.871 native output GB/s |
| F MO decode | 0.170 | about 0.67 GB/s of selected MO member output |
| F exact zlib recreation | 2.920 | about 0.043 GB/s of inflated member input |
| remaining F FILL work, approximate | 1.250 | 4.75 raw GB/s contribution rate |
| F ROOT expand/emit | 0.458 | 12.97 raw GB/s |

The zstd counters include the small conversation frames as well as material
frames.  The approximate remainder rows subtract the named nested counters
from their enclosing stage and are therefore diagnostic, not separately timed
blocks.

The sampled cycle profile agrees with the wall timers.  Across 13,000 samples
with no lost samples, zlib routines account for roughly 42% of sampled cycles,
zstd compression routines roughly 12%, `memcmp` 7.75%, and byte-array parsing
1.03%.  Cycle shares include all worker threads and therefore do not equal wall
shares.

## Exact zlib-recreation worker scaling

| worker limit | F FILL wall | F FILL raw rate | exact recreation wall |
|---:|---:|---:|---:|
| 8 | 4.567 s | 1.30 GB/s | 2.920 s |
| 16 | 4.459 s | 1.33 GB/s | 2.843 s |
| 32 | 4.360 s | 1.36 GB/s | 2.780 s |

The 32-worker row is only 4.8% faster than the 8-worker row for exact
recreation.  The work is limited by the exact algorithm and member-size
distribution, not by the configured worker count.  The existing bounded
host-level pool remains the right execution shape; simply enlarging it is not
a useful optimization.

## Interpretation

- Interning is not the P29 codec limit.  The full cold interning/Region stage
  is 2.61--5.32 GB/s on these three corpora, while the isolated hot trace replay
  previously reached 14.2 GB/s on this host.
- S1 is cheap.  At 13.22--20.73 GB/s it does not justify weakening the
  superblock representation for speed.
- ROOT/NEED processing is negligible in these complete runs.
- The ordinary material path has comfortable headroom.  Godot's special
  embedded-object path sets the current cold floor.
- A serial Godot C path of intern + S1 + encode takes about 8.31 seconds, or
  0.71 raw GB/s.  Its stage-overlapped ceiling is the 1.04 GB/s C FILL stage.
  Actual overlap still needs to be demonstrated with the preprocessor pipe and
  process topology.
- F's exact recreation is cold first-definition work.  Once the reconstructed
  Region is resident in the F cache, later builds should skip it.  The pending
  retained-state four-build run must quantify that claim rather than infer it.

## Remaining performance gates

This profile closes the question of where P29 spends CPU time, but it is not an
end-to-end product result.  The remaining measurements are:

1. external preprocessor stdout through live C ingestion/interner;
2. C fork to C cache/service transfer and returned compressed FILL;
3. actual C-to-F socket transfer with the packed four-byte frame header;
4. F cache/service to F compiler-fork transfer;
5. F output into a live compiler stdin pipe;
6. cold build 1 followed by retained-state builds 2--4, with per-build stage
   time and bytes;
7. concurrent-job scaling with one bounded host-level worker pool;
8. per-TU latency distributions, not only whole-corpus throughput.

## Reproduction and retained evidence

Instrumented source SHA-256:
`1c7e6c22606183269a683acc77367acab05c5a22f03397f6148213a3930b9dd7`.
The warning-clean binary SHA-256 is
`354e236a10d662b9a98c5dca2f9e30efbc2306ef7ba467876ef96c0daa833ece`.

Retained on `tt-quietbox2` under
`/home/ttuser/ictmp/issue16-p29-stage-profile/`:

| evidence | SHA-256 |
|---|---|
| `godot-detailed-r1.log` | `5153f939fe2f87635fdc5ea174746184688a0bde22d1c750c82ada98d63af2ec` |
| `godot-blobthreads-16.log` | `900790608e4e30f409e16b04d0fed8ec110e96c6f7f67691080f5e601d870395` |
| `godot-blobthreads-32.log` | `38949a535eb7b0f04f6447ee0e2a35f138631380341876b4f3201fcee6e360ef` |
| `llvm-detailed.log` | `95341d1dc56fa9ac2d8c30ac977e7db20c6c8ed226413e7630d07bfb9a8f88d4` |
| `duckdb-detailed.log` | `f1ccd04326a39f130c504b21a40cbd398fdedf66f9f09c32bf29d916d149e049` |
| `godot-perf.data` | `a805cf8df4d89ac61ba96b290632b622a916937039cefa545e09bb74f8eabffa` |
| `godot-perf-report.txt` | `986d4e827d70023af1c2ef592d641334258d1c34993060d1d7e3966e4def476a` |

