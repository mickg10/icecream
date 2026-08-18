# Per-TU learning curves: the four-line join

X = TU, Y = cumulative bytes transmitted. Six `/goal` corpora. **All four lines share one
basis — single-decoder cumulative C-side wire — and one TU axis, asserted rather than
assumed.** One file per corpus: `learning-curves/<corpus>.learning-curve.tsv`.

## The four lines and their endpoints

| corpus | TUs | raw | P29+BSC (codec50) | GRZ2 | fast interner | prediction @TU200 |
|---|---:|---:|---:|---:|---:|---:|
| range-v3 | 259 | 632,049,016 | 739,437 (854.8x) | 628,319 (1005.9x) | 27,282,571 (23.2x) | 778,505 |
| catch2 | 857 | 947,252,235 | 907,327 (1044.0x) | 626,178 (1512.8x) | 44,414,168 (21.3x) | 452,238 |
| rocksdb | 622 | 3,114,320,596 | 8,533,638 (364.9x) | 5,456,878 (570.7x) | 137,472,315 (22.7x) | 11,397,698 |
| eigen | 650 | 3,532,268,956 | 1,143,631 (3088.6x) | 2,040,151 (1731.4x) | 137,499,761 (25.7x) | 971,955 |
| llvm | 1238 | 3,620,271,340 | 7,275,808 (497.6x) | 7,638,087 (474.0x) | 174,833,202 (20.7x) | 1,651,030 |
| godot | 2207 | 5,932,762,185 | 35,085,071 (169.1x) | 53,427,567 (111.0x) | 299,979,078 (19.8x) | 555,383 |

**Both structural endpoints match the cold-C-encode `/goal` row to the byte** — codec50's
per-TU curve terminates exactly at its `TOTAL=`, and GRZ2's group sums plus framing at its
wire. The prediction column stops at TU 200 and its ratio there is **not** comparable to
the full-corpus ratios in the other columns; compare lines only at equal TU.

At TU 199, where all four lines exist:

| corpus | cumulative raw | P29+BSC | GRZ2 | fast interner | prediction |
|---|---:|---:|---:|---:|---:|
| range-v3 | 484,251,452 | 676,036 | **409,592** | 21,090,477 | 778,505 |
| catch2 | 212,426,768 | 536,189 | **388,779** | 10,532,194 | 452,238 |
| rocksdb | 1,224,557,334 | 5,629,027 | **2,794,064** | 53,542,848 | 11,397,698 |
| eigen | 1,098,381,060 | 901,099 | **724,975** | 43,497,987 | 971,955 |
| llvm | 724,323,634 | 2,361,461 | **1,216,755** | 36,461,089 | 1,651,030 |
| godot | 382,645,585 | 2,056,409 | **951,028** | 18,314,729 | 555,383 |

## The thesis the shapes carry

**The dedup asymptote is corpus-independent; the structural ones are not.** Pure line dedup
lands in 19.8-25.7x on all six — a 1.3x spread — while GRZ2 spans 111x to 1731x and codec50
169x to 3089x, both tracking corpus redundancy over more than a 15x range. Dedup removes
repeated line *text* but still transmits one local index per line *occurrence* every TU, and
that stream scales with occurrences rather than novelty: a floor repetition cannot lower.
The structural codecs win by factoring the *sequence* — long-range copies, regions, blocks —
which is exactly what dedup leaves on the wire. On the plot the fast-interner line should
flatten to a visibly **non-zero slope** while the structural lines keep bending.

## Rate

The fast interner is the fast one, and the cost is not interning:

| corpus | interner alone | body build | zstd-3 | pipelined C | F side |
|---|---:|---:|---:|---:|---:|
| range-v3 | 4.00 | 4.55 | 1.55 | **0.90** | 1.71 |
| catch2 | 4.05 | 4.70 | 1.55 | **0.91** | 1.73 |
| rocksdb | 4.16 | 4.90 | 1.59 | **0.93** | 1.76 |
| eigen | 4.79 | 5.52 | 1.80 | **1.06** | 2.12 |
| llvm | 3.80 | 4.35 | 1.42 | **0.83** | 1.63 |
| godot | 4.76 | 5.23 | 1.26 | **0.84** | 1.60 |

## Columns

```
tu  raw  cumulative_raw
p29batch_wire  p29batch_cum_wire  p29batch_ratio          codec50, /goal config, per TU
grz2_cum_wire  grz2_group_closes  grz2_ratio              binding --gtu 112, step function
grz2_fine_cum_wire                                        --gtu 8, SHAPE ONLY
fastintern_wire  fastintern_cum_wire  fastintern_ratio    production-fused, per TU
pred_cum_wire  pred_ratio                                 empty-online-k2, TUs 0-199 only
predfrozen_cum_wire                                       empty-frozen, the no-learning control
p29socket_*  p29socket_b_root/b_need/b_fill               cap_m5 -- DIFFERENT BASIS, see below
```

