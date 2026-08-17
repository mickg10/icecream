# PAQ/ZPAQ residual ceiling and runtime decision

## Result

PAQ-family modeling is useful **after** the existing line/region/LZ transform, not as a replacement
for whole-program deep-window LZ.

- Whole-program DuckDB favors `zstd -19 --long=31`: the implementer's independently measured
  ZPAQ m5 row is 7,631,779 bytes versus 7,189,449 bytes for zstd, a 6.2% loss.
- The exact `RAW_RUN` residual favors stronger ZPAQ levels on two unrelated large corpora. ZPAQ m5
  is 33.13% smaller than residual zstd-19-long on DuckDB and 33.47% smaller on Godot.
- ZPAQ m4 is the more informative engineering ceiling: it gives about 25% of residual savings over
  zstd-19-long while running approximately three times faster than m5.
- ZPAQ m3 is the nearest runtime starting point. It narrowly beats residual zstd-19-long on both
  corpora and leaves the projected DuckDB complete transfer only 25,412 bytes above the cold gate.
  Its selected ZPAQ method is a BWT followed by an order-0 ICM and order-1 ISSE, so this result
  specifically opens a fast BWT-plus-entropy row in addition to a reduced context mixer.
- `paq8px -1` is a ceiling only. On a 1 MiB DuckDB residual slice it is another 24.9% smaller than
  ZPAQ m5, but it encodes at about 0.009 MB/s.

No full PAQ-family implementation measured here meets the runtime gate. The measurements say that
the residual has enough predictable information to close the byte gap and identify which compact
models to reproduce; they do not nominate ZPAQ or paq8px as the live codec.

Machine-readable evidence is in
[`paq-residual-ceiling.json`](ml-artifacts/paq-residual-ceiling.json) and
[`paq-residual-ceiling.tsv`](ml-artifacts/paq-residual-ceiling.tsv).

## Scope and exactness

The inputs are the exact concatenated P26/P29 `RAW_RUN` byte planes before their normal zstd-3
encoding:

| program | complete raw `.ii` | residual raw | residual fraction | SHA-256 |
|---|---:|---:|---:|---|
| DuckDB | 1,985,715,205 | 31,439,473 | 1.5833% | `263157e32ec56f374770da42290eec8abb9baefb12322dca645f7b3090a6c372` |
| Godot | 5,932,762,185 | 96,195,716 | 1.6214% | `7356cc96e3d2dbf80068f2e38f143a7f2c8fadb7e124c79503e01897fffb1570` |

Every reported zstd and ZPAQ archive was independently decompressed and the resulting SHA-256 was
compared with the corresponding input. The paq8px slice was likewise decompressed and compared.
Every row is exact. Archive sizes include the codec's actual frame/archive overhead.

All timings are single-process, `-t1`/`-T1` runs on `ttuser@tt-quietbox2` (`tt-quietbox`, 32 logical
CPUs). Zstd is pinned at 1.5.7. ZPAQ 7.15 is built from commit
`9ab539f644e364f0d92e2918b90ce2534c75653f`; paq8px v216 is built from commit
`29237fb44cb1995690e3eb72c6c3b1e4aede5791`.

## Full residual results

### DuckDB

The projection substitutes the measured archive for P29's 5,103,844-byte residual zstd-3 lane and
leaves every other P29 byte unchanged. It is deliberately labeled a projection until the candidate
is integrated into the independent P29 decoder.

| codec | residual wire | vs z19-long | encode | decode | encode RSS | projected complete | delta to 7,883,713 gate |
|---|---:|---:|---:|---:|---:|---:|---:|
| zstd-6-long | 4,456,114 | 1.256x | 89.83 MB/s | 0.07 s | 40.5 MiB | 8,941,996 | +1,058,283 |
| zstd-19-long | 3,547,566 | 1.000x | 2.30 MB/s | 0.05 s | 135.5 MiB | 8,033,448 | +149,735 |
| ZPAQ m1 | 5,025,860 | 1.417x | 32.41 MB/s | 0.29 s | 101.5 MiB | 9,511,742 | +1,628,029 |
| ZPAQ m2 | 4,416,230 | 1.245x | 6.76 MB/s | 0.29 s | 174.8 MiB | 8,902,112 | +1,018,399 |
| **ZPAQ m3** | **3,423,243** | **0.965x** | **4.85 MB/s** | 7.12 s | 158.0 MiB | **7,909,125** | **+25,412** |
| ZPAQ m4 | 2,657,612 | 0.749x | 1.68 MB/s | 18.95 s | 202.8 MiB | 7,143,494 | **-740,219** |
| ZPAQ m5 | 2,372,254 | 0.669x | 0.56 MB/s | 57.83 s | 494.0 MiB | 6,858,136 | **-1,025,577** |

