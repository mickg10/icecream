# Per-TU Root / Need / Fill byte split (cap_m5)

Where every byte of the per-TU C<->F dialogue goes, per TU, byte-exact, for the
daemon-rate / generator lane.

## Instrumentation and its gate

Observation-only counters added to `cap_m5_main.cpp`: a snapshot of the per-frame-kind
ledger when a job is queued, differenced when it commits. No encoder input, no control
flow, no ordering change. Patcher `selector_m5_split_instrument.py`, gate
`selector_m5split_gate.sh`.

The gate has three parts and all three are green on every run:

1. the stdout summary (frame ledger, closures, exactness flags, cache totals) is
   **identical** to the unpatched build's,
2. the **pre-existing** curve columns (`logical physical worker raw wire cumulative_raw
   cumulative_wire`) are identical,
3. every row's split closes against its own wire:
   `b_root + b_need + b_fill + b_control + b_carry == wire`.

`curve_cols=SAME` and `split_ok=N/N` on all 10 runs — 2,511 TUs x {workers=1, workers=8}
= **5,022 rows, every one closing exactly**.

Getting (3) to hold required finding a real accounting site, not just summing frames:
**`cap_m5_main.cpp:1681` charges each relationship's closing `Done` + summary `Ack` to
that worker's LAST accepted TU** (and `spawn_worker` charges `Hello` to the first
subsequent TU). A naive per-TU frame diff is short by exactly one session close per
worker — 1 bad row at `--workers 1`, 8 at `--workers 8`. That is now in `b_carry`, which
is why `b_carry` is a column and not an assertion.

New columns: `b_root b_need b_fill b_control b_carry split_ok c_root c_block c_path
c_region_control c_raw_run c_array_control c_array_values missing_regions`.

## The split

`--codec z1 --real-pipes`; share of the whole relationship's wire.

| corpus | W | TUs | raw | wire | root | need | fill | ctrl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| range-v3 | 1 | 259 | 632,049,016 | 1,820,664 | 37.46% | 1.18% | **60.98%** | 0.36% |
| range-v3 | 8 | 259 | 632,049,016 | 7,076,645 | 15.55% | 1.26% | **83.07%** | 0.09% |
| cereal | 1 | 84 | 326,899,429 | 1,120,409 | 33.61% | 0.90% | **65.30%** | 0.18% |
| cereal | 8 | 84 | 326,899,429 | 5,807,880 | 7.65% | 1.16% | **91.12%** | 0.03% |
| catch2 | 1 | 857 | 947,252,235 | 2,384,393 | 47.59% | 2.65% | **48.80%** | 0.95% |
| catch2 | 8 | 857 | 947,252,235 | 8,010,182 | 24.98% | 1.59% | **73.12%** | 0.28% |
| duckdb | 1 | 689 | 1,985,715,205 | 13,089,408 | 23.60% | 3.52% | **72.74%** | 0.14% |
| duckdb | 8 | 689 | 1,985,715,205 | 27,705,253 | 14.00% | 2.25% | **83.68%** | 0.07% |
| rocksdb | 1 | 622 | 3,114,320,596 | 15,692,955 | 40.80% | 8.56% | **50.53%** | 0.10% |
| rocksdb | 8 | 622 | 3,114,320,596 | 26,831,854 | 28.17% | 5.77% | **65.99%** | 0.06% |

- **Fill is the wire.** 48.8-72.7% at one consumer, 66.0-91.1% at eight.
- **Need is nearly free** (0.9-8.6%) and **control is noise** (0.03-0.95%). The
  request/response round trip is not what costs bytes; the definitions it pulls are.

### What Fill is made of (share of TOTAL wire, workers=1)

| corpus | root token stream | block defs | path defs | region control | raw runs (literal text) | arrays | missing regions/TU |
|---|---:|---:|---:|---:|---:|---:|---:|
| range-v3 | 1.75% | 35.59% | 1.42% | 15.55% | **43.75%** | 0.03% | 40.4 |
| cereal | 0.90% | 32.65% | 0.67% | 15.38% | **49.08%** | 0.06% | 61.8 |
| catch2 | 2.79% | **44.50%** | 1.03% | 15.63% | 31.49% | 0.07% | 35.2 |
| duckdb | 3.98% | 19.57% | 0.76% | 14.89% | **46.10%** | 10.89% | 304.9 |
| rocksdb | 9.79% | 30.97% | 0.37% | 21.51% | 28.57% | 0.01% | 1031.4 |

