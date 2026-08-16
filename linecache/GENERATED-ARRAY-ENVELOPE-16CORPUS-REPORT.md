# P21: exact byte-array envelope/radix extension across 16 corpora

## Verdict

Keep this as a small extension of the existing `BYTE_ARRAY` Line operation. It recognizes exact
decimal and two-digit hexadecimal byte initializers whose declaration or fragment envelope is part
of the style. It does **not** justify another operation, transport block, model, or cache.

Against the strict P20 decimal-row control, complete zstd-3 stateful Line wire falls from
`87,955,340` to
`86,865,600` bytes: an additional
`1,089,740` bytes or
`1.24%`. DuckDB supplies most of that refinement; the
all-corpus result is retained because the same exact representation also captures LLVM and Godot
without corpus-specific rules.

This still does not close the objective. With the current structural ledger, projected cold wire is
`133,555,793` bytes, or
`200.61x` equal-corpus and
`213.80x` byte-weighted. The byte-weighted 400x allowance is
`71,386,679` bytes, leaving `62,169,114` bytes of measured excess. The half-cold
row remains arithmetic rather than an executed workload.

## Exact extension

The strict P20 form remains valid. P21 additionally accepts a whole Line only when it can be
represented exactly as:

```text
BYTE_ARRAY(prefix, separator, suffix, number-format, u8-values)

number-format := canonical-decimal
               | 0x + exactly-two lower-case hexadecimal digits
               | 0x + exactly-two upper-case hexadecimal digits
               | 0X + exactly-two lower-case hexadecimal digits
               | 0X + exactly-two upper-case hexadecimal digits
```

There must be at least four values, every value must be in `[0,255]`, the number format and separator
must be constant within the Line, and the renderer must reproduce the input byte for byte. Supported
envelopes are deliberately limited to:

- whitespace-prefixed numeric rows;
- continuation fragments beginning with a comma;
- declarations containing `uint8_t` or `unsigned char` before the first opening brace;
- LF, closing-brace, and closing-brace/semicolon endings, with an optional final comma.

The style table stores the exact prefix, comma/whitespace separator, suffix, and radix/case code.
The value stream stores one byte per value. All nonmatching Lines use the ordinary split-front form.
The receiver independently decodes both forms, renders the exact Lines, merges their lexicographic
orders, and installs the normal Line representation.

P20 recognized `1,476,915` Lines containing
`175,287,807` rendered bytes and
`38,471,795` values. P21 recognizes
`1,569,025` Lines containing
`193,479,938` rendered bytes and
`42,113,133` values.

## Complete balanced measurements

| level | mode | Line wire | raw/wire | equal-corpus | exact |
|---|---|---|---|---|---|
| zstd-1 | independent-best | 94,837,543 | 301.09x | 314.68x | yes |
| zstd-1 | stream-split-front | 108,118,845 | 264.10x | 306.05x | yes |
| zstd-1 | stream-generated-array | 91,252,554 | 312.92x | 325.70x | yes |
| zstd-3 | independent-best | 91,361,256 | 312.55x | 337.06x | yes |
| zstd-3 | stream-split-front | 103,210,181 | 276.67x | 331.22x | yes |
| zstd-3 | stream-generated-array | 86,865,600 | 328.72x | 354.03x | yes |

The independent row selects the smaller complete serialized representation separately for each TU.
The two stream rows retain zstd state across TUs and remain research ceilings. Every nonempty stream
is charged a four-byte length; every generated frame is charged a selector byte.

## Increment beyond strict P20

| corpus | recognized text | P20 Line | P21 Line | P21 saving | cold | half-cold* |
|---|---|---|---|---|---|---|
| duckdb | 16,331,125 | 7,828,026 | 6,914,109 | +913,917 | 163.34x | 228.24x |
| llvm | 1,883,929 | 7,464,814 | 7,362,680 | +102,134 | 335.36x | 508.91x |
| godot | 175,147,421 | 51,870,999 | 51,805,645 | +65,354 | 103.99x | 190.46x |
| abseil | 31,621 | 3,142,117 | 3,138,190 | +3,927 | 242.37x | 284.25x |
| opencv | 42,418 | 6,382,755 | 6,380,541 | +2,214 | 413.43x | 578.08x |
| spdlog | 7,998 | 513,142 | 512,060 | +1,082 | 124.98x | 185.22x |
| simdjson | 12,104 | 1,045,053 | 1,044,406 | +647 | 296.91x | 443.83x |
| cereal | 2,709 | 501,351 | 500,995 | +356 | 500.98x | 813.14x |
| fmt | 7,998 | 699,336 | 699,008 | +328 | 83.73x | 106.61x |
| rocksdb | 1,628 | 4,178,911 | 4,178,690 | +221 | 166.91x | 187.96x |
| range-v3 | 145 | 720,599 | 720,583 | +16 | 470.98x | 643.84x |
| catch2 | 0 | 691,243 | 691,243 | +0 | 566.15x | 713.55x |
| re2 | 0 | 453,470 | 453,470 | +0 | 158.14x | 234.39x |
| leveldb | 365 | 539,092 | 539,172 | -80 | 128.83x | 169.83x |
| nlohmann-json | 10,233 | 846,396 | 846,478 | -82 | 217.94x | 317.62x |
| eigen | 244 | 1,078,036 | 1,078,330 | -294 | 1591.84x | 2102.77x |

