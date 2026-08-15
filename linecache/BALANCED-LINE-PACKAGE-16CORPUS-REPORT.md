# Balanced exact-Line package and TU-local front-coding report

Issue: `mickg10/icecream#16`

## Outcome

The 16-corpus held-out experiment rejects a charged exact-Line package as a useful cold
bootstrap mechanism.  The best nonzero uniform package budget is statistically and
practically flat; larger packages lose.  This result holds at both zstd level 1 and level 3
and is not driven by one project.

The experiment also identifies a smaller general improvement worth carrying into the real
codec: within each TU, sort only that TU's newly discovered Lines, front-code each Line as
`LCP(previous) + suffix`, and choose the actual smaller framed representation against the
appearance-order literal frame.  This is causal because the complete current TU is known
before its definitions are sent.  It improves the Line-text leg on all 16 corpora.

Neither result closes the requested total cold target.  Combining the best measured
structural wire with the new Line-text wire gives an **optimistic**, incomplete cold ratio of
only **181.43x byte-weighted / 183.49x equal-corpus harmonic**.  The 462.92x structural score
must therefore never be presented as a total-codec result.

## Exact experiment

Source:

- `linecache/balanced_line_package_bench.cpp`
- `linecache/ml-artifacts/balanced-line-package-16corpus-z1.tsv`
- `linecache/ml-artifacts/balanced-line-package-16corpus-z3.tsv`

The exact full-set invocation is:

```sh
g++ -O3 -DNDEBUG -march=native -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  linecache/balanced_line_package_bench.cpp -lzstd \
  -o /tmp/balanced_line_package_bench

for level in 1 3; do
  /tmp/balanced_line_package_bench \
    --level "$level" \
    --out "linecache/ml-artifacts/balanced-line-package-16corpus-z${level}.tsv" \
    --trace llvm=linecache/traces/ml-llvm.bin \
    --trace rocksdb=linecache/traces/ml-rocksdb.bin \
    --trace duckdb=linecache/traces/ml-duckdb.bin \
    --trace abseil=linecache/traces/ml-abseil.bin \
    --trace opencv=linecache/traces/ml-opencv.bin \
    --trace godot=linecache/traces/ml-godot.bin \
    --trace fmt=linecache/traces/ml-fmt.bin \
    --trace spdlog=linecache/traces/ml-spdlog.bin \
    --trace catch2=linecache/traces/ml-catch2.bin \
    --trace nlohmann-json=linecache/traces/ml-nlohmann-json.bin \
    --trace range-v3=linecache/traces/ml-range-v3.bin \
    --trace eigen=linecache/traces/ml-eigen.bin \
    --trace re2=linecache/traces/ml-re2.bin \
    --trace leveldb=linecache/traces/ml-leveldb.bin \
    --trace simdjson=linecache/traces/ml-simdjson.bin \
    --trace cereal=linecache/traces/ml-cereal.bin
done
```

The input is the complete set of 16 `ICMLDS2` traces exported by `ml_bakeoff.cpp`: 9,292
TUs, 5,579,519 distinct union Lines, and 483.49 MiB of union Line text.  Every trace footer
is checked against the observed TU, raw-byte, Line-byte, event, and candidate counters.

For target corpus `T`:

1. Remove `T` from the training corpus mask.
2. Retain only exact Lines observed in at least one of the other 15 corpora.
3. Rank without target data by training-corpus document frequency, then Line length, then
   lexical bytes.
4. Fill each predetermined raw package budget: 0, 64, 128, 256, 512, 1024, 2048, and
   4096 KiB.
5. Lexically sort and front-code the package, serialize an actual frame, choose raw or
   zstd, and charge its four-byte length, representation byte, compression byte, and
   payload.
6. For each target TU, remove package hits and serialize the remaining first-use Line text.
   Compare two actual candidates: appearance-order literals and TU-local lexical
   front-coding.  Compress both, select the smaller bytes, and charge the complete frame.
