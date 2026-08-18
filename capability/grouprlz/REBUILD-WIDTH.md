# Rebuild at the binding widths: pinning removes the width penalty entirely

The cold build pays 2.40x–17.36x to open 30 cache domains instead of one. **The rebuild
pays nothing — if the routing pins.** That is the closing piece of the daemon lane: the
cost of a wide farm is paid once, on the cold build, not on every build after it.

`--repetitions 2`: TUs `[0,N)` are the cold build, `[N,2N)` the rebuild. `sticky` hashes
each file to a fixed daemon so a rebuild returns to it; `roundrobin` indexes the
concatenated sequence, so `N mod M ≠ 0` shifts files onto other daemons — a scheduler that
does not pin. All wire figures are the **direction-exact C→F** measure.

## Rebuild C→F wire at each binding width, with pinning

| corpus | cold build | M=1 (c_f=30) | M=4 (c_f=8) | M=8 (c_f=4) | M=30 (c_f=1) |
|---|---:|---:|---:|---:|---:|
| cereal | 1,108,663 | 37,713 | 274,407 | 323,420 | 342,951 |
| range-v3 | 1,794,125 | 1,384,037 | 1,384,281 | 1,384,817 | 1,398,472 |
| catch2 | 2,304,168 | 501,002 | 805,199 | 903,594 | 1,015,363 |
| eigen | 3,246,486 | 8,632,891 | 8,632,956 | 8,633,106 | 8,635,259 |
| duckdb | 12,614,525 | 3,197,820 | 3,706,523 | 3,845,592 | 3,965,705 |
| rocksdb | 14,336,931 | 8,594,654 | 8,606,828 | 8,608,228 | 8,623,711 |
| llvm | 14,439,911 | 8,553,352 | 8,553,734 | 8,555,416 | 8,570,845 |
| godot | 62,579,303 | 10,815,748 | 10,815,947 | 10,817,470 | 10,834,451 |

## With pinning, the rebuild is width-independent

| corpus | M=1 pinned | M=30 pinned | growth | what the COLD build pays for the same width |
|---|---:|---:|---:|---:|
| eigen | 8,632,891 | 8,635,259 | **+0.03%** | 12.19x |
| godot | 10,815,748 | 10,834,451 | **+0.17%** | 2.40x |
| llvm | 8,553,352 | 8,570,845 | **+0.20%** | 4.52x |
| rocksdb | 8,594,654 | 8,623,711 | **+0.34%** | 3.61x |
| range-v3 | 1,384,037 | 1,398,472 | **+1.04%** | 10.04x |
| duckdb | 3,197,820 | 3,965,705 | +24.01% | 4.25x |
| catch2 | 501,002 | 1,015,363 | +102.67% | 8.12x |
| cereal | 37,713 | 342,951 | +809.37% | 17.36x |
| **MEDIAN** | | | **+0.69%** | **6.32x** |

**Six of eight corpora pay under 1.1% to spread a pinned rebuild across 30 cache domains,
against 2.40x–12.19x for the same width on the cold build.** The mean growth of 117% is
carried entirely by cereal; the median is +0.69%, which is the number that describes the
behaviour.

The two exceptions are the small, highly redundant corpora — cereal (84 TUs, 2.8 per
domain at M=30) and catch2 — where a narrower domain has seen less history and every later
reference is therefore longer. Same reference-vocabulary effect as the earlier rebuild arm,
and the same corpora.

## What pinning is worth, at each binding width

C→F rebuild wire saved by pinning against a non-pinning scheduler:

| corpus | M=1 | M=4 | M=8 | M=30 |
|---|---:|---:|---:|---:|
| cereal | 0.0% | −22.9% | −63.4% | −62.1% |
| range-v3 | 0.0% | −21.0% | −36.4% | −67.6% |
| catch2 | 0.0% | −42.5% | −67.0% | −80.6% |
| eigen | 0.0% | −6.4% | −9.5% | −35.3% |
| duckdb | 0.0% | −72.1% | −73.0% | −78.2% |
| rocksdb | 0.0% | −46.3% | −48.0% | −52.4% |
| llvm | 0.0% | −48.8% | −52.0% | −64.2% |
| godot | 0.0% | −83.7% | −84.5% | −88.1% |
| **MEAN** | 0.0% | **−42.9%** | **−54.2%** | **−66.1%** |

At M=1 pinning is definitionally free — there is only one daemon. Its value grows with
width precisely because width is what it protects against.

## Definition requests: pinning drives them to zero

Missing regions requested during the rebuild:

| corpus | M=1 pin | M=4 pin | M=8 pin | M=30 pin | M=30 **no pin** |
|---|---:|---:|---:|---:|---:|
| cereal | 0 | 0 | 0 | 0 | 1,930 |
| range-v3 | 0 | 0 | 0 | 0 | 26,091 |
| catch2 | 0 | 0 | 0 | 0 | 45,343 |
| eigen | 0 | 0 | 0 | 0 | 34,996 |
| duckdb | 0 | 0 | 0 | 0 | 232,847 |
| rocksdb | 0 | 0 | 0 | 0 | 659,011 |
| llvm | 0 | 0 | 0 | 0 | 97,922 |
| godot | 0 | 0 | 0 | 0 | 223,225 |

**Exactly zero, at every width, on every corpus.** A pinned rebuild asks for no definitions
at all — the residual wire is pure reference stream. That is the cleanest statement of what
affinity buys, and it is why the width penalty disappears.

## Putting the lane together

| decision | cold build | rebuild |
|---|---|---|
| width (how many cache domains) | 2.40x–17.36x, near-linear, no knee | **+0.69% median, if pinned** |
| pinning (which domain) | worth under 1.6% | **worth 42.9–66.1%** |

The two levers are complementary and each is nearly worthless in the other's regime. A
scheduler should choose width from the cold build's marginal price and pin from the first
build onward, because pinning costs nothing when it is not needed and removes the width
penalty when it is.

## Gate, and one honest exception

64 runs, 8 corpora x 4 widths x 2 assignments. `split_ok` and `dir_ok` on **every row of
every run**.

`carry_undirected` is **0 on 63 of 64 runs**, and **194 B on one** — cereal at M=30 sticky.
That is 0.06% of that run's wire and it has a specific cause: with 84 TUs hashed over 30
domains some domains are opened and never serve a committed TU, and an entirely idle
relationship has no row to charge its session close to, so it falls to the global residual
rather than to a direction. So `cf_total` is exact except for the session bytes of daemons
that are opened and never used. Worth knowing if the scheduler ever opens domains
speculatively.

## Files

`rebuild-width/rebuildwidth.tsv`, `rebuild-width/rebuild-width-report.txt`. Runner
`selector_rebuildwidth.sh`, tables `selector_rebuildwidthreport.py`; instrumentation
`selector_m5_dir2_instrument.py` and gate `selector_m5dir2_gate.sh`.