`*` Half-cold is the budget projection `structure + 0.5 × cold Line wire`, not an executed cache
scenario. Positive P21 saving means the extension reduced wire. Three tiny corpora regress by a
combined 456 bytes in the persistent ceiling; an actual-byte per-TU fallback remains authoritative.

The extension's `1,089,740`-byte net gain is concentrated as follows:
DuckDB saves 913,917 bytes, LLVM 102,134, Godot 65,354, and the remaining thirteen corpora together
save 8,335 bytes. This concentration is why the extension is a finishing refinement rather than the
next factor-sized direction.

## zstd-3 generated-stream ledger

| component | wire bytes | fraction |
|---|---|---|
| rest_lcp | 3,315,290 | 3.82% |
| rest_length | 3,865,565 | 4.45% |
| rest_suffix | 40,455,496 | 46.57% |
| array_control | 556,961 | 0.64% |
| array_values | 38,663,896 | 44.51% |
| selectors | 8,392 | 0.01% |

Relative to P20, more initializer text moves out of `rest_suffix`, but its underlying values move
into `array_values`. That is the expected accounting: spelling and punctuation disappear while the
actual byte content remains. The remaining suffix and value streams are still the dominant Line
blocks.

## Integrated objective ledger

| quantity | result |
|---|---:|
| raw `.ii` input | 28,554,671,510 B |
| structural wire | 46,690,193 B |
| P21 Line wire | 86,865,600 B |
| projected cold wire | 133,555,793 B |
| cold ratio, byte-weighted / equal-corpus | 213.80x / 200.61x |
| corpora at cold 400x | 5 / 16 |
| projected half-cold wire | 90,122,993 B |
| half-cold ratio, byte-weighted / equal-corpus | 316.84x / 279.92x |
| corpora at half-cold 200x | 11 / 16 |

## Correctness and performance boundary

- All 96 rows are exact: 16 corpora × zstd-1/zstd-3 × three modes.
- Accepted and rejected syntax cases exercise decimal, hexadecimal, declaration, fragment, mixed
  spelling, overflow, and fallback behavior.
- Both winning and losing independent candidates are decompressed and reconstructed.
- Persistent streams are finished, independently replayed, and checked against trace truth.
- Component bytes sum exactly to reported wire.
- Report, JSON summary, and TSV are generated from the complete machine artifacts.

The Python classifier is research code, not the product throughput result. The complete harness took
minutes and therefore does not satisfy the required 1 GB/s product path. The retained benchmark JSON
contains per-corpus classification, compression, and reconstruction timings; the product decision
still requires the C++ scanner/renderer on the complete C/F data path.

## Decision and next work

1. Fold radix and exact envelope style into the one `BYTE_ARRAY` operation; do not add an
   `INTEGER_ARRAY` family.
2. Preserve actual-byte fallback and ordinary Line installation.
3. Do not spend another bake-off round widening initializer syntax unless a residual census predicts
   a factor-sized gain. This extension found only another 1.09 MB.
4. Move the main search back to the factor-sized structural and rest-suffix blocks, while treating
   the byte-value stream as explicit payload until a measured cross-TU reuse operation removes it.
5. Implement the winning narrow operation in C++ only after the architecture boundary is settled,
   then measure at least 1 GB/s end to end.
6. Replace the half-cold projection with an executed scenario and rerun standard, reverse, fixed
   shuffle, broad change, and revert workloads with encode-before-learn state.

## Reproduction

- P21 codec input: `linecache/ml-artifacts/generated-array-envelope-line-program-16corpus-benchmark.json`
- P21 codec SHA-256: `9985c4e6f90eddbb0f0b32ead40002209eac6fa607dea96ffba20df55a4ae301`
- P20 control input: `linecache/ml-artifacts/generated-array-line-program-16corpus-benchmark.json`
- P20 control SHA-256: `b3dd61c0ad3dedbd14fce99071d8541b4e397b2a9bd4314afe8901d4c08b5095`
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
  --array-syntax extended \
  --output linecache/ml-artifacts/generated-array-envelope-line-program-16corpus-benchmark.json \
  --tsv linecache/ml-artifacts/generated-array-envelope-line-program-16corpus-benchmark.tsv

python3 linecache/generated_array_report.py \
  --codec-report linecache/ml-artifacts/generated-array-envelope-line-program-16corpus-benchmark.json \
  --prior-codec-report linecache/ml-artifacts/generated-array-line-program-16corpus-benchmark.json \
  --structural-summary linecache/ml-artifacts/cross-context-hybrid-16corpus-summary.json \
  --summary linecache/ml-artifacts/generated-array-envelope-line-program-16corpus-summary.json \
  --tsv linecache/ml-artifacts/generated-array-envelope-line-program-16corpus.tsv \
  --output linecache/GENERATED-ARRAY-ENVELOPE-16CORPUS-REPORT.md
```