7. An independent decoder parses every selected frame and compares every reconstructed
   Line with the authoritative trace bytes.

All **256 result rows** (16 corpora × 8 budgets × 2 levels) decode exactly.

This is intentionally a Line-text-leg experiment, not a total transport.  The trace gives
the exact first-use text stream, but this standalone benchmark does not charge the
Region-to-Line composition, Root/superblock stream, missing/key lists, or the final combined
message layout.  The complete protocol already has a key/map block that can bind definition
order to stable keys; integration must charge that block.  If that binding cannot be reused,
the complete codec must explicitly serialize and charge it.

## Charged held-out package result

Equal-corpus harmonic gain is `empty-package Line wire / package Line wire`.  A value below
1 enlarges the transfer.

| raw package budget | z1 harmonic gain | z1 wins | z3 harmonic gain | z3 wins | decision |
|---:|---:|---:|---:|---:|---|
| 0 KiB | 1.0000x | 0/16 | 1.0000x | 0/16 | baseline |
| 64 KiB | 0.9998x | 7/16 | 0.9999x | 5/16 | reject: flat |
| 128 KiB | 0.9995x | 3/16 | 0.9990x | 0/16 | reject |
| 256 KiB | 0.9920x | 0/16 | 0.9902x | 0/16 | reject |
| 512 KiB | 0.9884x | 0/16 | 0.9864x | 0/16 | reject |
| 1024 KiB | 0.9931x | 1/16 | 0.9905x | 0/16 | reject |
| 2048 KiB | 0.9631x | 1/16 | 0.9664x | 1/16 | reject |
| 4096 KiB | 0.7511x | 0/16 | 0.7634x | 0/16 | reject |

The 64 KiB raw package compresses to roughly 5 KiB, but removing its exact hits from the
target residual saves essentially the same amount.  The package is merely moving common
text into a separately charged frame.  At larger budgets it increasingly ships training
content the target never uses.

This does not prove that every possible static Line model loses.  It does show that an
exact verbatim-Line bank has no factor-sized opportunity here and should not be a default
protocol block.  Parameterized templates remain a different representation and must be
evaluated separately.

## TU-local front-coding result

The empty-package row isolates the front-coding transform.  It compares actual bytes for
each TU, so a TU keeps its literal frame whenever front-coding loses.

| metric | z1 | z3 |
|---|---:|---:|
| literal-only Line-text wire | 123,719,006 B | 119,565,894 B |
| best literal/front Line-text wire | 115,997,188 B | 110,692,790 B |
| bytes removed | 7,721,818 B | 8,873,104 B |
| byte-weighted wire gain | 1.06657x | **1.08016x** |
| equal-corpus harmonic gain | 1.08020x | **1.12218x** |
| equal-corpus median gain | 1.08016x | **1.12742x** |
| per-corpus minimum gain | 1.03074x | **1.05784x** |
| per-corpus maximum gain | 1.15761x | **1.18803x** |
| corpora improved | 16/16 | 16/16 |

At z3, front-coding wins 6,952 of 8,392 TUs that contain new Lines (82.84%).  The fallback
selects literal frames for the remaining 1,440 TUs.

This is a candidate for the complete codec, not yet an accepted product component.  Its next
gate is to run inside the real C/F serializer while charging the stable-key association and
measuring the complete path above 1 GB/s.

## Balanced per-corpus view at z3

`Optimistic combined` is `raw / (hybrid structural wire + front-coded Line-text wire)`.
It is deliberately labelled optimistic because Region composition and several final protocol
bytes are still absent.

