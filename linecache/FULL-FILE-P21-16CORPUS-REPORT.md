# P9/P21 Line coding in the full-file CODEC-50 reconstruction

## Decision

Keep P21 `BYTE_ARRAY` as one optional Line-definition representation.  In the integrated
full-file reconstruction it reduces the balanced 16-corpus cold ledger from **146,624,393** to
**118,901,432 bytes**, a saving of **27,722,961 bytes**, while reconstructing all 9,292 translation
units exactly.  The byte-weighted ratio rises from **194.75x to 240.15x**.

P21 is a substantial block, but it is not cold-400 closure.  The measured empty-receiver allowance
is 71,386,679 bytes, leaving **47,514,753 bytes** to remove before key association, a nonempty F,
and the actual C/F dispatcher are charged.  The next candidate must change the Line/Region
materialization boundary; another small Root or initializer variation cannot close that gap.

## Exact scope

The harness proves an exact full-file reconstruction within one chronological, empty-receiver
dense-ID conversation.  Each run charges and decodes:

1. newly required Line definitions;
2. Region-to-Line composition translated into F-local Line IDs;
3. Root and reusable Block programs;
4. missing Region/Block lists;
5. path objects where the baseline uses them; and
6. component selectors and framing used by this harness.

F builds its own Line, Region, Block, and Root stores from decoded data and expands every complete
`.ii`.  All 48 corpus runs (16 S1, 16 P9, and 16 P21) compare byte-for-byte with their inputs.

This is deliberately not labelled total protocol acceptance.  The remaining acceptance blocks
are:

- generation-key and 64-to-32 association bytes for nonempty and half-cold F state;
- the actual C/F socket framing, dispatch, and lifecycle;
- an executed complementary half-cold object cache; and
- reorder/change/revert and multi-F runs over that complete state machine.

Persistent zstd streams are also an experiment-local ordered conversation.  A product form must
either bind each stream to an ordered C/F lane or select independently decodable per-TU frames and
measure their extra bytes.  The current result must not be used as proof of arbitrary packet-order
decoding.

## Integrated P21 algorithm

For each TU, C first discovers the Regions absent from the empty F mirror and collects every Line
needed to define them.

1. Sort the new exact Lines lexicographically.
2. Assign F-local dense Line IDs in that same order.  C records its private-Line-ID to F-ID mapping;
   F derives the IDs from decode order, so no permutation vector is omitted.
3. Classify each Line with the strict P21 parser.  A matching `BYTE_ARRAY` has an exact
   `(prefix, separator, suffix, number_format, u8 values)` program.  The format is canonical decimal
   or exact two-digit hexadecimal with prefix and digit case preserved.  Nonmatching Lines use the
   ordinary sorted split-front form.
4. Encode ordinary Lines into three streams: common-prefix length, suffix length, and suffix bytes.
   Encode generated arrays into a style/control stream and a raw-u8 value stream.
5. Compress each active stream with its own persistent zstd-3 C context, flush it at the TU
   boundary, and charge its length frame.  Charge the P21 selector/presence byte.
6. F decompresses with independent contexts.  It reconstructs ordinary Lines, renders array Lines,
   merges the two already-sorted sets, and assigns the same dense IDs.
7. C rewrites every Region composition through the C-ID-to-F-ID vector.  F independently decodes
   and installs that composition before expanding the Root.
8. F emits the full TU through its Line spans; the harness compares it with the original bytes.

The optimized decoder stores reconstructed Lines in packed byte arrays plus offsets.  It merges
spans instead of allocating and copying millions of small vectors.  Decimal rendering emits one to
three digits directly instead of calling formatted I/O for each of the 38.4 million generated-array
values in Godot.  These changes leave every serialized byte unchanged.

## Aggregate wire ledger

| category | S1 | P9 sorted split-front | P21 `BYTE_ARRAY` |
|---|---:|---:|---:|
| Root | 3,687,803 | 3,687,803 | 3,687,803 |
| Line definitions | 118,780,613 | 103,390,706 | **86,988,259** |
| Region-to-Line definitions | 19,338,068 | 24,235,561 | 24,235,561 |
| Block definitions | 428,568 | 428,568 | 428,568 |
| path definitions | 828,100 | 0 | 0 |
| missing lists | 3,488,397 | 3,488,397 | 3,488,397 |
| framing | 72,844 | 72,844 | 72,844 |
| **total** | **146,624,393** | **135,303,879** | **118,901,432** |
| byte-weighted ratio | 194.75x | 211.04x | **240.15x** |
| equal-corpus harmonic ratio | 197.99x | 222.11x | **232.16x** |
| gap to cold-400 | 75,237,714 | 63,917,200 | **47,514,753** |
| minimum transform/expand pipeline proxy | 1.25 GB/s | 1.08 GB/s | **1.10 GB/s** |

P9 removes 15,389,907 Line bytes but changes the lexicographic ID assignment.  That makes the
Region-composition stream **4,897,493 bytes larger**.  Removing the baseline's separate marker-path
objects recovers 828,100 bytes, for a net P9 saving of 11,320,514 bytes.  This is why substituting an
independent Line-only result would have overstated the integrated gain.

