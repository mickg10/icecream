# Per-TU learning curves: GRZ2 and the ultra-fast interner

X = TU, Y = cumulative bytes transmitted, on the six `/goal` corpora. Single-decoder
baseline: one F sees the whole stream. In the sharded farm (one C keyspace, ~30 F
daemons, non-uniform per-F streams) both curves are optimistic — GRZ2 especially, since a
copy referencing a TU that went to a different F is a miss it has no fill for.

## Endpoints — all three measured lines

| corpus | TUs | raw | P29 (cap_m5) | ratio | fast-interner | ratio | GRZ2 | ratio | fast C-side |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| range-v3 | 259 | 632,049,016 | 1,820,664 | 347.2x | 27,282,571 | 23.2x | 628,319 | **1005.9x** | 0.90 GB/s |
| catch2 | 857 | 947,252,235 | 2,384,393 | 397.3x | 44,414,168 | 21.3x | 626,178 | **1512.8x** | 0.91 GB/s |
| rocksdb | 622 | 3,114,320,596 | 15,692,955 | 198.5x | 137,472,315 | 22.7x | 5,456,878 | **570.7x** | 0.93 GB/s |
| eigen | 650 | 3,532,268,956 | 3,289,527 | 1073.8x | 137,499,761 | 25.7x | 2,040,151 | **1731.4x** | 1.06 GB/s |
| llvm | 1238 | 3,620,271,340 | 14,614,029 | 247.7x | 174,833,202 | 20.7x | 7,638,087 | **474.0x** | 0.83 GB/s |
| godot | 2207 | 5,932,762,185 | 62,871,696 | 94.4x | 299,979,078 | 19.8x | 53,427,567 | **111.0x** | 0.84 GB/s |

Every GRZ2 total matches the cold-C-encode `/goal` row to the byte.

**The P29 column is `cap_m5`, the per-TU socket dialogue at one sticky consumer — not
`codec50`'s batch wire.** They differ substantially for the same corpus (range-v3: 347.2x
here against codec50's 854.8x) because the dialogue pays per-TU framing, per-TU entropy
coding and a Fill round trip that the batch encoder does not. Both are honest numbers for
different deployments; do not put them on one axis or quote one as the other.

**All three lines are verified on the same TU axis.** At every GRZ2 group boundary
`hist_base + hist_extent` equals the fast interner's `cumulative_raw`; and the cap_m5 curve's
per-TU `raw` equals the fast interner's per-TU `raw` for every TU of all six corpora, with
the TU order the identity permutation throughout (`selector_axischeck.py`). Zero mismatches.

## The finding the shapes carry

**The fast interner's asymptote is corpus-independent; the structural codec's is not.**
Pure line dedup lands in a 19.8-25.7x band on all six corpora — a 1.3x spread — while GRZ2
spans 111x to 1731x, a **15.6x** spread that tracks corpus redundancy. Dedup removes
repeated line *text*; it still transmits the occurrence stream, one local index per line
occurrence, every TU. That stream scales with occurrences, not with novelty, so it sets a
floor no amount of repetition can lower. GRZ2 and P29 win by factoring the *sequence* —
long-range copies, regions, blocks — which is exactly the part dedup leaves on the wire.

So on the dashboard the fast-interner line should flatten to a **visibly non-zero slope**
while the structural lines keep bending. That gap is what the structural codecs buy.

## Rate: it is the fast one, but z3 is the pipeline's limit

Reporting both halves rather than the flattering one. C-side, level 3, `F-empty`:

| corpus | interner alone | body build | zstd-3 | **pipelined C** | F side |
|---|---:|---:|---:|---:|---:|
| range-v3 | 4.00 | 4.55 | 1.55 | **0.90** | 1.71 |
| catch2 | 4.05 | 4.70 | 1.55 | **0.91** | 1.73 |
| rocksdb | 4.16 | 4.90 | 1.59 | **0.93** | 1.76 |
| eigen | 4.79 | 5.52 | 1.80 | **1.06** | 2.12 |
| llvm | 3.80 | 4.35 | 1.42 | **0.83** | 1.63 |
| godot | 4.76 | 5.23 | 1.26 | **0.84** | 1.60 |

The interner tier itself runs **3.80-4.79 GB/s** — it is the ultra-fast one. In series with
body construction and zstd-3 the codec delivers **0.83-1.06 GB/s**. Entropy coding, not
interning, is what costs; the same conclusion the P29 stage split reached.

## Reading the files

`learning-curves/<corpus>.learning-curve.tsv`, one row per TU:

```
tu  raw  cumulative_raw
fastintern_wire  fastintern_cum_wire          per TU, exact
grz2_cum_wire  grz2_group  grz2_group_closes    step: complete groups only
fastintern_ratio  grz2_ratio
grz2_fine_cum_wire                            shape only, see below
p29_wire  p29_cum_wire  p29_ratio             cap_m5, workers=1, per TU, exact
p29_b_root  p29_b_need  p29_b_fill            that TU's Root/Need/Fill byte split
```

Three things to respect when plotting:

1. **GRZ2 is a step function and that is intrinsic.** It buffers whole groups
   (`--gtu 112`, the binding configuration), so `grz2_cum_wire` only advances when a group
   closes: 3 steps for range-v3, 8 catch2, 7 rocksdb, 7 eigen, 12 llvm, 23 godot. Between
   boundaries nothing has been transmitted, so forward-filling the last closed group is the
   honest curve, not an artifact to smooth away.

2. **`grz2_fine_cum_wire` is a different codec instance.** It re-runs GRZ2 at `--gtu 8` for
   curve resolution (33-276 points). It costs real bytes and is **not** the `/goal` wire:
   range-v3 +14.2%, catch2 +18.3%, rocksdb +13.0%, eigen +28.2%, llvm +13.4%, godot +3.2%.
   Plot it dashed, label it shape-only. (It also quantifies a real deployment tradeoff:
   emitting every 8 TUs instead of every 112 costs 3-28% of the wire.)

3. **Axis identity is asserted, not assumed** — see the note under the endpoint table. The
   join refuses to emit P29 columns at all if the per-TU `raw` sequences disagree.

`grz2_cum_wire` and `grz2_fine_cum_wire` each add their stream framing (108 B, belonging to
no group) to the final row, so both terminate at the codec's true total.

The fourth dashboard line, the online-prediction curve, comes from `~/bakeoff/curves/` and
is the one input not verified here; check its TU indexing against manifest order before
overlaying it.

## Provenance

- GRZ2: `grz2g-selector enc ... -m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512
  --gadd 128 --hist 1024 -j 8 --curve` — the binding `/goal` configuration, `--curve` is a
  pre-existing feature.
- Fast interner: `production-fused --level 3 --curve`, an observation-only patch
  (`selector_fused_curve_instrument.py`). Identity control: the unpatched build reports
  identical `body`/`comp`/`miss`/`keys`/`occ` totals on all six, and the codec's own per-TU
  byte-exact reconstruction check reports `verify=PASS` on every pass.
- Godot needed larger interner tables (`FUSED_LINE_CAP_LOG2=23`, 2.64M distinct lines
  overflow the stock `1<<21`). Verified wire-neutral: the enlarged build emits a
  **byte-identical curve** on range-v3.
