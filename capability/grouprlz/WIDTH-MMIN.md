# Cache-domain width: M_min(30), and where sharding stops paying

Folds in both of local-oracle's refinements.

**Slots are not cache domains.** The owner's 30 is 30 simultaneous compiler *slots*. Caches
opened = distinct selected F daemons = **M**, and the binding width is
`M_min(30) = min |S| s.t. Σ c_f ≥ 30`, where `c_f` = slots behind daemon `f`. So the M=30
row is the **c_f=1 thin-daemon spray diagnostic** — one end of the spectrum, not the
universal binding number. Feasible saving is measured against that spray, not against M=1.

This also re-labels the earlier `CF-SPLIT-30-SLOT.md`: its numbers are correct as
measurements, but its "30 slots" row is the c_f=1 diagnostic, not the binding case.

## M_min(30): the binding width, by slots-per-daemon

C→F wire at the binding width, and what it saves against the c_f=1 spray:

| corpus | c_f=1 → M_min=30 | c_f=4 → M_min=8 | c_f=8 → M_min=4 | c_f=30 → M_min=1 |
|---|---:|---:|---:|---:|
| cereal | 19,970,035 | 5,737,579 (−71.3%) | 3,162,974 (−84.2%) | 1,108,667 (−94.4%) |
| range-v3 | 18,277,708 | 6,981,504 (−61.8%) | 4,366,363 (−76.1%) | 1,794,129 (−90.2%) |
| catch2 | 18,791,057 | 7,864,635 (−58.1%) | 5,380,433 (−71.4%) | 2,304,172 (−87.7%) |
| eigen | 41,509,630 | 17,029,852 (−59.0%) | 9,818,578 (−76.3%) | 3,246,490 (−92.2%) |
| duckdb | 54,132,979 | 27,067,649 (−50.0%) | 20,230,753 (−62.6%) | 12,614,529 (−76.7%) |
| rocksdb | 51,996,891 | 25,269,808 (−51.4%) | 19,693,546 (−62.1%) | 14,336,935 (−72.4%) |
| llvm | 67,341,429 | 31,604,373 (−53.1%) | 22,897,858 (−66.0%) | 14,439,915 (−78.6%) |
| godot | 156,562,992 | 98,507,383 (−37.1%) | 81,442,848 (−48.0%) | 62,579,307 (−60.0%) |
| **MEAN** | baseline | **−55.2%** | **−68.3%** | **−81.5%** |

**Just making daemons four times fatter — c_f 1 → 4, which narrows M_min(30) from 30 to 8 —
already recovers a mean 55.2% of the C→F wire, with no change to the codec, the schedule or
the slot budget.** Eight slots per daemon recovers 68.3%. The 81.5% figure quoted earlier
is the c_f=30 corner, where all 30 slots sit behind one cache.

## The carry is now direction-exact

`cf_total` was previously a lower bound in `[C→F, C→F + carry]`. The ledger now attributes
the carry, and the two invariants stay independent because they use different carry terms:
the kind split uses `b_carry`, the direction split uses `cf_carry + fc_carry`, and
`cf_carry + fc_carry == b_carry`.

**`carry_undirected == 0` across all 48 runs**, so `cf_total` is exact, not a bound. The
per-daemon carry resolves to **29 B C→F** (Hello 25 + Done 4) and **165 B F→C** (the
summary Ack) — so the old bound was under-counting C→F by 29 B per daemon, 870 B at M=30,
rather than the full 195 B. Every conclusion drawn from the bound survives unchanged; the
ledger now owns the bytes either way.

(The first attempt at this double-counted — the carry went into `cf_control` while
`b_carry` was still in the direction invariant. The dual gate caught it on the first run,
which is the reason both invariants exist.)

## Width sweep: C→F wire by M

| corpus | raw/z19 | M=1 | M=2 | M=4 | M=8 | M=16 | M=30 |
|---|---:|---:|---:|---:|---:|---:|---:|
| cereal | 694 | 1,108,667 | 1.67x | 2.85x | 5.18x | 9.85x | 18.01x |
| range-v3 | 853 | 1,794,129 | 1.51x | 2.43x | 3.89x | 6.43x | 10.19x |
| catch2 | 1183 | 2,304,172 | 1.49x | 2.34x | 3.41x | 5.25x | 8.16x |
| eigen | 2846 | 3,246,490 | 1.70x | 3.02x | 5.25x | 8.57x | 12.79x |
| duckdb | 280 | 12,614,529 | 1.23x | 1.60x | 2.15x | 3.07x | 4.29x |
| rocksdb | 502 | 14,336,935 | 1.14x | 1.37x | 1.76x | 2.47x | 3.63x |
| llvm | 496 | 14,439,915 | 1.23x | 1.59x | 2.19x | 3.20x | 4.66x |
| godot | 103 | 62,579,307 | 1.12x | 1.30x | 1.57x | 1.98x | 2.50x |

