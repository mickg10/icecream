# Mixed warmth: what a warm daemon is worth, and only if you route to it

Neither the pure-cold nor the pure-rebuild arm is the scheduler's real decision. This is:
a build arrives at a farm where **one daemon already has this project's history and the
rest are cold**, and the scheduler chooses how many to use.

`--repetitions 2 --latejoin-at N` (N = TUs in one pass): pass 1 runs entirely on worker 0,
so it alone ends warm; workers 1..k−1 spawn cold at the boundary and share pass 2. Pass-2
wire is therefore exactly *what this build costs on a farm with 1 warm + (k−1) cold*.
`k=1` is the dense-to-warm baseline — use only the daemon that already knows the project.

All rows close both invariants (`split_ok`, `dir_ok`) on 100% of rows, 8 corpora x 5 fan-outs.

## The headline

| corpus | route to the warm one | spread over 30 | x | cold 30, no warm | what the warm daemon buys when you still spread |
|---|---:|---:|---:|---:|---:|
| cereal | 40,266 | 19,670,582 | **488.5x** | 20,217,591 | −2.7% |
| range-v3 | 1,391,972 | 18,142,916 | 13.0x | 18,524,428 | −2.1% |
| catch2 | 526,877 | 18,464,546 | 35.0x | 19,078,143 | −3.2% |
| eigen | 8,652,556 | 42,199,817 | 4.9x | 42,029,723 | +0.4% |
| duckdb | 3,218,655 | 54,571,661 | 17.0x | 55,090,705 | −0.9% |
| rocksdb | 8,613,479 | 54,442,418 | 6.3x | 54,028,700 | +0.8% |
| llvm | 8,590,657 | 67,992,737 | 7.9x | 68,150,955 | −0.2% |
| godot | 10,882,123 | 160,466,272 | 14.7x | 157,822,682 | +1.7% |
| **MEDIAN** | | | **13.9x** | | **−0.6%** |

**A warm daemon in a farm of 30 is worth nothing — −0.8% mean, and it goes the wrong way on
three corpora — if the build still spreads across all 30. The same warm daemon is worth
4.9x to 488x if the build is routed to it.**

Warmth is not a property of the farm. It is only realised by the routing decision. A
scheduler that tracks warmth but keeps spreading has bought itself a rounding error.

## Bytes per TU, by who served it

The clean way to see it: what does one TU cost depending on which kind of daemon takes it?

| corpus | k | warm B/TU | cold B/TU | penalty |
|---|---:|---:|---:|---:|
| cereal | 2 → 30 | 496 → 121 | 25,657 → 242,842 | 51.7x → **2001x** |
| range-v3 | 2 → 30 | 5,364 → 5,401 | 12,715 → 72,377 | 2.4x → 13.4x |
| catch2 | 2 → 30 | 634 → 676 | 3,914 → 22,276 | 6.2x → 32.9x |
| eigen | 2 → 30 | 13,331 → 13,055 | 18,507 → 66,740 | 1.4x → 5.1x |
| duckdb | 2 → 8 | 4,575 → 4,032 | 26,410 → 42,622 | 5.8x → 10.6x |
| llvm | 30 | 6,553 | 56,578 | 8.6x |
| godot | 2 → 30 | 4,920 → 4,648 | 23,494 → 75,069 | 4.8x → **16.2x** |

Two things fall out:

- **A warm TU costs the same no matter how big the farm is.** range-v3 5,374 → 5,401,
  eigen 13,312 → 13,055, godot 4,931 → 4,648 across a 30x fan-out. The cost of a warm TU is
  a property of the TU, not of the farm.
- **A cold TU gets more expensive the more cold daemons you recruit** — 4.8x to 16.2x worse
  than a warm TU at k=30, because each additional cold daemon serves fewer TUs and so
  amortises its own definition set over less work.

So the marginal decision is concrete: moving one TU off the warm daemon onto a cold one
costs `cold_B/TU − warm_B/TU`, which at 30 slots is ~67 KB on range-v3, ~50 KB on llvm,
~70 KB on godot. That is the price of the parallelism, per TU, and the scheduler can now
weigh it against the compile time it buys.