| corpus | TU-front gain | 64 KiB package gain | structural wire MiB | Line-text wire MiB | optimistic combined |
|---|---:|---:|---:|---:|---:|
| abseil | 1.143x | 1.0000x | 7.16 | 3.40 | 233.05x |
| catch2 | 1.105x | 0.9997x | 0.94 | 0.77 | 530.50x |
| cereal | 1.137x | 0.9992x | 0.14 | 0.53 | 464.46x |
| DuckDB | 1.095x | 1.0000x | 5.00 | 8.07 | 144.93x |
| Eigen | 1.095x | 0.9999x | 1.09 | 1.15 | 1508.18x |
| fmt | 1.136x | 1.0003x | 0.89 | 0.72 | 80.83x |
| Godot | 1.058x | 1.0000x | 5.00 | 66.93 | 78.65x |
| LevelDB | 1.127x | 0.9999x | 0.55 | 0.57 | 122.67x |
| LLVM | 1.097x | 1.0000x | 3.27 | 8.07 | 304.38x |
| nlohmann-json | 1.127x | 0.9999x | 0.48 | 0.91 | 201.63x |
| OpenCV | 1.108x | 1.0000x | 4.60 | 7.12 | 376.91x |
| range-v3 | 1.127x | 1.0003x | 0.59 | 0.79 | 435.75x |
| RE2 | 1.139x | 0.9985x | 0.23 | 0.47 | 149.39x |
| RocksDB | 1.188x | 0.9997x | 13.81 | 4.46 | 162.54x |
| simdjson | 1.150x | 1.0001x | 0.51 | 1.08 | 281.15x |
| spdlog | 1.133x | 1.0002x | 0.26 | 0.53 | 117.99x |

The broad result changes the prioritization.  Godot contributes the largest Line-text leg,
while RocksDB contributes the largest structural leg; fmt and several short corpora have too
little amortization horizon.  No single one of them should become the design center.  The
acceptance view remains minimum, lower quartile, median, equal-corpus harmonic, and pass count.

## Cold and half-cold requirement ledger

The following projection adds only the two independently measured legs:

| Line-text state | byte-weighted ratio | equal-corpus harmonic | interpretation |
|---|---:|---:|---|
| cold Line text | 181.43x | **183.49x** | optimistic; far below 400x |
| half of cold Line text | 279.85x | **262.80x** | has 200x headroom, but incomplete |
| Line text already installed | 611.58x | **462.92x** | structural-only result |

| requested property | current evidence | state |
|---|---|---|
| complete cold ≥400x | optimistic incomplete combination is only 183.49x equal-corpus | **not met** |
| complete half-cold ≥200x | optimistic incomplete combination is 262.80x | **unproven until real cache scenario is charged** |
| online learning | hybrid uses encode-before-learn and immutable published phrases | structurally demonstrated |
| reorder/change stability | 14/14 retained hybrid variants decode exactly with small ratio movement | structurally demonstrated |
| new Line transform reorder stability | the trace order determines first-use TU grouping | not yet measured under reordered manifests |
| exact complete reconstruction | Line frames and structural frames are exact independently | combined complete codec still required |
| complete path ≥1 GB/s | older complete codec clears the gate on its measured set | new combined transform not yet gated |
| balanced evaluation | every result above covers all 16 corpora with one vote per corpus | met |

## Decision and next work

1. **Keep** the canonical cross-context structural hybrid as the structural contender while
   its simplification review is pending.
2. **Keep** TU-local best-of literal/front Line-text coding and port it only into the
   measurement codec first.
3. **Reject** a charged exact-Line package as a default cold bootstrap block.
4. Build one real combined frame path in which F receives only serialized bytes.  Charge the
   package/key map, missing exchange, Line definitions, Region composition, phrase definitions,
   context sidecar, Root payload, selectors, and framing.
5. Run cold, actual half-cold, reorder, reverse, deterministic shuffle, content perturbation,
   and revert scenarios across the balanced corpus set.
6. In parallel, evaluate a **fast parameterized Line program** across all 16 corpora.  The old
   one-corpus P4 result removes only about 20% and its research implementation is below the
   throughput gate; it is evidence, not the missing factor.  The next contender must improve
   the lower end broadly and retain literal fallback per frame.

The total target remains open.  The report intentionally rules out a weak direction and promotes
one small general transform without redefining success around either result.
