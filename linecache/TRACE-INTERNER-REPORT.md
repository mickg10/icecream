# Exact trace-interner capability benchmark

This is a standalone capability experiment for issue #16.  It measures how
quickly one CPU thread can turn a preprocessed byte stream into the exact
stable line-ID stream when repeated marker-aligned spans may be replayed.  It
is not the production C/F protocol and does not include compression, framing,
socket transfer, helper scheduling, or compiler-pipe writes.

The tested source is `trace-interner-bench.cpp`, SHA-256
`d69ec8307e585c46a24a5de2f58b4cb94bd2826e903f6f0e9c08d44d92141e34`.

## Algorithm

The scanner divides each TU at preprocessor marker lines (`# ` at the start of
a line).  For each span it tries, in order:

1. the two previously observed successors of the preceding span;
2. a growable open-addressed span index selected by a sampled fingerprint;
3. exact line-by-line insertion when the span is new.

A predictor or index result is only a candidate.  Equal byte length and a full
byte comparison are required before its stored `u32` ID vector is replayed.
New spans are split into lines and interned through exact packed tables for
lengths up to 4 and 16 bytes, then a full-line hash table with exact byte
comparison.  Every occurrence ID is written to the per-TU output buffer.

The benchmark's untimed validator resolves every output ID and compares every
reconstructed byte with the original TU.  The output buffer is also exposed to
a compiler barrier during timed passes so the ID production cannot disappear.

## Machine and build

Runs were pinned to physical core 8 on:

```text
AMD EPYC 8124P 16-Core Processor (16 cores / 32 threads)
Linux 6.8.0-124-generic x86_64
g++ 11.4.0
```

Build and run commands:

```sh
g++ -O3 -DNDEBUG -std=c++17 -march=native \
  linecache/trace-interner-bench.cpp -o trace-interner-bench
taskset -c 8 ./trace-interner-bench --manifest MANIFEST
taskset -c 8 ./trace-interner-bench --manifest MANIFEST --validate

# Exact line-at-a-time fallback row.
g++ -O3 -DNDEBUG -std=c++17 -march=native -DLINE_ONLY \
  linecache/trace-interner-bench.cpp -o trace-interner-line

# Make every inexpensive selector equal; exact comparisons must still decide.
g++ -O3 -DNDEBUG -std=c++17 -march=native \
  -DFORCE_SELECTOR_COLLISIONS \
  linecache/trace-interner-bench.cpp -o trace-interner-alias-check

# Bounds and undefined-operation check on the 20-TU slice.
g++ -O1 -g -std=c++17 -march=native \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  linecache/trace-interner-bench.cpp -o trace-interner-sanitize
```

The corpus is loaded before timing.  `COLD` starts with an empty interner and
includes scanning, splitting, insertion, and reuse later in the corpus.  `HOT`
is the all-present second pass.  Rates are decimal GB/s.  Hardware counters are
enabled only around each measured pass.

## Median of three

| Corpus | TUs | Raw bytes | Distinct lines | COLD GB/s | HOT GB/s | HOT predictor hit / span | Cache used / allocated |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| LLVM | 1,238 | 3,620,271,340 | 771,055 | 6.698 | 13.944 | 96.33% | 147.92 / 267.15 MiB |
| RocksDB | 622 | 3,114,320,596 | 587,613 | 4.191 | 13.006 | 97.11% | 230.11 / 350.64 MiB |
| DuckDB | 689 | 1,985,715,205 | 637,610 | 3.896 | 13.983 | 97.57% | 207.81 / 348.64 MiB |
| Abseil | 700 | 2,581,008,467 | 376,835 | 5.701 | 12.661 | 94.87% | 153.18 / 268.65 MiB |
| OpenCV | 1,506 | 4,630,994,774 | 586,099 | 7.714 | 14.813 | 96.39% | 184.16 / 267.65 MiB |

The LLVM median PMU row was approximately 0.380 cycles/byte cold and 0.214
cycles/byte hot.  A separate exact, full-corpus line-only run measured 0.788
GB/s cold and 0.800 GB/s hot.  Thus the result above is a span-replay result,
not a claim that a generic line lookup itself runs at 10 GB/s.