The definition payload is **literal text plus block structure** — raw runs 28.6-49.1% and
block definitions 19.6-44.5% of the entire wire between them. Region control is a steady
14.9-21.5%. Path definitions are negligible (<1.5%). The Root token stream itself — the
part that references already-defined material — is 0.9-9.8%.

## The marginal cost of a consumer is a Fill cost

Same corpus, same order, one consumer versus eight:

| corpus | wire x | Root x | Need x | **Fill x** |
|---|---:|---:|---:|---:|
| range-v3 | 3.89 | 1.61 | 4.15 | **5.29** |
| cereal | 5.18 | 1.18 | 6.70 | **7.23** |
| catch2 | 3.36 | 1.76 | 2.02 | **5.03** |
| duckdb | 2.12 | 1.26 | 1.35 | **2.44** |
| rocksdb | 1.71 | 1.18 | 1.15 | **2.23** |

Root barely moves (1.18-1.76x): it is emitted once per TU whatever the fan-out. Fill
multiplies by 2.2-7.2x because **every relationship that lacks a definition must be sent
its own copy.** This is the byte-level form of the transition-matrix result that the cost
is having consumers rather than when they join, and it says exactly where the generator
lane should aim: sharing the definition store across relationships attacks 2.2-7.2x of
the wire, while anything that improves the Root reference stream attacks at most 1.8x.

## Cold -> warm: the wire becomes a pure reference stream, then relapses

Per-decile, `--workers 1`. Two representative corpora:

**cereal** — the clean asymptote.

| decile | wire | raw/wire | root | need | fill | missing/TU |
|---|---:|---:|---:|---:|---:|---:|
| 0-10% | 710,529 | 45 | 4.62% | 1.19% | 94.16% | 589.9 |
| 10-20% | 12,986 | 2453 | 1.54% | 1.42% | 95.56% | 7.0 |
| 40-50% | 31,814 | 1009 | 42.55% | 1.42% | 55.34% | 22.0 |
| 50-60% | 56,508 | 492 | 91.42% | 0.43% | 7.81% | 11.2 |
| 60-70% | 65,864 | 485 | **99.37%** | 0.11% | **0.23%** | **0.0** |
| 90-100% | 74,181 | 483 | **99.14%** | 0.11% | **0.23%** | **0.0** |

63% of cereal's entire wire is spent in its first 10% of TUs. After that the definition
traffic goes to **zero** and the wire is **99.4% Root** — a pure reference stream. That is
the steady state the daemon lane is designed around, and it is reached.

**rocksdb** — the relapse.

| decile | wire | raw/wire | root | need | fill | missing/TU |
|---|---:|---:|---:|---:|---:|---:|
| 0-10% | 3,683,772 | 103 | 28.10% | 7.97% | 63.89% | 2325.5 |
| 30-40% | 1,189,077 | 276 | 46.78% | 1.82% | 51.25% | 168.9 |
| 60-70% | 731,403 | 372 | 60.23% | 1.52% | 38.01% | 78.7 |
| 80-90% | 1,083,475 | 261 | 48.40% | 7.09% | 44.36% | 585.2 |
| 90-100% | 1,686,084 | 158 | 37.39% | 13.79% | 48.71% | **1739.4** |

rocksdb warms up through the middle of the build and then **goes back into bootstrap**:
missing regions per TU rises 78.7 -> 585.2 -> 1739.4 and compression falls 372x -> 158x.
duckdb (novelty burst across deciles 40-80%) and catch2 (450 KB in decile 80-90% after
6 KB in the one before) do the same thing.

**"Warm" is not a state the relationship reaches and holds — it is per-material.** A build
that walks into a subsystem the receiver has never seen re-enters bootstrap for as long as
that subsystem lasts. Any daemon-rate model that assumes a monotone warm-up will
mis-predict the tail on exactly the corpora that matter most (rocksdb spends its last
decile at 1739 missing definitions per TU).

## Files

- `m5-pertu-split/<corpus>.w{1,8}.curve.tsv` — the per-TU rows, all columns.
- `m5-pertu-split/split-table.txt` — the generated tables above, including every decile.
- `selector_m5_split_instrument.py`, `selector_m5split_gate.sh`,
  `selector_m5split_run.sh`, `selector_m5split_table.py` — patcher, gate, runner, tables.
