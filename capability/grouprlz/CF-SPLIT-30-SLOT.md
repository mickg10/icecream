# Per-TU split on the C→F-only wire, at the binding 30-slot cold shard

Republished per local-oracle's ask: routing-first accepted, the owner's **30-slot cold
shard made binding**, and the byte split taken on the **C→F direction only**.

Separating directions needed a real instrumentation change, not a re-cut of the old
numbers: `Ack` is the one frame **both** sides send, so keying on frame type alone cannot
tell C→F from F→C. The ledger now carries `bytes_out` / `bytes_in` alongside `bytes`
(`bytes_out[i] + bytes_in[i] == bytes[i]` by construction, so nothing existing changes
value), and the per-TU rows carry both invariants.

**Gate — all four configurations, `--workers` 1 / 8 / 30 / latejoin:** stdout summary and
the pre-existing curve columns byte-identical to the unpatched build, and on **every row of
every run**:

```
b_root + b_need + b_fill + b_control + b_carry == wire      (split_ok)
cf_total + fc_total + b_carry               == wire         (dir_ok)
```

16 runs, 8 corpora, **100% of rows closing on both**.

The first gate run failed one row per worker on *both* invariants and the failures were on
*different* rows — a genuine double-count: the carried control bytes were being charged to
`b_carry` for the kind split and to `cf/fc` for the direction split at once. The carry is
now a single undirected term shared by both invariants, which is why both close.

## How much of the wire is C→F at all

| corpus | TUs | wire (1 slot) | C→F | share | C→F (30 slots) | share |
|---|---:|---:|---:|---:|---:|---:|
| cereal | 84 | 1,120,409 | 1,108,638 | 98.95% | 19,969,165 | 98.77% |
| range-v3 | 259 | 1,820,664 | 1,794,099 | 98.54% | 18,276,808 | 98.66% |
| catch2 | 857 | 2,384,393 | 2,304,142 | 96.63% | 18,790,157 | 98.49% |
| eigen | 650 | 3,289,527 | 3,246,460 | 98.69% | 41,508,730 | 98.76% |
| duckdb | 689 | 13,089,408 | 12,614,499 | 96.37% | 54,132,079 | 98.26% |
| llvm | 1238 | 14,614,029 | 14,439,885 | 98.81% | 67,340,529 | 98.81% |
| rocksdb | 622 | 15,692,955 | 14,336,905 | 91.36% | 51,995,991 | 96.24% |
| godot | 2207 | 62,871,696 | 62,579,277 | 99.53% | 156,562,092 | 99.20% |

**The C→F wire is 91.4-99.5% of the total** (rocksdb the low outlier, at 8.6% return
traffic because of its heavy Need volume). Restricting to C→F therefore does not change any
earlier conclusion — it tightens the numbers by a few percent and confirms the return
channel is not a design concern.

## The binding case: one cold build across 30 slots

| corpus | C→F 1 slot | C→F 30 slots | x | avoidable | Fill share | shard efficiency |
|---|---:|---:|---:|---:|---:|---:|
| cereal | 1,108,638 | 19,969,165 | 18.01x | **94.4%** | 96.9% | 0.881 |
| range-v3 | 1,794,099 | 18,276,808 | 10.19x | 90.2% | 92.9% | 0.510 |
| catch2 | 2,304,142 | 18,790,157 | 8.15x | 87.7% | 88.3% | 0.475 |
| eigen | 3,246,460 | 41,508,730 | 12.79x | 92.2% | 80.5% | 0.593 |
| duckdb | 12,614,499 | 54,132,079 | 4.29x | 76.7% | 91.8% | 0.174 |
| llvm | 14,439,885 | 67,340,529 | 4.66x | 78.6% | 89.2% | 0.200 |
| rocksdb | 14,336,905 | 51,995,991 | 3.63x | 72.4% | 84.6% | 0.185 |
| godot | 62,579,277 | 156,562,092 | 2.50x | **60.0%** | 94.5% | 0.084 |
| **MEAN** | | | | **81.5%** | | **0.388** |

