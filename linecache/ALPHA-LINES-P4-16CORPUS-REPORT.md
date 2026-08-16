# P4 alpha-Line residual coding: complete 16-corpus rejection ledger

## Decision

Do **not** add this P4 alpha-normalized whole-Line codec to the product path.

The complete cold 16-corpus execution is exact, but it fails both continuation gates:

- Against an independent ordinary residual frame, actual per-TU selection saves only **457,673
  bytes**, far below the required 5,000,000-byte balanced-corpus continuation line.
- Against the current P26 executable, the complete wire grows by **3,524,464 bytes**, from
  97,021,934 to 100,546,398 bytes. The weighted ratio falls from **294.31x to 283.99x**.
- A serial full-Godot run on the intended Zen 4 host reaches only **0.423 GB/s** at C while
  evaluating the two alpha variants, below the 1 GB/s complete-path floor.

This result closes the ruled P4 question at the correct integration boundary. It does not reject
alpha normalization as a generic definition codec; it shows that alpha coding does not repay its
control and framing after P24/P26 have already removed public Lines, markers, byte arrays, source
copies, and source-patch overlap from the residual stream.

## Exact representation executed

Only the final whole-Line `RAW_RUN` branch of P24 is eligible. Source-patch middle bytes remain
literal gaps and are never parsed as template input.

For each TU, C builds two bounded candidates:

1. C++ keywords remain exact rule literals.
2. C++ keywords are parameterized like other identifiers.

Both candidates use:

- exact token/gap skeletons;
- first-local-occurrence identifier numbering and repeated-slot equality;
- identifier, number, and quoted-token slot types;
- rules and value lexicons scoped to one complete TU/FILL only;
- columnar instances grouped by exact normalized shape;
- exact literal fallback for every non-winning group;
- explicit local ordinals so F restores original Line order;
- an interleave vector for untouched source-patch gaps;
- separate zstd-3 control and data frames.

F receives only the selector and chosen frame set, reconstructs every local Line by ordinal,
interleaves the literal gap bytes, and produces the exact concatenated `mixedRaw[1]` stream already
consumed by the Region program. No Region opcode, object identity, blob codec, or final replay rule
changes.

The actual per-TU selector compares the complete framed sizes of:

```text
ordinary:  [zstd-3 exact residual frame]

alpha:     [zstd-3 control frame]
           [zstd-3 data frame, omitted when empty]

wire:      [one-byte mode selector] + smaller complete candidate
```

Thus an alpha candidate cannot enlarge an individual TU relative to the independent ordinary-frame
control. The aggregate regression relative to P26 comes from replacing P26's persistent flushed
residual stream with independently selectable TU frames.

## Complete cold result

```text
raw bytes                                      28,554,671,510
TUs                                                     9,292
exact corpora                                             16/16

P26 complete wire                                  97,021,934
P26 weighted ratio                                    294.31x

independent-ordinary complete wire                101,004,071
P4 selected complete wire                         100,546,398
P4 saving vs independent ordinary                     457,673
P4 regression vs P26                                3,524,464
P4 weighted ratio                                     283.99x

cold-400 maximum wire                              71,386,678.775
P26 gap to cold-400                                25,635,255.225
P4 gap to cold-400                                 29,159,719.225
```

The selector saw 8,065 residual-bearing TUs. Alpha won on only **154** (1.91%); ordinary won on
7,911. Of the alpha wins, keyword-literal normalization won 127 TUs and keyword-parameterized
normalization won 27.

The residual input comprised 290,690,697 raw bytes in 3,914,787 eligible Lines plus 2,470,750
literal source-patch gap bytes. Before ordinary fallback, the per-TU-best alpha-only representation
cost 53,134,705 framed bytes versus 45,147,025 for independent ordinary frames: alpha itself was
7,987,680 bytes larger. The selector salvaged isolated local wins rather than revealing a generally
better representation.

| corpus | P26 | P26+P4 | delta | saved vs independent | alpha-selected TUs |
|---|---:|---:|---:|---:|---:|
| abseil | 5,864,879 | 6,114,345 | +249,466 | 15,806 | 9 |
| catch2 | 1,006,074 | 1,080,168 | +74,094 | 87 | 1 |
| cereal | 512,588 | 533,424 | +20,836 | 0 | 0 |
| duckdb | 9,590,734 | 9,826,832 | +236,098 | 26,602 | 11 |
| eigen | 1,378,045 | 1,457,074 | +79,029 | 0 | 0 |
| fmt | 1,046,398 | 1,064,642 | +18,244 | 38 | 1 |
| godot | 44,731,020 | 46,002,368 | +1,271,348 | 223,342 | 61 |
| leveldb | 689,713 | 711,260 | +21,547 | 0 | 0 |
| llvm | 9,330,294 | 9,817,012 | +486,718 | 7,392 | 8 |
| nlohmann-json | 1,168,818 | 1,223,193 | +54,375 | 1,936 | 4 |
| opencv | 8,567,003 | 9,154,197 | +587,194 | 179,401 | 48 |
| range-v3 | 874,465 | 938,562 | +64,097 | 1,460 | 6 |
| re2 | 488,243 | 502,801 | +14,558 | 90 | 1 |
| rocksdb | 9,825,414 | 10,128,108 | +302,694 | 564 | 3 |
| simdjson | 1,408,733 | 1,437,600 | +28,867 | 955 | 1 |
| spdlog | 539,513 | 554,812 | +15,299 | 0 | 0 |

