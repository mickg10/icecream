# DENSE_AFFINITY: what routing a build to fewer F daemons actually saves

Measured, not modelled. `cap_m5` with the observation-only per-TU byte split, so every
number below is decomposed into Root / Need / Fill and **every row's split closes exactly
against its own wire** — 100% of rows in all 62 runs.

The wire is exact and deterministic. Makespan is recorded but the box has 32 cores, so
`k=32` runs the C side plus 32 real consumer pipes on 32 cores; treat wall time as
indicative.

## Result 1 — on a cold build, DENSITY is the whole game

Total C→F wire when one build is spread round-robin across `k` daemons, relative to `k=1`:

| corpus | TUs | k=1 wire | k=2 | k=4 | k=8 | k=16 | k=32 |
|---|---:|---:|---:|---:|---:|---:|---:|
| catch2 | 857 | 2,384,393 | 1.48x | 2.30x | 3.36x | 5.15x | 8.16x |
| cereal | 84 | 1,120,409 | 1.67x | 2.86x | 5.18x | 9.87x | **19.21x** |
| duckdb | 689 | 13,089,408 | 1.22x | 1.59x | 2.12x | 3.02x | 4.46x |
| eigen | 650 | 3,289,527 | 1.70x | 3.02x | 5.23x | 8.56x | 13.33x |
| range-v3 | 259 | 1,820,664 | 1.51x | 2.43x | 3.89x | 6.42x | 10.58x |
| rocksdb | 622 | 15,692,955 | 1.13x | 1.35x | 1.71x | 2.37x | 3.59x |

**Avoidable fraction** — the share of the wire that routing to one warm daemon instead of
`k` would not have sent at all, `(wire(k) − wire(1)) / wire(k)`:

| corpus | k=2 | k=4 | k=8 | k=16 | k=32 |
|---|---:|---:|---:|---:|---:|
| catch2 | 32.4% | 56.6% | 70.2% | 80.6% | 87.7% |
| cereal | 40.3% | 65.0% | 80.7% | 89.9% | 94.8% |
| duckdb | 18.2% | 37.0% | 52.8% | 66.8% | 77.6% |
| eigen | 41.2% | 66.9% | 80.9% | 88.3% | 92.5% |
| range-v3 | 33.6% | 58.9% | 74.3% | 84.4% | 90.6% |
| rocksdb | 11.7% | 25.8% | 41.5% | 57.8% | 72.2% |
| **MEAN** | **29.6%** | **51.7%** | **66.7%** | **78.0%** | **85.9%** |

**On bigoracle's ~60.8%:** it is not a single number, it is a point on this curve. 60.8%
sits between `k=4` (51.7%) and `k=8` (66.7%), i.e. it corresponds to a fan-out of about
**six**. At a realistic ~30-daemon spray the avoidable fraction is far larger — **85.9%**.
So the P0 is if anything understated, but only if the comparison is against a genuinely
warm single daemon.

### Where the bytes go, and why it is nearly linear in k

Multiplier against `k=1`, mean over the six corpora:

| k | Root x | Need x | Fill x | per-consumer Fill x |
|---:|---:|---:|---:|---:|
| 1 | 1.00 | 1.00 | 1.00 | 1.000 |
| 2 | 1.22 | 1.41 | 1.60 | 0.801 |
| 4 | 1.54 | 2.16 | 2.72 | 0.680 |
| 8 | 1.90 | 3.47 | 4.68 | 0.585 |
| 16 | 2.17 | 5.98 | 8.35 | 0.522 |
| 32 | 2.34 | 10.42 | **14.82** | **0.463** |

The fan-out cost is **Fill** — definitions, re-sent to each relationship that lacks them.
Root grows only 2.34x across a 32x fan-out because it is emitted once per TU regardless.

The last column is the mechanism. Split the build 32 ways and each daemon sees 1/32 of the
TUs — but still needs **46%** of the definition bytes a single daemon needed for the
*whole* build. The definition set is extremely concave in TU count, so `k × D(N/k)` grows
almost linearly in `k`. That is why spreading is close to paying `k` times for the same
definitions.

### Assignment policy at fixed fan-out buys essentially nothing

Wire at `k=8` against round-robin:

| corpus | round-robin | sticky | random |
|---|---:|---:|---:|
| catch2 | 8,010,182 | −1.10% | −1.59% |
| cereal | 5,807,880 | −0.03% | −0.21% |
| duckdb | 27,705,253 | −1.58% | −0.86% |
| eigen | 17,208,713 | −8.86% | −2.42% |
| range-v3 | 7,076,645 | −0.75% | −0.02% |
| rocksdb | 26,831,854 | −0.60% | −0.14% |

Under 1.6% everywhere except eigen. **On a cold build it is how MANY daemons you use, not
which ones or in what order.** The scheduler lever is density, full stop.