**Under the binding constraint, a mean 81.5% of the C→F wire is avoidable** by routing
densely instead of spreading over all 30 slots — 60.0% at worst (godot), 94.4% at best
(cereal). The wire multiplier runs 2.50x to 18.01x.

*Shard efficiency* is the fraction of the whole build's definition bytes that **each** of
the 30 shards still has to be sent. At the mean of 0.388, a shard seeing 1/30 of the TUs
pays 39% of the definitions the whole build needed. That is the entire mechanism in one
number.

### C→F composition at 30 slots — it is definitions, not references

| corpus | cf_root | share | cf_fill | share | cf_control | missing-request x |
|---|---:|---:|---:|---:|---:|---:|
| cereal | 623,699 | 3.1% | 19,344,962 | **96.9%** | 504 | 25.88x |
| range-v3 | 1,296,932 | 7.1% | 16,978,191 | 92.9% | 1,685 | 12.60x |
| catch2 | 2,198,732 | 11.7% | 16,585,554 | 88.3% | 5,871 | 4.83x |
| eigen | 8,089,346 | 19.5% | 33,414,962 | 80.5% | 4,422 | 20.59x |
| duckdb | 4,444,351 | 8.2% | 49,683,033 | 91.8% | 4,695 | 2.22x |
| llvm | 7,250,759 | 10.8% | 60,081,232 | 89.2% | 8,538 | 6.26x |
| rocksdb | 7,994,812 | 15.4% | 43,996,953 | 84.6% | 4,226 | 1.50x |
| godot | 8,584,758 | 5.5% | 147,962,013 | **94.5%** | 15,321 | 5.68x |

**80.5-96.9% of the C→F wire at 30 slots is Fill.** Control is noise (under 16 KB
everywhere). Missing-definition requests multiply 1.50x-25.88x.

## The finding that should change the constraint

Shard efficiency is **not** constant, and it moves with corpus compressibility in the
direction that hurts most:

| corpus | whole-program raw/z19 | TUs per shard | shard efficiency |
|---|---:|---:|---:|
| eigen | 2846x | 21.7 | 0.593 |
| catch2 | 1183x | 28.6 | 0.475 |
| range-v3 | 853x | 8.6 | 0.510 |
| cereal | 694x | 2.8 | 0.881 |
| rocksdb | 502x | 20.7 | 0.185 |
| llvm | 496x | 41.3 | 0.200 |
| duckdb | 280x | 23.0 | 0.174 |
| godot | 103x | 73.6 | 0.084 |

**The corpora that compress best are the ones sharding hurts most.** A highly redundant
build is redundant precisely because nearly every TU draws on the same definitions — so
nearly every shard needs nearly all of them, and splitting multiplies the wire almost by
the shard count. A low-redundancy build like godot has definitions that genuinely belong to
particular TUs, so a shard only needs its own share and efficiency falls to 0.084.

This is the same variable that sets the cold C-encode rate bar, acting in the opposite
direction: **novel-material density makes a corpus slow to encode but cheap to shard, and
redundancy makes it fast to encode but expensive to shard.** No single fixed shard count is
right for both regimes.

**Consequence for the 30-slot constraint:** taken as a fixed number it is worst exactly
where the codec is strongest. cereal at 30 slots pays 18.01x for a build of 84 TUs — 2.8
TUs per shard. The constraint should bind as a *ceiling* on concurrency, with the actual
shard count chosen per build; the measurement says pick it from build size **and**
redundancy, not from the slot count.

## Files

`cf-split-30-slot/cfsplit.tsv` (8 corpora x {1,30} slots, totals and both gate columns),
`cf-split-30-slot/<corpus>.s{1,30}.tsv` (the per-TU rows, all columns),
`cf-split-30-slot/cf-report.txt`. Instrumentation `selector_m5_dir_instrument.py`, gate
`selector_m5dir_gate.sh`, runner `selector_cfsplit.sh`, tables `selector_cfreport.py`.
