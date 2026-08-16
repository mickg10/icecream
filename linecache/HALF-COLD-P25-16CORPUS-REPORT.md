# P25: exact key-map cold and complementary half-cold Region-cache execution

## Result

P25 closes the previously unexecuted key-association and half-cold cache boundary around P24.  C
associates every first-used conversation-dense Region ID with its persistent C-cache `u64` key.  F
binds only the objects found in its own key-indexed store, constructs the missing-object response,
and decodes only the returned fills.  The independent F store then expands the ordinary S1
Root/Block stream and reconstructs every byte of every `.ii` file.

All three complete 16-corpus executions are exact.  Both complementary half-cold executions clear
the aggregate 200x requirement and the 1 GB/s transform/expand requirement:

| complete 16-corpus execution | key-map cold | half-cold key bit 0 | half-cold key bit 1 |
|---|---:|---:|---:|
| raw bytes / TUs / exact corpora | 28,554,671,510 / 9,292 / 16 | same | same |
| measured wire | 113,834,804 | **81,994,885** | **49,359,310** |
| byte-weighted ratio | 250.84x | **348.25x** | **578.51x** |
| equal-corpus harmonic ratio | 258.23x | **398.38x** | **468.60x** |
| allowance | 71,386,679 at 400x | 142,773,358 at 200x | 142,773,358 at 200x |
| margin to required allowance | **42,448,125 over** | **60,778,473 under** | **93,414,048 under** |
| minimum release pipeline | 1.70 GB/s | 2.00 GB/s | 2.28 GB/s |
| minimum individual-corpus ratio | 104.93x | 128.59x | 178.18x |

Thus the aggregate half-cold-200 gate is met for both deterministic complements.  It is not a claim
that every individual corpus clears 200x: 14 of 16 do so on both complements.  fmt is below 200x on
both, and Godot's harder complement is 128.59x.  Those tails remain explicit in the table below.

Cold-400 remains open.  P24 without the key map was 108,378,454 bytes; the complete association
plane adds 5,456,350 bytes, producing 113,834,804 bytes and a measured 42,448,125-byte cold gap.

## Exact state and messages

The prototype separates stable cache identity from conversation-local coding identity:

- The C cache assigns every newly installed Region one nonzero monotonic `u64` key.  The research
  harness uses `Region insertion index + 1`, which has the same allocation semantics.  It does not
  reuse the sampled Region lookup hash as an identity.
- Each C/F conversation uses compact Region IDs in its Roots, Blocks, and Region programs.
- F's reusable store is keyed by the C-cache key and belongs to the corresponding C-cache
  generation.  Preloaded objects are not placed into conversation-dense slots until an association
  arrives.

For each TU, the exercised transition is:

1. C constructs the Root and its required Region/Block closure.
2. For every Region mentioned for the first time in this conversation, C emits
   `(dense_id varint, key64 little-endian)` in one zstd-3 association frame.
3. F independently decodes that frame.  For each key present in its preload, F binds the immutable
   raw Region span to the supplied dense ID.  An absent key leaves the dense slot unknown.
4. F walks the required closure against its actual Region and Block state and emits the exact
   missing Region and Block IDs.  This response is independently zstd-3 encoded and decoded.  An
   empty missing set is still returned when it acknowledges a new association batch.
5. C uses the decoded response, rather than a precomputed synthetic list, to construct paths,
   mixed Region programs, and Block definitions.
6. F decodes every fill into its own append-only byte arena and dense-view vectors.  Preloaded raw
   Regions contribute no hidden public-Line metadata; only Regions decoded during the current
   conversation can establish P24 public views.
7. F expands the Root/Blocks, emits the complete `.ii`, and the harness compares every output byte
   with the original.

The association and missing messages are both compressed, length-framed, decompressed, parsed, and
compared.  They are charged in the `missing` category.  The ordinary P24 mode remains byte-for-byte
unchanged: fmt still measures exactly 1,047,430 bytes without `--key-map`.