P21 then removes another **16,402,447 bytes**, entirely from the Line leg.  Its integrated Line wire
is 122,659 bytes above the earlier independent 86,865,600-byte capability because this C++ form has
its own exact stream layout, selectors, and frame boundaries.  The integrated number is the one to
use in the objective ledger.

## Per-corpus result

| corpus | S1 wire | P9 wire | P21 wire | P21 ratio |
|---|---:|---:|---:|---:|
| LLVM | 11,908,351 | 10,265,567 | **10,160,230** | 356.32x |
| RocksDB | 11,390,768 | **10,042,861** | 10,043,263 | 310.09x |
| DuckDB | 12,442,305 | 11,343,131 | **10,374,004** | 191.41x |
| Abseil | 7,072,905 | 6,179,175 | **6,175,917** | 417.92x |
| OpenCV | 10,811,760 | 8,984,047 | **8,981,229** | 515.63x |
| Godot | 79,906,920 | 77,014,967 | **61,694,666** | 96.16x |
| fmt | 1,388,886 | 1,245,100 | **1,244,155** | 109.59x |
| spdlog | 857,116 | 747,577 | **746,571** | 131.78x |
| Catch2 | 1,385,816 | **1,197,539** | 1,197,783 | 790.84x |
| nlohmann/json | 1,601,207 | 1,382,004 | **1,381,972** | 212.68x |
| range-v3 | 1,303,599 | **1,097,867** | 1,098,147 | 575.56x |
| Eigen | 2,017,939 | **1,842,590** | 1,843,320 | 1,916.26x |
| RE2 | 766,284 | **662,952** | 663,005 | 166.26x |
| LevelDB | 1,002,755 | **885,220** | 885,371 | 162.50x |
| simdjson | 1,920,187 | 1,683,053 | **1,682,048** | 278.46x |
| cereal | 847,595 | 730,229 | **729,751** | 447.96x |

P21's large balanced win is concentrated in Godot and DuckDB, where exact generated byte arrays
exist.  Six smaller corpora regress against P9 by a combined 1,860 bytes because the persistent
P21 streams are maintained even when that TU has little useful array material.  A product selector
may remove those small losses, but it is not the next factor-sized experiment.

## Throughput and memory

The first correct renderer used one vector per decoded generated Line and formatted every decimal
value separately.  It reconstructed exactly but measured 0.69 GB/s on fmt and 0.75 GB/s on Godot.
Packed spans and direct digit emission changed the all-corpus minimum to **1.10 GB/s**; Godot now
measures 1.10 GB/s C encode and 1.15 GB/s F decode, for a 1.10 GB/s pipeline proxy.  The refreshed
16/16 P21 matrix is exact and has the identical category ledger above.

This rate covers the P21 transform, compression, decode, object installation, and full Root
expansion inside the harness.  It excludes corpus loading/pre-interning and the real socket path, so
it proves the codec capability gate, not the final product throughput gate.

Maximum process RSS is 7.34 GiB on Godot because the research harness preloads 5.93 GB and retains
the complete C and F history in one process.  It is not a proposed daemon allocation.  Bounded
generation-local stores and the executed half-cold scenario remain necessary.

## Residual and next factor-sized candidate

P21 still spends 86.99 MB on Lines and 24.24 MB on Region composition.  Even eliminating all Region
composition would leave about 94.67 MB, still 23.28 MB over cold-400.  The independent P21 component
audit attributes about 38.66 MB to actual generated-array values and 40.46 MB to remaining suffix
bytes.  The next candidate therefore must avoid publishing one-use Lines as universal objects and
must remove their separate Region composition at the same time.

The next exact experiment is a causal mixed Region materializer:

- F stores the exact raw bytes of every completed Region.
- Already-public repeated Lines may be referenced by dense ID.
- A first-seen private Line is literal within its Region, without forcing a separate public Line
  definition.
- If that Line is reused later, C publishes a Line view by naming the completed source Region plus
  byte offset and length; F installs the view without retransmitting the bytes.
- A Region program may mix literal spans, public-Line references, and prior-Region views, with an
  actual-byte fallback and independent exact replay.

This is causal encode-before-learn behavior and keeps the blocks independent: raw Region storage,
optional Line views, and ordinary Root expansion.  It targets both remaining large controllable
legs rather than adding another model family.

## Reproduction

Build:

```text
g++ -O3 -march=native -std=c++17 -DICE_LINE_CAP_LOG2=23 \
  linecache/codec50.cpp -o /tmp/issue16-codec50-p21 -lzstd
```

Run each of `/tanksmall/scratch/ictmp/corpus{,2..16}/manifest.txt` with:

```text
/tmp/issue16-codec50-p21 --manifest MANIFEST --z 3 --byte-array-lines
```

Retain one log per corpus and summarize with:

```text
python3 linecache/summarize_complete_line_codec.py \
  --s1-dir /tmp/issue16-complete-baseline-z3 \
  --p9-dir /tmp/issue16-complete-p9-final-z3 \
  --p21-dir /tmp/issue16-complete-p21-final-z3 \
  --output linecache/ml-artifacts/complete-line-codec-16corpus-summary.json \
  --tsv linecache/ml-artifacts/complete-line-codec-16corpus.tsv
```

The JSON records every retained log's SHA-256, exactness, category ledger, transform rates, and peak
RSS.  The TSV contains all per-corpus comparisons.