Godot and OpenCV account for 88.0% of the selector's 457,673-byte saving, yet both complete corpus
totals regress materially relative to P26. The result is therefore not hidden by equal-corpus
weighting or a DuckDB-heavy aggregate.

## Why the earlier standalone P4 result does not transfer

The earlier definition-plane experiment compared P4 with literal serialization of all first-seen
Line definitions. On DuckDB it reduced 11,060,591 bytes to 9,182,120 bytes. The live P24 residual is
a substantially harder and smaller population:

- repeated public Lines have already become references;
- preprocessor markers and paths are factored;
- generated byte arrays and P26 compressed members are on separate paths;
- exact source lines and profitable source patches have already been removed;
- the remaining literal bytes are already concatenated into a zstd-friendly syntax stream.

P4 must then pay for TU-local rule definitions, lexicon entries, slot control, restoration ordinals,
and a second frame. Those costs exceed the remaining alpha redundancy on almost every TU. This is
why projecting the standalone 17% definition-plane saving onto P26 would have been wrong; the
integrated execution measures the actual incremental value.

## Throughput and resource evidence

The final Godot screen was run alone on `tt-quietbox` (AMD EPYC 8124P, Zen 4), pinned to CPUs 0-15:

```text
raw                                      5,932,762,185 bytes
TUs                                                  2,207
exact                                                   yes
C encode                                             0.423 GB/s
F decode                                             1.224 GB/s
complete pipeline minimum                            0.423 GB/s
peak capability-harness RSS                          7.48 GiB
```

The matching P26 member-NEED path previously measured 1.095-1.109 GB/s at C. P4's C number includes
both exact normalization variants because actual selection was part of the ruled candidate. A
single optimized variant could be faster, but the balanced byte saving misses its continuation gate
by 4,542,327 bytes, so a throughput optimization pass is not warranted.

The other fifteen corpus jobs were run concurrently only to close the byte ledger; their timing
fields are retained for completeness but are not used as throughput evidence.

## Validation and reproducibility

- Warning-clean C++17 release build with `-O3 -DNDEBUG -march=native -Wall -Wextra -Wpedantic
  -Werror`.
- Complete cold chronological matrix: 16/16 corpora and 9,292/9,292 TUs reconstruct exactly.
- Decoder rebuilds the residual from the selected compressed frames and local selector, then the
  existing independent Region decoder rebuilds each complete `.ii` byte stream.
- Deterministic JSON/TSV generation records every input log digest.

Build:

```sh
g++ -O3 -DNDEBUG -march=native -std=c++17 -DICE_LINE_CAP_LOG2=23 \
  -Wall -Wextra -Wpedantic -Werror linecache/codec50.cpp \
  -o /tmp/codec50-alpha -lzstd -lz -pthread
```

Run:

```sh
/tmp/codec50-alpha --manifest MANIFEST --z 3 \
  --mixed-regions --byte-array-lines --direct-ordinals \
  --compressed-blobs --blob-threads 8 --blob-lazy-fallback \
  --alpha-lines
```

Regenerate the machine ledger:

```sh
python3 linecache/summarize_alpha_lines.py \
  --baseline-json linecache/ml-artifacts/compressed-blob-p26-16corpus-summary.json \
  --logs /tmp/issue16-alpha-cold-screen1 \
  --json linecache/ml-artifacts/alpha-lines-p4-16corpus-summary.json \
  --tsv linecache/ml-artifacts/alpha-lines-p4-16corpus.tsv
```

Retained logs:

- local copy: `/tmp/issue16-alpha-cold-screen1`
- Zen 4 host: `/home/ttuser/local-oracle-bakeoff/alpha-lines/alpha-cold-screen1`

## Next research move

The 25.64 MB P26 cold gap is not in this residual definition family. Further work should return to
the Region/root occurrence sequence: online, causal superblocks that replace repeated Region-id
programs while retaining exact exception replay. That work must report chronological learning
curves, reordered-TU runs, and controlled source perturbations; a static or TU-local residual
template bank cannot establish the required online-learning behavior.
