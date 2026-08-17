# Complete P29 bounded-BSC integration

## Result

A single fixed **112-TU** precompute boundary passes every currently binding gate on the two
large corpora used to choose and stress the residual codec. These are complete P29
encode/decode/accounting rows, not residual substitutions or projected totals.

| corpus | complete wire | cold gate | cold delta | TU100 / z6-long | TU200 / z6-long | min C / F GB/s | result |
|---|---:|---:|---:|---:|---:|---:|:---:|
| DuckDB | 7,873,485 | 7,883,713 | -10,228 | 1,519,485 / 2,129,543 | 2,124,228 / 2,847,389 | 1.708 / 2.774 | **PASS** |
| Godot | 35,094,169 | 64,118,473 | -29,024,304 | 1,064,247 / 1,290,414 | 2,057,414 / 2,211,547 | 1.013 / 1.144 | **PASS** |

The 112 boundary is the smallest measured clean common operating point between the earlier
100-TU and 128-TU candidates: 100 misses DuckDB cold by 7,132 bytes; 128 charges
material through TU256 at TU129 and misses Godot's TU200 reference. At 112, DuckDB is
10,228 bytes inside its cold gate and Godot is 154,133 bytes inside its TU200 gate.

This establishes the repeated integrated method on DuckDB and Godot. The subsequent
[fixed-16 breadth run](P29-BSC-GROUP-FIXED16.md) reconstructs all 16 corpora exactly;
this one fixed-112 candidate passes cold size and speed together on 9/16. That is a
measured standalone row, not the projected multi-mechanism best-of selector.

## Exact wire contract

Each nonempty group is one independently decodable frame:

```text
u32 little-endian header
  bits 31..29: codec (0=zstd-3, 1=libbsc BWT+adaptive QLFC, 2=zstd-10)
  bits 28..0 : payload byte length
payload[payload_length]
```

Libbsc payloads retain their self-describing 28-byte block headers and are capped at
64 MiB raw per internal block. The speed-gated row evaluates zstd-3 and BSC. Zstd-10
was removed from the timed candidate set because it selected zero groups at every
measured 10-TU-or-larger DuckDB/Godot boundary and zero at 112; removing the losing
trial changes no wire byte. Decoder tag 2 remains supported.

The sender reads a complete per-TU length plan, forms consecutive groups of at most 112
TUs, emits each group's full frame at its first charged TU, and records that exact frame
in the component ledger. The receiver decodes only the selected frame and feeds its
literal bytes into the ordinary P29 control-program decoder. The current encoder
regenerates each TU's raw literal lane; a stale plan with different content necessarily
fails final `.ii` comparison, while a length mismatch fails earlier.

## Repetition evidence

| corpus | rep | C GB/s | F GB/s | group encode / decode s | peak RSS MiB | elapsed s |
|---|---:|---:|---:|---:|---:|---:|
| DuckDB | 1 | 1.716 | 2.774 | 0.455 / 0.329 | 2742.8 | 4.52 |
| DuckDB | 2 | 1.708 | 2.820 | 0.456 / 0.322 | 2742.9 | 4.49 |
| DuckDB | 3 | 1.720 | 2.790 | 0.456 / 0.322 | 2742.8 | 4.49 |
| Godot | 1 | 1.031 | 1.158 | 0.479 / 0.329 | 8146.9 | 18.66 |
| Godot | 2 | 1.013 | 1.144 | 0.545 / 0.330 | 8152.0 | 18.94 |
| Godot | 3 | 1.023 | 1.148 | 0.486 / 0.334 | 8156.8 | 18.95 |

All three repetitions per corpus produced the same complete byte count, group-frame
SHA-256, per-TU curve SHA-256, and component-ledger SHA-256. Every curve has exactly one
row per TU; every row is exact; every component row sums to complete physical wire; and
both cumulative ledgers close to the reported total.

## Boundaries and limitations

- This is bounded precompute, not zero-lookahead streaming. A group must be available
  before its first TU can be reconstructed, so the method trades at most 112 TUs of
  startup/lookahead for the measured compression.
- Group encode/decode time is added serially to the split C/F proxy; no unimplemented
  overlap credit is taken. The gate is therefore conservative for independent lanes.
- The split result is the accepted research harness, not a preprocessor-pipe-to-compiler
  deployment measurement. That end-to-end product measurement remains later work.
- Peak RSS includes the full corpus, research dictionaries, both verification stores,
  retained raw plan, decoded plan, and analysis ledgers. It is not a per-job cache target.
- The three-repetition evidence covers DuckDB and Godot. The fixed-16 follow-up has one
  exact target-host run per remaining corpus; it is breadth evidence, not a three-run
  minimum-rate study for every corpus.

## Reproduction

The target build uses `-O3 -march=znver3`, libbsc 3.3.12 commit
`baffa62c70b6ebbecc9af14ce550e965ea247680`, and the retained zstd 1.4.8 MT
library. Runs are pinned with `taskset -c 0-15` on `tt-quietbox2` and use
`OMP_NUM_THREADS=1`; group-worker counts are 7 for DuckDB and 16 for Godot.

Machine-readable results: `ml-artifacts/p29-bsc-group-integration.json` and
`ml-artifacts/p29-bsc-group-integration.tsv`.

Retained run root: `/tmp/issue16-p29-bsc-integration-20260817` on
`ttuser@tt-quietbox2`.