## Exact-output gates

The exact reconstruction pass succeeded for all 4,755 TUs and all emitted
occurrences:

| Corpus | Reconstructed occurrences | Result |
| --- | ---: | --- |
| LLVM | 137,913,644 | PASS |
| RocksDB | 110,485,751 | PASS |
| DuckDB | 69,094,918 | PASS |
| Abseil | 90,359,789 | PASS |
| OpenCV | 159,014,620 | PASS |

On LLVM, the built-in edit case inserted one line after the common
`stdc-predef.h` marker in each TU.  It reconstructed 137,914,882 occurrences
exactly and measured 13.784 GB/s.  This is a focused next-marker
resynchronization check: the inserted bytes are identical in every TU, so it
does not replace a corpus produced by actually rebuilding after varied header
edits.

The selector-alias check completed exact reconstruction and the edit case for
one TU.  The address/undefined-operation instrumented build completed the
20-TU trace and edit cases with exact reconstruction.  Forcing equal selectors
over a large corpus intentionally exposes linear probing in this prototype;
production code needs a probe-depth threshold that promotes such buckets to a
stronger discriminator.

## Reproduction material

Corpus archives on the benchmark host:

| Archive | SHA-256 |
| --- | --- |
| `corpus.tar.zst` | `fa2ffe3c9494cf91f5a11dae51014c8af1a68657d52fbf5cb34d5ccd3d762de0` |
| `corpus2.tar.zst` | `f148559b191d7bd40375a8fed2a75a18fa47bdfcf003d94b82b04fa25ea610d3` |
| `corpus3.tar.zst` | `107b205a8540fb93589a090c242f277f1f6f6423f9c5fd8e21c2850a6586a0f3` |
| `corpus4.tar.zst` | `7452a675d79653c40ec2b62740a9efca1597808b116c38b33291b87ce4823a12` |
| `corpus5.tar.zst` | `a3f4e54bc6cd5554d1fffaf773b2dd43685577d040e7317e666359efab742ca0` |

The raw logs remain at `/home/ttuser/local-oracle-bakeoff/` on the benchmark
host:

| Log | SHA-256 |
| --- | --- |
| `final-all-corpora-runs.log` | `3e413901b9fe2267e7e8c5289958cfae69e38cc03cf97f1519544ac6199a4669` |
| `final-validation.log` | `d85b79f345fe93f2e4ac749272e23fc6641579cad998b4ca4486ec9820c64c8e` |
| `final-fallback-and-stress.log` | `ab3a11c3ef3175ab50ef9e59992e6084092556d7582b7122b30095c17bdafa0b` |

The release binary used for the table has SHA-256
`60742238117f26131c8c89e9a1a85e85a08af828ea16308a2a6e0d0ed68286cc`.

## What this permits, and what remains unproved

The result establishes enough single-thread headroom to try a simple
single-owner encoder path before making sharding mandatory.  It does not show
that the fused production path reaches 10 GB/s.  The next benchmark must add,
one cost at a time:

1. persistent-ID to per-file dense-ID translation and first-appearance tables;
2. exact `USED_KEYS`, value map, inline text, and occurrence-body construction;
3. whole-message zstd compression and decoding;
4. the C-side owner boundary and F-side per-C cache lookup;
5. socket transfer and streaming expansion into the compiler pipe.

Report both effective raw-byte throughput and encoded bytes, CPU cycles, peak
resident memory excluding the input corpus, and latency per TU.  If the fused
single owner remains above 10 GB/s with concurrent clients, keep it.  Add
shards only when measured owner saturation, tail latency, or working-set size
requires them.

This prototype also has deliberate benchmark limits: fixed-capacity line
tables, `u32` arena offsets, append-only storage, one-level marker spans, and
an input corpus held wholly in memory.  Marker-poor inputs fall back to a large
whole-TU span after a miss.  A production trace layer should use bounded
line-aligned child spans so local changes have bounded recovery work on those
inputs.