## Complement definition and cache skew

The half-cold state uses both deterministic complements of `key64 & 1`.  A run with bit `b`
preloads every Region whose stable key has low bit `b`; the other Regions start absent.  Across the
16 corpora:

| preload | Region objects | immutable raw Region bytes | association wire | missing reply wire |
|---|---:|---:|---:|---:|
| none (cold) | 0 | 0 | 5,456,350 | 3,488,397 |
| key bit 0 | 776,293 | 335,879,233 | 5,456,350 | 1,885,091 |
| key bit 1 | 776,300 | 506,094,161 | 5,456,350 | 1,885,269 |

The complements split object count almost exactly in half, but they intentionally do not pretend
that all objects have equal size.  Godot is the strongest example: its two equal-count halves hold
95,933,308 and 251,260,942 raw Region bytes.  A large generated translation payload falls mostly in
one partition, making the opposite partition much harder.  Reporting both complements prevents
that skew from disappearing into an average.

## Complete category ledger

| wire category | key-map cold | bit 0 half-cold | bit 1 half-cold |
|---|---:|---:|---:|
| Root | 3,687,803 | 3,687,803 | 3,687,803 |
| Line/material payload | 80,123,571 | 59,606,410 | 26,812,600 |
| Region programs | 19,749,171 | 10,040,415 | 10,198,502 |
| Blocks | 428,568 | 428,568 | 428,568 |
| paths | 828,100 | 818,384 | 818,342 |
| association + missing reply | 8,944,747 | 7,341,441 | 7,341,619 |
| other framing | 72,844 | 71,864 | 71,876 |
| **total** | **113,834,804** | **81,994,885** | **49,359,310** |

The Root and Block legs are identical in all three executions.  The remaining cold gap therefore
cannot be closed by another Root-ID variation: eliminating the entire 3.69 MB Root would still
leave almost 38.8 MB to remove.  The binding planes are the ordinary literal material and the
already-compressed generated-array values, followed by Region control.

## Per-corpus result

| corpus | key-map cold | half bit 0 | half bit 1 | preloaded raw MiB 0 / 1 | pipeline GB/s 0 / 1 |
|---|---:|---:|---:|---:|---:|
| llvm | 376.88x | 697.35x | 635.26x | 26.4 / 23.3 | 6.75 / 5.43 |
| rocksdb | 260.40x | 382.17x | 382.23x | 52.4 / 52.2 | 4.15 / 4.50 |
| duckdb | 192.57x | 299.56x | 347.51x | 37.1 / 52.6 | 3.54 / 4.01 |
| abseil | 379.58x | 576.55x | 568.16x | 29.7 / 29.5 | 5.38 / 6.07 |
| opencv | 517.56x | 853.28x | 900.40x | 35.0 / 37.1 | 6.64 / 6.18 |
| godot | 104.93x | **128.59x** | 418.53x | **91.5 / 239.6** | 2.00 / 4.34 |
| fmt | 115.22x | **181.73x** | **178.18x** | 4.8 / 4.4 | 2.21 / 2.28 |
| spdlog | 173.83x | 300.29x | 307.73x | 2.2 / 2.2 | 2.37 / 2.67 |
| catch2 | 854.07x | 1,310.27x | 1,325.04x | 5.0 / 5.2 | 6.24 / 6.42 |
| nlohmann-json | 241.36x | 399.19x | 400.63x | 7.3 / 7.4 | 3.44 / 3.33 |
| range-v3 | 688.18x | 1,138.40x | 1,125.23x | 3.8 / 3.7 | 6.04 / 6.18 |
| eigen | 2,467.32x | 3,813.01x | 3,758.38x | 11.2 / 11.2 | 6.99 / 8.05 |
| re2 | 217.06x | 359.15x | 399.45x | 1.8 / 1.9 | 2.85 / 3.08 |
| leveldb | 190.82x | 307.88x | 302.21x | 2.8 / 2.8 | 2.95 / 2.98 |
| simdjson | 322.24x | 524.60x | 553.06x | 7.1 / 7.2 | 3.89 / 4.19 |
| cereal | 616.87x | 1,098.70x | 1,077.35x | 2.3 / 2.2 | 4.03 / 4.99 |

