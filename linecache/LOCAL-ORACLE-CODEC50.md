# Local-oracle Protocol-50 contender

Issue: `mickg10/icecream#16`

This is the first independently implemented local-oracle transport contender. It is a working
two-process codec, not a size model. Its present value is the measured transport substrate: it
round-trips every byte and clears 1.0 GB/s on all four primary corpora. Its compression ratio is not
yet competitive with the 400x goal, so it is a platform for the definition-coding bake-off rather
than a proposed winner.

## What actually runs

`linecache/codec50.cpp` forks and execs a receiver process. The receiver gets only a socket FD and an
output-pipe FD; it has no manifest and begins with empty stores.

The C side performs the following work chronologically:

1. Intern exact Lines and marker Regions for the current TU.
2. Cover the Region sequence with Blocks learned only from preceding TUs.
3. Use the per-conversation view of F's installed objects to assemble a proactive FILL. Every Region,
   Block, and Line definition in that FILL carries an explicit stable key.
4. Serialize DICT, ROOT, and FILL as four-byte-length-prefixed frames. Each payload is either raw or
   actually compressed with zstd; the shorter real representation wins.
5. Keep at most eight TUs in flight. F sends an early zero-missing/ready response after installing a
   TU closure and a later ACK after emitting the exact TU bytes.

The F side independently parses the frames, decompresses the real bytes, installs immutable
Line/Region/Block objects, expands the Root, and writes reconstructed bytes through a pipe. A third
consumer compares the pipe stream byte-for-byte with the corpus. Region expansions are retained once
and sent with `writev`, avoiding a second whole-TU staging copy.

The eight-TU window is bounded. It is large enough to overlap C's next-TU work with F's current-TU
output but small enough to preserve natural socket backpressure. An unbounded experiment was rejected
after both socket directions filled. A one-TU lockstep mode remains available as `--sync`.

## Measurement boundary

Two intervals are printed deliberately:

- `codec wall`: after the fixture corpus is in memory, from the codec's first consumed TU through the
  last reconstructed byte, final ACK, and F summary. This corresponds to bytes arriving from the local
  compiler pipe and is the 1.0 GB/s gate used below.
- `full wall`: includes reading all benchmark files from storage before the codec begins. This is useful
  harness information but is not the compiler-pipe path.

Neither interval uses computed byte estimates. `wire` is the sum of frame bytes actually written in
both directions, including every four-byte outer length and eight-byte frame header.

## Zen4 five-run result

Host and command shape:

```text
AMD EPYC 8124P, 16 cores / 32 threads
Linux 6.8.0-124-generic
g++ 11.4.0
libzstd 1.4.8
g++ -O3 -march=native -std=c++17 codec50.cpp -o codec50-local -lzstd -pthread
taskset -c 2-4 ./codec50-local --manifest MANIFEST --z 1 --window 8
```

One untimed development/setup sequence preceded these five consecutive measured repetitions. Each of
the 20 measured corpus runs reported `byte-exact=PASS` and `child=PASS`.

| Corpus | Raw bytes | Actual two-way wire | Ratio | Codec GB/s median | Codec range | Fixture-inclusive GB/s median |
|---|---:|---:|---:|---:|---:|---:|
| LLVM | 3,620,271,340 | 20,042,136 | 180.63x | 1.457 | 1.420-1.722 | 0.740 |
| RocksDB | 3,114,320,596 | 18,600,506 | 167.43x | 1.397 | 1.332-1.473 | 0.730 |
| DuckDB | 1,985,715,205 | 17,912,393 | 110.86x | 1.102 | 1.064-1.156 | 0.639 |
| OpenCV | 4,630,994,774 | 15,987,113 | 289.67x | 1.793 | 1.721-1.902 | 0.819 |

DuckDB remains the binding speed and size corpus, but even its slowest measured codec run stayed above
1.0 GB/s.

### DuckDB window sweep

Single-run capability sweep, identical bytes and affinity:

| In-flight TU bound | Codec GB/s |
|---:|---:|
| 2 | 0.911 |
| 4 | 1.124 |
| 8 | 1.130 |
| 16 | 1.141 |

Window 8 was retained: window 4 had too little margin, while window 16 bought speed at the cost of
twice the queued work. The five-run window-8 median is more meaningful than a single sweep point.

## Other completed checks

- ASan + UBSan, 20 LLVM TUs, zstd-1, window 8: exact PASS, no reported finding.
- zstd off, zstd-1, and zstd-3: exact PASS.
- pre-TU prefix/suffix predictor: exact PASS, but it enlarged the actual zstd-1 wire on the 20-TU
  control (39.13x versus 39.52x), so it is disabled by default.
- Blocks disabled: exact PASS; ratio worsened on the same control (36.57x versus 39.52x).
- Two chronological passes with persistent F state: exact PASS.

## Generic pretraining experiment

Training on standard C++ is feasible, but raw source alone is not the target distribution. The cold
definition stream contains preprocessed header bodies, macro expansions, marker records, generated
declarations, and exact whitespace. The experiment therefore needs three separately labelled rows:

1. `GENERIC_SOURCE`: unrelated raw C/C++ projects.
2. `GENERIC_II`: unrelated buildable projects preprocessed under several compiler/header profiles.
3. `TOOLCHAIN_BASE`: matching compiler and header environment, treated as a distinct deployment row.

Repositories and revisions must be split as whole groups between training and evaluation. The useful
first artifacts are small and independently measurable: trained zstd dictionaries, byte phrases,
exact skeleton/templates, residual coding tables, and an encoder-only candidate ranker. Evaluation is:

```text
no predictor
generic pretrained predictor
generic predictor + strict encode-before-learn project adaptation
```

Every row emits explicit base/template IDs plus exact residual bytes and uses the same F decoder. An
incorrect prediction can only cost bytes or encoder time. The final choice is made from actual framed
bytes, and predictor time remains inside the complete codec throughput measurement.

## Honest gaps

1. Ratios of 110.86x-289.67x do not meet the 400x target. Definition coding, especially DuckDB's
   generated/template-heavy distinct Lines, is now the primary work.
2. The pipelined fast path relies on the exact per-conversation C view of F. Any nonzero correction
   response currently fails the benchmark rather than being serviced concurrently. The product path
   needs a small control reader (or the existing lockstep correction mode) while keeping the bounded
   data window.
3. The output consumer is an exact pipe verifier, not a real compiler process. A compiler-pipe scenario
   gate remains required.
4. The research frame fields currently use host byte order. Production Protocol 50 must specify the
   integer byte order explicitly.
5. Generation rollover, cache retention, and per-C eviction are not part of this contender yet.
6. The file retains the implementer's older comparison harness above the authoritative local-oracle
   path. It is never called by `main` and should be split before product integration.

## Next bake-off rounds

1. Reuse the common chronological definition event stream and exact decoder.
2. Implement skeleton-grouped column coding for DuckDB as the first independent size contender.
3. Measure trained zstd dictionaries and small static phrase/template bases on disjoint projects.
4. When bigoracle's learned systems land, compete with the same K=1/2/4/8 candidate-cost interface:
   deterministic ordering, generic ranker, and generic-plus-online adaptation.
5. Integrate only rows that preserve byte-exact reconstruction and keep the complete codec above
   1.0 GB/s with margin.

The raw five-run log is `linecache/traces/local-codec50-zen4-5x.log`.