The important boundary is m3: it beats zstd-19-long by 124,323 residual bytes and reduces the
complete projected miss from 1,706,013 bytes under P29/zstd-3 to 25,412 bytes. M4 and m5 prove ample
byte headroom, but do so at non-product rates.

### Godot

Godot already passes the cold `.ii` gate. It is used here to test whether the residual result
generalizes beyond DuckDB.

| codec | residual wire | vs z19-long | encode | decode | encode RSS | projected complete |
|---|---:|---:|---:|---:|---:|---:|
| zstd-6-long | 11,694,628 | 1.261x | 100.20 MB/s | 0.16 s | 114.5 MiB | 37,678,327 |
| zstd-19-long | 9,272,575 | 1.000x | 2.35 MB/s | 0.18 s | 249.0 MiB | 35,256,274 |
| ZPAQ m1 | 13,333,617 | 1.438x | 38.95 MB/s | 0.78 s | 102.9 MiB | 39,317,316 |
| ZPAQ m2 | 11,605,426 | 1.252x | 6.48 MB/s | 0.86 s | 391.0 MiB | 37,589,125 |
| **ZPAQ m3** | **9,241,472** | **0.997x** | **4.67 MB/s** | 20.73 s | 357.5 MiB | **35,225,171** |
| ZPAQ m4 | 6,909,348 | 0.745x | 1.70 MB/s | 57.13 s | 428.3 MiB | 32,893,047 |
| ZPAQ m5 | 6,169,401 | 0.665x | 0.56 MB/s | 172.69 s | 866.2 MiB | 32,153,100 |

The m4 and m5 savings are nearly identical in percentage terms on DuckDB and Godot. That stability
is stronger evidence than the small m3 win alone: the residual really contains information that
deep-window zstd does not exploit.

## What the ZPAQ levels are buying

ZPAQ's numeric levels select methods from content analysis rather than merely changing one search
depth. A `-s0` trace of the Godot m3 run selected:

```text
block 1: -method 36,183,0
block 2: -method 36,184,0
```

In libzpaq, these redundancy/type values enter the `type >= 640` level-3 branch. That expands to a
BWT transform followed by an order-0 indirect context model and an order-1 indirect secondary
symbol estimator. Thus the m3 result is not evidence that a generic byte mixer alone is enough.
It is evidence for a fast block-sorting residual candidate.

The higher levels add the expensive part that explains their larger gain:

- several overlapping direct and indirect byte/word contexts;
- match prediction;
- adaptive mixing in logistic probability space;
- secondary probability estimation/correction;
- arithmetic coding of the resulting bit probabilities.

M5's practical advantage over m4 is modest: it saves 285,358 bytes on DuckDB and 739,947 bytes on
Godot, or about 10.7% of the m4 output, while taking roughly three times as long and about twice the
memory on Godot. M4 is therefore the better target ceiling unless the last megabyte is binding.

## paq8px ceiling

Running full paq8px on these residuals would consume hours. The exact first 1 MiB of DuckDB was used
as a bounded information probe:

| codec | encoded bytes | versus zstd-19-long | encode time |
|---|---:|---:|---:|
| zstd-3 | 167,060 | 1.304x | — |
| zstd-19-long | 128,138 | 1.000x | — |
| ZPAQ m3 | 119,938 | 0.936x | 0.19 s |
| ZPAQ m4 | 95,140 | 0.742x | 0.57 s |
| ZPAQ m5 | 86,961 | 0.679x | 1.84 s |
| **paq8px v216 level 1** | **65,319** | **0.510x** | **116.11 s** |