### Transport makespan gets worse too, not better

| corpus | k=1 | k=2 | k=4 | k=8 | k=16 | k=32 |
|---|---:|---:|---:|---:|---:|---:|
| catch2 | 1.27 | 1.80 | 1.82 | 2.08 | 2.39 | 2.80 |
| cereal | 0.41 | 0.57 | 0.64 | 0.88 | 1.28 | 2.15 |
| duckdb | 3.51 | 3.88 | 4.16 | 4.66 | 5.50 | 7.04 |
| eigen | 4.23 | 4.71 | 4.74 | 5.18 | 5.87 | 7.11 |
| range-v3 | 0.83 | 1.12 | 1.21 | 1.37 | 1.73 | 2.41 |
| rocksdb | 4.53 | 5.01 | 5.04 | 5.52 | 6.68 | 8.74 |

Fanning out makes the *transport* 1.7-5.2x slower, because the C side has to materialise
and entropy-code a separate Fill per consumer and it is the bottleneck. **This does not
say builds get slower** — this harness's consumers are pipe sinks, not compilers, and real
compilation is exactly what fan-out parallelises. It says the transport does not pay you
back for the extra wire; the win has to come entirely from compile parallelism.

## Result 2 — on a REBUILD, affinity is worth up to 3x, and it is a different lever

`--repetitions 2`: TUs `[0,N)` are the cold build, `[N,2N)` the rebuild. `sticky` hashes
each file to a fixed daemon so a rebuild returns to the same one; `roundrobin` indexes the
concatenated sequence, so `N mod k ≠ 0` shifts files onto different daemons — a fair model
of a scheduler that does not pin work.

Rebuild wire, sticky against round-robin at the same fan-out:

| corpus | k=4 | k=8 | k=32 |
|---|---:|---:|---:|
| range-v3 | −21.3% | −36.8% | **−66.3%** |
| cereal | −22.7% | −63.2% | −59.4% |
| catch2 | −43.5% | −67.0% | **−77.6%** |
| rocksdb | −50.3% | −51.8% | −55.9% |
| **MEAN** | **−34.5%** | **−54.7%** | **−64.8%** |

**This is the opposite of the cold result.** Cold, the routing pattern was worth under 2%;
on a rebuild the same choice is worth 35-65%. Cold builds care about density; rebuilds
care about affinity. A scheduler that optimises only one of them leaves the other on the
table.

With sticky routing the rebuild wire becomes nearly fan-out-independent for the larger
corpora — range-v3 1,391,972 (`k=1`) → 1,409,203 (`k=32`), **+1.2%**; rocksdb 8,613,479 →
8,652,919, **+0.5%** — but not for the small highly-redundant ones: catch2 +100%, cereal
+790%. The `p2_root` column shows why: cereal's rebuild Root stream grows 35,529 →
345,685 as it is split, while range-v3's and rocksdb's are flat. Where TUs share enough
structure, one daemon's fuller history makes every later reference shorter, so fan-out
charges a **second** time — through the reference stream, on every subsequent build.

Two honest limits on this half: the rebuild here re-sends *identical* files, which is the
best case for warmth; and `roundrobin`'s penalty depends on `N mod k`, so it models a
non-pinning scheduler rather than an adversarial one.

## What this says for the scheduler

1. **Density is the P0 and the measured saving is bigger than the claim** — 85.9% mean
   avoidable wire at a 30-way spray, 66.7% at 8-way. Route to the fewest warm daemons.
2. **Do not spend effort on clever cold routing.** At fixed fan-out, sticky and random are
   within 1.6% of round-robin. There is no cold-side ordering win to chase.
3. **Pin files to daemons across builds.** It is worth nothing cold and 35-65% warm, and it
   is cheap — a hash, which the harness already implements.
4. **The transport will not repay fan-out.** Wire and transport makespan both worsen
   monotonically with `k`; the only argument for spreading is compile parallelism, so the
   scheduler should spread exactly as far as compile concurrency demands and no further.

## Files

`dense-affinity/affinity.tsv` (cold sweep, 6 corpora x {1,2,4,8,16,32} x
{roundrobin,sticky,random}), `dense-affinity/rebuild.tsv` (4 corpora x {1,4,8,32} x
{sticky,roundrobin}, pass-1/pass-2 separated), `dense-affinity/affinity-report.txt`.
Harnesses `selector_affinity.sh`, `selector_affinity_rebuild.sh`,
`selector_affinity_report.py`; instrumentation and its gate
`selector_m5_split_instrument.py`, `selector_m5split_gate.sh`.

Known data defect, harmless here: the `raw` column of `affinity.tsv` is clamped at
2^31−1 for corpora above 2 GB (awk `%d`). Nothing in this analysis uses it; the script is
fixed for future runs.
