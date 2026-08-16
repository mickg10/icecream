# P20: exact generated-byte-array Line program across 16 corpora

## Verdict

Keep one narrow `BYTE_ARRAY` operation in the Line-definition candidate set. Across all 16 balanced
corpora at zstd-3, it reduces the stateful split-front Line plane from
`103,210,181` to
`87,955,340` bytes: a
`15,254,841`-byte or
`14.78%` saving. Every candidate and fallback reconstructs
the exact original Lines.

This is a substantial finishing operation, not the missing factor of two. Combining it with the
current `46,690,193`-byte best structural ledger yields only
`199.32x` equal-corpus and
`212.07x` byte-weighted cold projections. The arithmetic
half-cold projection remains above 200x in aggregate, but only
`11 / 16` individual corpora pass. A real cache scenario is
still required; halving Line wire is not evidence of such a scenario.

## Exact operation

The encoder recognizes only Lines matching this conservative grammar:

```text
horizontal-prefix decimal-u8 (comma fixed-horizontal-separator decimal-u8){3,} comma LF
```

Decimal spelling is canonical, every value is in `[0,255]`, every value has a trailing comma, and
the separator is identical within the Line. A frame carries:

```text
BYTE_ARRAY(style table, style-id stream, values-per-Line stream, raw-u8 value stream)
```

All other Lines use the existing lexicographically sorted split-front streams. The receiver renders
decimal digits, commas, exact prefix/separator bytes, and LF, merges the two sorted outputs, and
compares the result with the independent truth set. A one-byte frame selector can encode baseline
versus generated form plus the stream-presence mask. No new message family or predictor is needed.

## Balanced measurements

| level | mode | Line wire | raw/wire | equal-corpus | exact |
|---|---|---|---|---|---|
| zstd-1 | independent-best | 95,789,898 | 298.10x | 311.87x | yes |
| zstd-1 | stream-split-front | 108,118,845 | 264.10x | 306.05x | yes |
| zstd-1 | stream-generated-array | 92,209,572 | 309.67x | 322.70x | yes |
| zstd-3 | independent-best | 92,415,505 | 308.98x | 333.51x | yes |
| zstd-3 | stream-split-front | 103,210,181 | 276.67x | 331.22x | yes |
| zstd-3 | stream-generated-array | 87,955,340 | 324.65x | 350.03x | yes |

The independent row is the product-shaped actual-byte selector over complete per-TU zstd frames.
The two stream rows retain compression state across TUs and are ceilings comparable to the previous
Line report. Each nonempty stream is charged a four-byte length and every generated frame a selector.

The strict subset contains `1,476,915` Lines,
`175,287,807` rendered bytes, and
`38,471,795` underlying byte values.

## zstd-3 generated-stream ledger

| component | wire bytes | fraction |
|---|---|---|
| rest_lcp | 3,349,428 | 3.81% |
| rest_length | 3,900,845 | 4.44% |
| rest_suffix | 43,115,764 | 49.02% |
| array_control | 544,464 | 0.62% |
| array_values | 37,036,447 | 42.11% |
| selectors | 8,392 | 0.01% |

The `37,036,447`-byte value stream is the dominant irreducible-looking part of
this operation. Most of Godot's values are already-compressed editor translations, documentation,
fonts, or ICU data. Higher-level source reuse may avoid retransmitting some of them, but ordinary
text prediction should not be expected to manufacture those payload bytes.

## Integrated cold and half-cold projection

| quantity | result |
|---|---:|
| raw `.ii` input | 28,554,671,510 B |
| structural wire | 46,690,193 B |
| generated-array Line wire | 87,955,340 B |
| projected cold wire | 134,645,533 B |
| cold ratio, byte-weighted / equal-corpus | 212.07x / 199.32x |
| corpora at cold 400x | 5 / 16 |
| projected half-cold wire | 90,667,863 B |
| half-cold ratio, byte-weighted / equal-corpus | 314.94x / 278.66x |
| corpora at half-cold 200x | 11 / 16 |

| corpus | array text | old Line | new Line | saving | cold | half-cold* |
|---|---|---|---|---|---|---|
| fmt | 181 | 699,964 | 699,336 | +628 | 83.71x | 106.60x |
| godot | 174,431,128 | 67,067,667 | 51,870,999 | +15,196,668 | 103.87x | 190.26x |
| spdlog | 181 | 513,222 | 513,142 | +80 | 124.81x | 185.03x |
| leveldb | 0 | 539,025 | 539,092 | -67 | 128.84x | 169.84x |
| duckdb | 701,757 | 7,886,760 | 7,828,026 | +58,734 | 151.92x | 216.85x |
| re2 | 0 | 453,417 | 453,470 | -53 | 158.14x | 234.39x |
| rocksdb | 0 | 4,178,290 | 4,178,911 | -621 | 166.91x | 187.96x |
| nlohmann-json | 784 | 846,646 | 846,396 | +250 | 217.95x | 317.63x |
| abseil | 1,325 | 3,141,417 | 3,142,117 | -700 | 242.28x | 284.19x |
| simdjson | 8,764 | 1,045,296 | 1,045,053 | +243 | 296.79x | 443.69x |
| llvm | 113,328 | 7,465,094 | 7,464,814 | +280 | 332.22x | 505.29x |
| opencv | 28,124 | 6,383,300 | 6,382,755 | +545 | 413.35x | 578.00x |
| range-v3 | 75 | 720,302 | 720,599 | -297 | 470.98x | 643.84x |
| cereal | 2,160 | 501,387 | 501,351 | +36 | 500.71x | 812.78x |
| catch2 | 0 | 690,999 | 691,243 | -244 | 566.15x | 713.55x |
| eigen | 0 | 1,077,395 | 1,078,036 | -641 | 1592.05x | 2102.96x |