## Correctness and performance gates

- Warning-clean release build with `-O3 -Wall -Wextra -Werror`.
- Cold, bit-0, and bit-1 matrices reconstruct all 9,292 TUs exactly: 48/48 corpus runs.
- ASan+UBSan reconstruct both fmt complements exactly with the same release wire totals.
- The ordinary P24 fmt control is unchanged at 1,047,430 bytes.
- 16 focused unit/property tests pass; Python compilation, Ruff, and `git diff --check` pass.
- Association and missing-response subledgers close the complete `missing` category exactly.
- Complementary preload counts sum to every corpus's Region count, every dense Region is associated,
  and association bytes are independent of cache temperature.
- Every category closes its reported total and the aggregate regenerates deterministically.

The measured speed is the current transform/compress/decode/install/expand proxy, not yet the
actual socket dispatcher.  All release rows clear 1 GB/s; the all-corpus minima are stated above.

## Consequence for the next contender

P25 resolves the half-cold accounting question without resolving cold-400.  The complete cold
budget is now concrete:

```text
current complete cold       113,834,804 bytes
cold-400 allowance           71,386,679 bytes
required saving              42,448,125 bytes
```

Association tuning alone cannot provide the factor.  The next contender must address tens of MB
inside P24's literal/value material.  In particular, Godot's complementary result confirms that
the generated translation bytes are a genuine binding payload: whichever half does not already
hold them must transfer them.

The next measurement should revisit the exact compressed-blob transform under the *complete raw
corpus* speed equation.  Its local transform operates on roughly 114 MB of expanded blob payload,
not on Godot's 5.93 GB raw corpus; therefore a sub-1-GB/s local component is not automatically a
failure.  It must measure the full C inflate/transform plus F decode/exact regeneration time and
the resulting complete ledger.  In parallel, ordinary literal material needs one bounded exact
candidate block with actual-byte selection; another Root predictor cannot close the measured gap.

Still unproved after P25:

1. cold-400 closure;
2. actual socket framing and ordered-lane lifecycle;
3. bounded cache eviction and multi-F assignment;
4. reorder plus broad input-change/revert execution over persistent keys and online learning.

## Reproduction and retained evidence

Build:

```sh
g++ -O3 -march=native -std=c++17 -DICE_LINE_CAP_LOG2=23 \
  -Wall -Wextra -Werror linecache/codec50.cpp -lzstd -o /tmp/codec50-p25
```

Run one complete state:

```sh
/tmp/codec50-p25 --manifest MANIFEST --z 3 --mixed-regions --byte-array-lines --key-map
/tmp/codec50-p25 --manifest MANIFEST --z 3 --mixed-regions --byte-array-lines --half-cold-bit 0
/tmp/codec50-p25 --manifest MANIFEST --z 3 --mixed-regions --byte-array-lines --half-cold-bit 1
```

Regenerate the committed summary:

```sh
python3 linecache/summarize_half_cold_codec.py \
  --p24-dir /tmp/issue16-complete-p24-final-z3 \
  --keymap-cold-dir /tmp/issue16-p25-final-cold \
  --bit0-dir /tmp/issue16-p25-final-bit0 \
  --bit1-dir /tmp/issue16-p25-final-bit1 \
  --output linecache/ml-artifacts/half-cold-p25-16corpus-summary.json \
  --tsv linecache/ml-artifacts/half-cold-p25-16corpus.tsv
```

Retained full logs:

- `/tmp/issue16-p25-final-cold`
- `/tmp/issue16-p25-final-bit0`
- `/tmp/issue16-p25-final-bit1`

Every log digest is recorded in the machine summary.