## Five things to respect

1. **`p29batch_*` is the line to plot, not `p29socket_*`.** They are different deployments
   of the same family and differ by ~2.5x on the same corpus (range-v3: 854.8x batch vs
   347.2x socket) because the per-TU dialogue pays framing, per-TU entropy coding and a Fill
   round trip the batch encoder does not. `p29socket_*` is carried for the daemon lane —
   with its Root/Need/Fill split on the same axis — and belongs on a separate panel labelled
   *socket-dialogue basis*, if at all.

2. **GRZ2 is a step function by construction.** It buffers whole groups (`--gtu 112`), so
   `grz2_cum_wire` advances only when a group closes: 3 steps range-v3, 8 catch2, 7 rocksdb,
   7 eigen, 12 llvm, 23 godot. Between boundaries nothing has been transmitted, so
   forward-filling the last closed group is the honest curve, not an artifact to smooth.

3. **`grz2_fine_cum_wire` is a different codec instance** (`--gtu 8`, 33-276 points), for
   resolution only. It costs range-v3 +14.2%, catch2 +18.3%, rocksdb +13.0%, eigen +28.2%,
   llvm +13.4%, godot +3.2%. Plot dashed and label shape-only. It also prices a real
   deployment tradeoff: emitting every 8 TUs instead of every 112 costs 3-28% of the wire.

4. **The prediction line is COLD and SHORT.** It is `empty-online-k2` from
   `~/bakeoff/curves_empty/` — no pretrained seed. The seeded series in `~/bakeoff/curves/`
   (`pretrained-seed-online-k2`) is warm-started and would **not** be a cold single-decoder
   baseline; on range-v3 it reports 794.9x at TU 200 against the cold 622.0x, so mixing them
   would overstate the line by ~28%. Only TUs 1-200 exist for any corpus, which on godot is
   382 MB of 5.9 GB.

5. **The prediction line's charge basis differs from the other three in one way you should
   label.** Its charged bytes are `payload + definition + context + selector`, and the
   running sum equals `cumulative_charged_bytes` exactly on all six — internally consistent.
   But `candidate_state_bytes` / `logical_state_bytes` (11.5 / 12.0 MB on range-v3 at TU 200)
   are **not** charged, on the assumption that both sides learn symmetrically from the
   decoded stream. That is the standard prequential convention and is probably right, but
   unlike the other three lines **I have no byte-exact reconstruction gate for it** — I did
   not build that codec and did not verify a decode. Label it as a model-cost curve, not a
   gated wire.

## What is gated, and how

| line | gate |
|---|---|
| P29+BSC (codec50) | `--curve-tsv` self-checks that per-TU sums equal the totals; endpoint == `TOTAL=`; `exact=true` on every TU of all six |
| GRZ2 | group sums + framing == the codec's own reported wire; totals match the `/goal` row byte for byte |
| fast interner | observation-only `--curve` patch; unpatched build reports identical `body`/`comp`/`miss`/`keys`/`occ`; per-TU byte-exact reconstruction `verify=PASS` |
| prediction | internal consistency only (see point 5) |
| axis | codec50's per-TU `raw` is the reference; fast interner asserted TU by TU, GRZ2 asserted at every group boundary via `hist_base + hist_extent`, prediction asserted on `cumulative_raw_bytes` at every one of its 200 points, cap_m5 asserted TU by TU. A source that fails is dropped to `NA`, never silently misaligned. |

## Provenance

- codec50: `--curve-tsv` / `--component-curve-tsv`, pre-existing features, run with the
  binding `/goal` flags (`selector_p29curve.sh`).
- GRZ2: `grz2g-selector enc ... --gtu 112 ... --curve`, pre-existing feature, binding config.
- fast interner: `production-fused --level 3 --curve`
  (`selector_fused_curve_instrument.py`). Godot needed `FUSED_LINE_CAP_LOG2=23` — 2.64M
  distinct lines overflow the stock `1<<21` — proven wire-neutral by a byte-identical curve
  on range-v3.
- join: `selector_curvejoin4.py`. Axis audit: `selector_axischeck.py`.

All lines are **single-decoder baseline**: one F sees the whole stream. In the sharded farm
(one C keyspace, ~30 F daemons, non-uniform per-F streams) every line is optimistic, GRZ2
most of all, since a copy referencing a TU that went to a different F is a miss it cannot
fill.