`*` Half-cold is the explicit arithmetic projection `structure + 0.5 × cold Line wire`; it is not
an executed cache workload.

Godot supplies almost the entire factor-sized gain: its Line wire falls from `67,067,667` to
`51,870,999` bytes. Small selector/history movements cause sub-kilobyte regressions in six corpora;
an actual-byte TU-local baseline remains available and the aggregate maximum regression is 700
bytes. The design must not special-case the corpus: the strict operation is content-defined and the
literal/front fallback remains authoritative.

## Correctness and speed boundary

- The parser and renderer have accepted/rejected unit cases.
- Every independent baseline, independent candidate, persistent baseline, and persistent candidate
  is decoded from serialized zstd bytes and compared with the exact trace Lines.
- The losing independent representation is also decoded, outside product decode timing, so fallback
  cannot conceal a broken candidate.
- All 96 balanced rows (16 corpora × 2 levels × 3 modes) are exact.
- Python compile, Ruff, and diff checks pass.

The Python research parser is intentionally not a throughput claim. On full Godot it prepares the
272.75 MB first-use Line set in roughly 41 seconds and therefore misses the product gate badly. The
zstd part itself processes the much smaller Line plane quickly, but a C++ parser/renderer must be
measured end to end. Product acceptance remains at least 1 GB/s of original `.ii` input on the real
C/F path, including classification, rendering, compression, framing, and copying.

## Decision and next work

1. Keep `BYTE_ARRAY` as one optional version-dispatched Line-definition representation with ordinary
   fallback; do not create a separate transport.
2. Materialize reconstructed Lines through the ordinary Line installation path for the first product
   row. This isolates the representation gain and preserves existing IDs/composition.
3. Implement the strict scanner and renderer in C++, then benchmark classification plus round trip on
   the complete 16-corpus trace at zstd-1 and zstd-3.
4. Execute actual cold and half-cold cache workloads. The half-cold projection above is useful budget
   arithmetic only.
5. Continue on the remaining 43.12 MB rest-suffix channel and the 46.69 MB structural channel. Flat
   byte phrases are already closed; the next contender must be a coarser parameterized program or a
   simpler structural ledger, always selected by complete serialized bytes.
6. Preserve the existing online learner's encode-before-learn state rule and rerun standard, reverse,
   deterministic shuffles, broad content change, and revert after the Line operation is integrated.

## Reproduction

- Codec input: `linecache/ml-artifacts/generated-array-line-program-16corpus-benchmark.json`
- Codec SHA-256: `b3dd61c0ad3dedbd14fce99071d8541b4e397b2a9bd4314afe8901d4c08b5095`
- Structural input: `linecache/ml-artifacts/cross-context-hybrid-16corpus-summary.json`
- Structural SHA-256: `e008691cb9f4d0e32c25adb2d7fd5ef07fdad6adb2976cd7a00fc8a90f799e3a`

```sh
python3 linecache/generated_array_line_codec.py \
  --trace abseil=linecache/traces/ml-abseil.bin \
  --trace catch2=linecache/traces/ml-catch2.bin \
  --trace cereal=linecache/traces/ml-cereal.bin \
  --trace duckdb=linecache/traces/ml-duckdb.bin \
  --trace eigen=linecache/traces/ml-eigen.bin \
  --trace fmt=linecache/traces/ml-fmt.bin \
  --trace godot=linecache/traces/ml-godot.bin \
  --trace leveldb=linecache/traces/ml-leveldb.bin \
  --trace llvm=linecache/traces/ml-llvm.bin \
  --trace nlohmann-json=linecache/traces/ml-nlohmann-json.bin \
  --trace opencv=linecache/traces/ml-opencv.bin \
  --trace range-v3=linecache/traces/ml-range-v3.bin \
  --trace re2=linecache/traces/ml-re2.bin \
  --trace rocksdb=linecache/traces/ml-rocksdb.bin \
  --trace simdjson=linecache/traces/ml-simdjson.bin \
  --trace spdlog=linecache/traces/ml-spdlog.bin \
  --levels 1 3 \
  --output linecache/ml-artifacts/generated-array-line-program-16corpus-benchmark.json \
  --tsv linecache/ml-artifacts/generated-array-line-program-16corpus-benchmark.tsv

python3 linecache/generated_array_report.py \
  --codec-report linecache/ml-artifacts/generated-array-line-program-16corpus-benchmark.json \
  --structural-summary linecache/ml-artifacts/cross-context-hybrid-16corpus-summary.json \
  --summary linecache/ml-artifacts/generated-array-line-program-16corpus-summary.json \
  --tsv linecache/ml-artifacts/generated-array-line-program-16corpus.tsv \
  --output linecache/GENERATED-ARRAY-LINE-PROGRAM-16CORPUS-REPORT.md
```
