# 4-pass learning curves, five codecs

> **PROVISIONAL — superseded grouping.** These cells were produced by the single-4x-stream
> harness, in which a GRZ2 group can span the cold build into the first warm rebuild, so the
> per-build boundaries here are **not independently closed** (only 48 of 176 pass endpoints
> were GRZ2 close points). The 4x endpoints are correct and unchanged, but the intermediate
> builds are not. Regenerate with `selector_passcell.sh`, whose prefix method closes and
> independently decodes every build boundary — see `../pass-closure-proof/`.

Each project is run **4 times back-to-back** — cold pass 1, then warm rebuilds 2-4 — so the
per-TU curve runs to 4x the TU count. One shared 4x manifest per project means every codec
sees literally the same TU sequence, which is what makes the joined axis assertable.

`<project>.curve4.tsv`, one row per TU:

```
tu  pass  raw  cumulative_raw
p29_cum_wire         P29+BSC line-interning (codec50, batch basis)
grz2_cum_wire        GRZ2, binding --gtu 112; step function, forward-filled
fastintern_cum_wire  ultra-fast interner (production-fused, dedup + z3)
zstd3_cum_wire       zstd-3 per TU, independent -- the no-cross-TU-memory baseline
pred_cum_wire        Region-sequence ORACLE (empty-online-k2, cold, no seed)
                     -- NOT a full-.ii wire; see the note below
grz2_group_closes    1 where a GRZ2 group closed on this TU
```

> **Prediction line — read this before comparing totals.** `pred_cum_wire` is a
> **Region-sequence oracle** (`empty-online-k2`, cold, no pre-shared seed), **not a
> full-`.ii` wire** like the other four columns. It predicts over the Region stream rather
> than encoding the complete preprocessed input, so **its totals are NOT comparable to the
> P29+BSC / GRZ2 / fast-interner / zstd-3 totals — only the SHAPE of its curve is.** Any
> ratio taken against it is meaningless. This matches how the dashboard labels it.

## Lead batch

| project | TUs (4x) | P29+BSC | GRZ2 | fast interner | zstd-3/TU |
|---|---:|---:|---:|---:|---:|
| cereal | 336 (4x84) | 463,197 | 425,894 | 56,132,818 | 190,284,580 |
| range-v3 | 1036 (4x259) | 760,672 | 1,201,471 | 105,588,898 | 361,541,068 |
| catch2 | 3428 (4x857) | 950,546 | 1,729,319 | 174,475,892 | 573,945,636 |

## The contrast the passes are there to show

cereal, cumulative wire at the end of each pass:

| pass | cumulative raw | P29+BSC | fast interner | zstd-3/TU |
|---|---:|---:|---:|---:|
| 1 | 326,899,429 | 459,174 | 14,620,168 | 47,571,145 |
| 2 | 653,798,858 | 460,656 | 28,457,718 | 95,142,290 |
| 3 | 980,698,287 | 461,916 | 42,295,268 | 142,713,435 |
| 4 | 1,307,597,716 | 463,197 | 56,132,818 | 190,284,580 |

- **P29+BSC spends 99.1% of its four-pass wire in pass 1.** Rebuilds 2-4 add 1,482, 1,260
  and 1,281 bytes — roughly 0.3% each. That is the learning curve going flat.
- **zstd-3 per TU is dead straight**: 47.6M, 95.1M, 142.7M, 190.3M — exactly 4x, because it
  has no memory between TUs. It is the honest floor for "what if we did nothing clever".
- **The fast interner also climbs near-linearly** (14.6M -> 56.1M, 3.84x over 4 passes) and
  does **not** flatten. Pure line dedup still re-transmits the occurrence stream every pass;
  only the definitions are saved. This is the same non-zero asymptotic slope seen in the
  single-pass curves, now unmistakable over four builds.

So the plot separates three regimes rather than two: no memory (zstd-3, linear), dedup-only
memory (fast interner, near-linear at a lower slope), and structural memory (P29+BSC and
GRZ2, flat after pass 1).

## Gates

Per project, all asserted before the join is written:

- `zstd3` self-check: the per-TU sums equal independently accumulated totals (`sum_ok=1`).
- `fastintern`: per-TU byte-exact reconstruction, `verify=PASS`.
- `p29`: codec50's own `--curve-tsv` total-consistency check, and its curve endpoint equals
  the run's `TOTAL=` (verified equal on every project).
- `grz2`: group sums plus stream framing equal the codec's reported wire; every group
  boundary asserted against codec50's `cumulative_raw`.
- axis: every codec's per-TU `raw` sequence must match codec50's, TU for TU. A source that
  disagrees is dropped to `NA` rather than silently misaligned.

## Codec coverage caveats

- **GRZ2 is a step function** by construction (groups close every 112 TUs), so its column
  is flat between boundaries and `grz2_group_closes` marks the real points. On cereal the
  first group does not close until TU 111, which is inside pass 2 — hence `NA` for pass 1.
- **The prediction line is the expensive one.** It is single-threaded Python at roughly
  30 MB/s, against seconds-per-project for the other four. It does handle 4 passes and full
  corpora (`--max-tus` is unbounded; the earlier 200-TU curves were a `--max-tus 200`
  choice, not a limit), and it verifies `exact: true` at 336 TUs. Its cost is what will
  decide how many of the 30 projects carry it; any project without it shows `NA` and is
  named here rather than quietly omitted.