## There is no knee — the answer is a price, not a width

Marginal cost of the next cache domain, as a share of the build's **own** M=1 C→F wire:

| corpus | raw/z19 | 1→2 | 2→4 | 4→8 | 8→16 | 16→30 |
|---|---:|---:|---:|---:|---:|---:|
| cereal | 694 | 67.4% | 59.0% | 58.1% | 58.5% | **58.3%** |
| eigen | 2846 | 70.2% | 66.1% | 55.5% | 41.6% | 30.1% |
| range-v3 | 853 | 50.8% | 46.3% | 36.4% | 31.8% | 26.8% |
| catch2 | 1183 | 49.0% | 42.2% | 27.0% | 22.9% | 20.8% |
| llvm | 496 | 22.7% | 17.9% | 15.1% | 12.6% | 10.5% |
| duckdb | 280 | 22.8% | 18.8% | 13.5% | 11.5% | 8.7% |
| rocksdb | 502 | 14.2% | 11.6% | 9.7% | 8.8% | 8.3% |
| godot | 103 | 11.9% | 9.1% | 6.8% | 5.1% | **3.7%** |

**The marginal price is near-constant, so the wire is near-linear in width and there is no
efficient point to name.** cereal pays 58% of its entire single-domain wire for every extra
cache domain, all the way out to 30. That is the honest answer to "where does sharding stop
paying": nowhere in particular — it stops paying as soon as the marginal price exceeds what
the extra compile concurrency is worth, and that price is a per-corpus constant the
scheduler can read off.

Set a threshold and the regimes separate cleanly:

| corpus | raw/z19 | widest M under 5% | under 10% | under 25% |
|---|---:|---:|---:|---:|
| cereal | 694 | 1 | 1 | 1 |
| range-v3 | 853 | 1 | 1 | 1 |
| catch2 | 1183 | 1 | 1 | 1 |
| eigen | 2846 | 1 | 1 | 1 |
| duckdb | 280 | 1 | 1 | **30** |
| rocksdb | 502 | 1 | 1 | **30** |
| llvm | 496 | 1 | 1 | **30** |
| godot | 103 | 1 | 1 | **30** |

**At a 25% marginal-price budget the low-redundancy builds can open all 30 cache domains
and the high-redundancy ones cannot open a second.** That is the ceiling-versus-target
question answered per regime rather than per farm: a single fixed width is wrong for both,
and the split falls exactly along whole-program compressibility — the same variable that
governs shard efficiency and the cold-encode rate bar.

## Fill share of the C→F wire, by width

| corpus | M=1 | M=2 | M=4 | M=8 | M=16 | M=30 |
|---|---:|---:|---:|---:|---:|---:|
| cereal | 66.0% | 78.7% | 87.0% | 92.2% | 95.3% | 96.9% |
| range-v3 | 61.9% | 69.8% | 77.7% | 84.2% | 89.3% | 92.9% |
| catch2 | 50.5% | 56.7% | 63.8% | 74.5% | 83.0% | 88.3% |
| eigen | 57.9% | 60.0% | 62.0% | 64.8% | 73.2% | 80.5% |
| duckdb | 75.5% | 78.5% | 82.3% | 85.7% | 89.2% | 91.8% |
| rocksdb | 55.3% | 58.2% | 63.2% | 70.1% | 77.8% | 84.6% |
| llvm | 69.4% | 71.7% | 75.6% | 80.5% | 85.3% | 89.2% |
| godot | 93.6% | 93.2% | 93.0% | 93.2% | 93.8% | 94.5% |

Widening turns the wire into definitions: every corpus rises toward 80-97% Fill. godot is
already 93.6% Fill at M=1 and stays flat — its wire is definitions whatever the width,
which is why it is also the cheapest to shard.

## Gate

48 runs, 8 corpora x 6 widths. `split_ok` and `dir_ok` on **every row of every run**, and
`carry_undirected` totals **0**. Summary and pre-existing curve columns byte-identical to
the unpatched build (`selector_m5dir2_gate.sh`, verified at `--workers` 1 / 8 / 30 /
latejoin).

## Files

`width/width.tsv` (one row per corpus per width, both gate columns) and
`width/width-report.txt`.  The per-TU rows are not committed -- this analysis is entirely
aggregate and `selector_width.sh` regenerates them in minutes; the per-TU evidence for the
daemon lane is already in `m5-pertu-split/` and `cf-split-30-slot/`.
Instrumentation `selector_m5_dir2_instrument.py`, gate `selector_m5dir2_gate.sh`, runner
`selector_width.sh`, tables `selector_widthreport.py`.