## Marginal cost of recruiting cold daemons (pass-2 wire relative to k=1)

| corpus | k=2 | k=4 | k=8 | k=30 |
|---|---:|---:|---:|---:|
| cereal | 27.3x | 64.7x | 131.9x | 488.5x |
| range-v3 | 1.7x | 2.9x | 4.7x | 13.0x |
| catch2 | 3.7x | 7.9x | 13.7x | 35.0x |
| eigen | 1.2x | 1.5x | 2.1x | 4.9x |
| duckdb | 3.3x | 5.4x | 8.1x | 17.0x |
| rocksdb | 1.7x | 2.3x | 3.0x | 6.3x |
| llvm | 1.8x | 2.6x | 3.7x | 7.9x |
| godot | 2.9x | 5.0x | 8.9x | 14.7x |

Even the *first* cold recruit costs 1.2x–27.3x. There is no free second daemon.

## The two giants confirm the cold density curve, and move the mean

llvm and godot are now on the cold density sweep, which changes the headline slightly and
sharpens the match to bigoracle's number:

| corpus | k=2 | k=4 | k=8 | k=16 | k=32 |
|---|---:|---:|---:|---:|---:|
| catch2 | 32.4% | 56.6% | 70.2% | 80.6% | 87.7% |
| cereal | 40.3% | 65.0% | 80.7% | 89.9% | 94.8% |
| duckdb | 18.2% | 37.0% | 52.8% | 66.8% | 77.6% |
| eigen | 41.2% | 66.9% | 80.9% | 88.3% | 92.5% |
| **godot** | 10.7% | 23.3% | 36.6% | 49.6% | 61.2% |
| **llvm** | 18.5% | 36.9% | 54.3% | 68.7% | 79.4% |
| range-v3 | 33.6% | 58.9% | 74.3% | 84.4% | 90.6% |
| rocksdb | 11.7% | 25.8% | 41.5% | 57.8% | 72.2% |
| **MEAN (8)** | **25.8%** | **46.3%** | **61.4%** | **73.3%** | **82.0%** |

The giants sit at the bottom of the range — godot is the least penalised corpus in the set
(61.2% at k=32 against cereal's 94.8%), consistent with low-redundancy builds sharding more
cheaply. With them included, **bigoracle's ~60.8% now lands essentially exactly on k=8
(61.4%)** rather than on the interpolated k≈6 the six-corpus set implied.

## What this adds to the scheduler design

The two levers do not merely coexist; they gate each other.

1. **Density without affinity is most of the win on a cold farm** — 61.4% at 8-way, 82.0%
   at 32-way.
2. **Affinity without density is worth nothing** — −0.8% mean. Tracking which daemon is
   warm and then spreading anyway buys a rounding error.
3. **Affinity with density is the largest effect measured in this lane** — 4.9x to 488x,
   median 13.9x. This dwarfs both the cold density effect and the earlier rebuild-pinning
   result, because it compounds them.
4. The per-TU numbers give the scheduler an actual price: **~50-70 KB per TU moved off a
   warm daemon at 30 slots**, against the compile time that TU's parallelism buys.

Same caveat as the density sweep: transport makespan worsens with fan-out here because the
C side is the bottleneck and the consumers are pipe sinks, not compilers. Nothing in this
document says builds get slower — it says the transport never pays you back, so spreading
must be justified by compile concurrency alone.

## Files

`mixed-warmth/mixedwarm.tsv` (8 corpora x {1,2,4,8,30}, pass 1 / pass 2 separated, warm-
and cold-served bytes and TU counts split out), `mixed-warmth/mixed-report.txt`,
`dense-affinity/affinity.tsv` (now including llvm and godot),
`dense-affinity/affinity-report.txt`. Harness `selector_mixedwarm.sh`, tables
`selector_mixedreport.py`; instrumentation `selector_m5_dir_instrument.py` and its gate
`selector_m5dir_gate.sh`.
