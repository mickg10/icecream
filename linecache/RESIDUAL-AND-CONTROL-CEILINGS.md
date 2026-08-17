# Residual and Region-control ceilings after P26

> Historical note: the generic-codec decision below predates the exact PAQ/ZPAQ residual sweep.
> [`PAQ-RESIDUAL-CEILING.md`](PAQ-RESIDUAL-CEILING.md) supersedes the claim that all stronger
> residual coding should be closed: ZPAQ m3/m4/m5 expose a repeatable residual-only gain, while
> confirming that the unmodified codecs are too slow for the live path.

## Question

After P26, the cold ledger still had about 41 MB of zstd-3 RAW_RUN literal wire and 19.7 MB of
Region/control wire. This note records three stronger-than-product ceilings used to decide whether
another simple token/alphabet/control transform could close a material part of the cold-400 gap.

These are exact negative experiments. They are not proposed wire formats.

## 1. Generic residual entropy levels

The residual dump is the exact concatenation of P26's RAW_RUN byte plane before its normal zstd-3
stream. Higher levels and other general compressors measure entropy headroom, but most do not meet
the 1 GB/s full-pipeline requirement.

### DuckDB residual

Raw residual: 31,439,473 bytes.

| codec | encoded bytes | encode throughput |
|---|---:|---:|
| zstd-3 | 5,103,844 | fast baseline |
| zstd-6 | 4,560,012 | below the allowed level |
| zstd-19 | 3,609,361 | about 1.8 MB/s |
| xz-9 | 3,486,384 | about 2.1 MB/s |
| Brotli-5 | 4,228,615 | 57.95 MB/s |

### Godot residual

Raw residual: 96,195,716 bytes.

| codec | encoded bytes | encode throughput |
|---|---:|---:|
| zstd-3 standalone | 13,605,825 | fast baseline |
| zstd-3 + long window | 13,189,450 | fast enough, small gain |
| zstd-6 | 12,139,445 | below the allowed level |
| Brotli-5 | 11,101,164 | 69.99 MB/s |
| Brotli-9 | 10,403,368 | 19.56 MB/s |

Integrated zstd-3 long-distance matching saves only 2,770 bytes on fmt, 65,013 on DuckDB, and
489,699 on Godot. The residual contains real entropy headroom, but the large generic-codec gains
are far too slow and the fast long-window gain is too small.

## 2. Exact whole-generation word-token and sorted-Line ceilings

`residual_token_ceiling.cpp` is intentionally stronger than a causal product codec:

- it sees the entire residual before choosing dictionary words;
- it tries several global occurrence thresholds;
- it separately sorts every Line lexicographically and front-codes it;
- it transmits and decodes every required definition, rank, length, and residual byte;
- it reconstructs the complete residual exactly.

### fmt

| form | final wire | delta versus zstd-3 |
|---|---:|---:|
| ordinary zstd-3 | **378,779** | — |
| sorted/front-coded Lines | 442,614 | +63,835 |
| best word threshold (`count >= 3`) | 448,414 | +69,635 |

### Godot

| form | final wire | delta versus zstd-3 |
|---|---:|---:|
| ordinary zstd-3 | **13,605,825** | — |
| sorted/front-coded Lines | 16,824,228 | +3,218,403 |
| best word threshold (`count >= 2`) | 14,247,565 | +641,740 |

The explicit dictionary/control cost is larger than the zstd match gain even with complete future
knowledge. A causal first-profitable-use version cannot improve this ceiling.

## 3. Exact generation-wide alpha-normalized Line ceiling

`global_alpha_ceiling.cpp` gives the P4 alpha rule builder the complete generation in one batch,
removing per-TU selection and lifetime limitations. Both keyword modes decode exactly.

### fmt

| form | final wire | delta versus zstd-3 |
|---|---:|---:|
| ordinary zstd-3 | **378,779** | — |
| literal-keyword rules | 441,718 | +62,939 |
| parameterized-keyword rules | 440,438 | +61,659 |

### Godot

| form | final wire | delta versus zstd-3 |
|---|---:|---:|
| ordinary zstd-3 | **13,605,825** | — |
| literal-keyword rules | 14,648,351 | +1,042,526 |
| parameterized-keyword rules | 14,659,562 | +1,053,737 |

The best Godot mode builds 54,161 rules and uses 881,991 instances, but its rule/control cost is
larger than the bytes it removes. Extending P4 from TU-local to generation-wide state does not
rescue this family.

## 4. Semantic Region-control split ceiling

The integrated `--split-control-ceiling` diagnostic separates the Region grammar into eight
whole-generation streams:

1. Region lengths and operation counts;
2. opcodes;
3. literal lengths;
4. public-Line IDs;
5. Region deltas, offsets, and lengths;
6. marker path/line/flags;
7. source-copy fields;
8. source-patch fields.

It rebuilds the original control stream byte-for-byte and compares the sum of eight whole-run
zstd-3 frames with the actual streamed control plane.

| corpus | actual control | split whole-run ceiling | saving |
|---|---:|---:|---:|
| fmt | 479,473 | 474,071 | 5,402 |
| Godot | 4,164,025 | 3,923,335 | 240,690 |

Even complete batching and semantic separation recover only 0.24 MB on the largest control case.

## Decision

Reject these as cold continuation lanes:

- global word dictionaries over residual text;
- global sorted/front-coded residual Lines;
- generation-wide P4 alpha rules;
- a larger semantic split of the existing Region-control grammar;
- higher generic compression levels in the live path.

The measurements do not say the 41 MB literal plane is irreducible. They say its next useful
representation must use a stronger exact generated-domain structure than words, alphabet
templates, or field splitting. P27's canonical MO factor is an example of such a structure and is
reported separately.

## Reproduction

```sh
g++ -O3 -march=native -std=c++17 -Wall -Wextra -Werror \
  linecache/residual_token_ceiling.cpp -lzstd -o /tmp/residual-token-ceiling

g++ -O3 -march=native -std=c++17 -Wall -Wextra -Werror \
  linecache/global_alpha_ceiling.cpp -lzstd -o /tmp/global-alpha-ceiling

/tmp/residual-token-ceiling --input RESIDUAL.raw --z 3
/tmp/global-alpha-ceiling --input RESIDUAL.raw --z 3
```

Retained logs:

- `/tmp/issue16-{fmt,godot}-token-ceiling.log`
- `/tmp/issue16-{fmt,godot}-global-alpha.log`
- `/tmp/issue16-fmt-split-control.log`
- `/tmp/issue16-p27-godot-split-control.log`
