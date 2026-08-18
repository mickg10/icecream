# Queued items 2 and 3

## 2. Small-corpus GRZ2 as the per-cell third candidate

Scope is exactly the 19 docker cells where the frozen GRZ2 encode is below 1 GB/s;
everywhere else the frozen policy already passes and no third candidate is needed.

**The framing "kept iff rate-legal AND within a size delta" turns out not to be a trade
on these cells, because there is nothing to trade against.** On all 19, the frozen GRZ2
encode is sub-1 GB/s by construction and P29+BSC is 0/44 on the C gate. So the third
candidate is not competing with a legal alternative -- it is the *only* legal option, on
the 13 cells where it clears the gate.

| outcome | cells |
|---|---|
| third candidate rate-legal, ships | **13 of 19** |
| nothing rate-legal at all | 6 (re2 x3, fmt x3 -- all <= 131 MB) |

Total size cost of shipping those 13: **+1,755,364 bytes**.

The cost is very unevenly distributed, so a delta cap still matters -- not to choose
between candidates, but to decide whether missing the deadline is preferable to a much
larger wire:

| delta cap | kept | size cost | dropped |
|---|---:|---:|---|
| +10% | 2/13 | +81,760 | most |
| +15% | 8/13 | +583,033 | leveldb/fedora, json/fedora, spdlog x3 |
| **+25%** | **10/13** | **+865,405** | **spdlog x3 only** |
| +60% | 13/13 | +1,755,364 | none |

**A +25% cap is the natural setting**: it keeps everything except the three spdlog cells,
which are the +58-60% outliers. And spdlog makes the point sharply -- its small-policy
wire (794,922-803,799) is **larger than P29+BSC's** (698,032-715,752). If P29 were ever
rate-legal there it would be the better choice; it is not, so on spdlog the real options
are a 59%-larger wire or a missed deadline. That is an owner call, not a codec one.

## 3. TU100 / TU200 chronological checkpoints

What each codec would actually have emitted by TU N, measured by encoding the real
prefix, against `zstd -6 --long=31` on the same raw prefix. Ten fixed-16 corpora have
>= 100 TUs; nine have >= 200.

**19 of 19 eligible checkpoints PASS, none marginally.** The best-of-two ratio to the cap
ranges 0.570 to 0.827 -- every checkpoint is 17% to 43% under its cap.

| corpus | TU100 min / cap | TU200 min / cap |
|---|---|---|
| llvm | 1,117,670 / 1,614,405 = 0.692 | 1,998,490 / 2,907,907 = 0.687 |
| rocksdb | 2,123,641 / 3,633,246 = **0.585** | 2,907,059 / 5,099,581 = **0.570** |
| duckdb | 1,415,869 / 2,144,664 = 0.660 | 1,883,043 / 2,841,123 = 0.663 |
| abseil | 976,356 / 1,586,360 = 0.616 | 1,327,302 / 2,212,343 = 0.600 |
| opencv | 529,760 / 640,543 = **0.827** | 1,134,024 / 1,525,959 = 0.743 |
| godot | 931,434 / 1,280,930 = 0.727 | 1,578,609 / 2,196,838 = 0.719 |
| catch2 | 377,439 / 539,019 = 0.700 | 424,532 / 618,686 = 0.686 |
| range-v3 | 404,700 / 610,361 = 0.663 | 575,691 / 883,458 = 0.652 |
| eigen | 521,935 / 743,718 = 0.702 | 734,912 / 1,099,434 = 0.668 |
| simdjson | 850,497 / 1,156,884 = 0.735 | (153 TUs) |

Six corpora are shorter than 100 TUs and have no checkpoint (fmt 50, spdlog 34, re2 72,
leveldb 72, cereal 84, nlohmann-json 99).

### The checkpoint result reinforces the runway finding

**GRZ2 is the smaller codec at every one of the 19 checkpoints** -- including on Godot,
LLVM and Eigen, the three corpora where P29+BSC wins the *complete* program. P29 never
leads early; it only overtakes in the tail.

That is the same statement as the surviving selector signal, seen from the other end: at
TU100 and TU200 no runway has been consumed yet, so P29's structural model has not had
the job count to amortize over. It also means the chronological cap is not the binding
constraint anywhere -- GRZ2 alone satisfies it with 17-43% headroom on every eligible
corpus, so the chronological gate does not constrain the selector design.