paq8px level 1 is 24.9% smaller than ZPAQ m5 and 49.0% smaller than zstd-19-long on this slice. It
decoded exactly in 118.89 seconds and peaked at 380,416 KiB RSS. Its approximately 0.009 MB/s local
rate is over 2,600 times below the approximately 24 MB/s residual rate required by the effective
throughput model. The slice result must not be extrapolated as a full-corpus byte count.

Its value is diagnostic: it says m5 is not the residual's entropy floor and provides candidate
features—long matches, word/token contexts, many weak byte contexts, adaptive mixing, and secondary
correction—to ablate in a much smaller deterministic model.

## Runtime accounting

With a 3 GB/s ordinary base path and residual fraction `f`:

```text
1 / effective_GBps = 1 / 3 + f / residual_codec_GBps
```

The residual codec must sustain 23.75 MB/s for DuckDB and 24.32 MB/s for Godot to retain at least
1 GB/s raw-equivalent throughput. The measured encode-side projections are:

| codec | DuckDB effective | Godot effective | runtime gate |
|---|---:|---:|:---:|
| zstd-6-long | 1.96 GB/s | 2.02 GB/s | pass |
| ZPAQ m1 | 1.22 GB/s | 1.33 GB/s | pass |
| ZPAQ m2 | 0.37 GB/s | 0.35 GB/s | fail |
| ZPAQ m3 | 0.28 GB/s | 0.26 GB/s | fail |
| ZPAQ m4 | 0.10 GB/s | 0.10 GB/s | fail |
| ZPAQ m5 | 0.035 GB/s | 0.034 GB/s | fail |

This is an optimistic component projection because it starts from a 3 GB/s base and excludes the
candidate-selection work. A promoted implementation must measure the complete path directly.

## Reconciliation with the whole-program result

There is no contradiction between the implementer's whole-program ZPAQ loss and this residual win:

```text
whole .ii stream:
    enormous repeated header expansions
    -> deep 2 GiB LZ window is the dominant tool

P29 residual stream:
    long repeated lines/regions already removed
    -> remaining local order, byte, word, match, and probability correlations matter
```

Applying PAQ to the first stream throws away zstd's most valuable advantage. Applying a reduced
PAQ/BWT candidate only to the second stream stacks the two advantages. Likewise, an order-0/1/2
rANS coder losing on a dense lane only says that probability coding cannot replace missing LZ
matches; it says nothing against probability coding after those matches have already been removed.

## Implementation decision

Keep the codec as independently selectable blocks:

```text
deep-LZ base over dense/repeated material
    + exact P29 structure
    + residual candidate selector:
        A. fast zstd/LDM fallback
        B. fast BWT + compact ICM/ISSE or rANS tables
        C. reduced CM: MATCH + a few byte/token contexts + mixer/SSE
```

The next experiments should be executed in this order:

1. Benchmark a modern fast BWT construction/inverse plus FSE/rANS on the exact DuckDB and Godot
   residuals. M3 needs about a five-fold speedup to reach the local residual-rate gate.
2. Profile or ablate ZPAQ m4 to identify the byte contribution of MATCH, context chains, mixing,
   and SSE. Reimplement only components that repay their actual serialized and runtime cost.
3. Split the residual into causal candidate blocks and select between fast zstd, BWT, and reduced CM
   by actual framed bytes. Retain the fast fallback for every block.
4. Integrate the winning residual candidate into the independent P29 encoder/decoder. Only then
   report complete bytes, TU100/TU200 learning curves, and measured full-path throughput.
5. Run the same row on LLVM, RocksDB, OpenCV, then the fixed 16. Two corpora establish a useful
   direction, not a suite-wide result.

The immediate target is not to reproduce paq8px. It is to recover enough of the m3/m4 gain at
roughly 24 MB/s local residual rate, which is a much smaller and testable problem.

## Retained evidence

- remote root: `/tmp/issue16-compression-tournament/results`
- raw inputs: `/tmp/issue16-duckdb-residual.raw`, `/tmp/issue16-godot-residual.raw`
- pinned sources: `/tmp/issue16-zpaq-src-20260817`, `/tmp/issue16-paq8px-src-20260817`
- ZPAQ method trace: `godot-full-zpaq-m3-methodtrace.zpaq` and its console record
- every archive SHA-256, timing, RSS, decoded SHA-256, and projection input is retained in the JSON
  artifact linked above

