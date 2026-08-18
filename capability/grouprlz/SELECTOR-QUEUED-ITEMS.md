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

### Scope: this is the isolated-single-build case only

Everything above assumes each cell is encoded by its own short-lived process, so the
per-invocation setup is paid once per build and cannot be spread. The cost model measured
earlier says exactly what happens when it can be:

```
frozen GRZ2 encode:  time = 0.2241 s + raw / 1.575 GB/s
```

In a streaming farm -- a long-lived encoder handling job after job -- the 0.2241 s setup
is paid once for the daemon, not once per build, so each corpus is charged only its
marginal term and every cell encodes at the asymptotic **1.575 GB/s** regardless of size.
Under that deployment **the frozen GRZ2 policy is rate-legal on all 44 cells and the third
candidate is not needed at all**, along with its +1,755,364 bytes.

So the third candidate is a fix for one specific deployment shape: isolated builds, each
paying their own setup. The spdlog dilemma -- a 59%-larger wire or a missed deadline --
exists only in that shape. In a streaming farm spdlog ships the frozen wire at full rate
and the question does not arise. Worth settling which deployment the gate is meant to
describe before paying any size for it.

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

---

## Strategic check: does the classifier gate the size result at all?

Raised in review: if running both codecs (policy A) is affordable once P29 is rate-legal,
per-cell min gives the oracle 0.9705x with **no classifier at all**. Measured against the
rate-pass data rather than argued.

Fixed 16-core budget, disjoint 8 P29 / 8 GRZ, GRZ on the encode-only (in-memory) basis,
P29 charged both passes, summed over all 44 docker cells (74.54 GB):

| policy | makespan | GB/s | core-s per raw GiB |
|---|---:|---:|---:|
| **A: run both complete, take per-cell min** | 201.3 s | 0.370 | **3.72** |
| **B: TU112 probe, then finish only the selected** | 199.7 s | 0.373 | **3.67** |
| GRZ complete alone (encode-only) | 57.2 s | 1.304 | |
| P29 complete alone (two-pass) | 201.3 s | 0.370 | |

**Policy A costs 1.02x the core-seconds of policy B and +0.8% makespan.**

The reason is structural: **P29 is the slower side on 44 of 44 cells**, so
`max(P29, GRZ) = P29` everywhere and running GRZ concurrently on its own cores is free in
wall time -- it finishes in 57 s while P29 is still working. Meanwhile policy B does not
actually save that work: it still pays the probe race *and* the finish.

Two consequences:

1. **The classifier is off the critical path for the size result.** Policy A is rate-legal
   exactly when P29 is, because P29 sets the makespan on every cell. So the moment P29
   becomes rate-legal, policy A is legal *by construction* and delivers the oracle
   0.9705x z19 with no classification decision at all -- for about 2% more CPU.
2. **The classifier only earns its place if core-seconds, not makespan, are the binding
   resource.** At 3.72 versus 3.67 core-s per raw GiB the difference is not close to
   justifying a rule we cannot yet validate.

This does not make the classifier work wasted -- policy B remains the cheaper design if
the C side is CPU-constrained rather than latency-constrained, and the runway finding is
the honest state of that question. But it does mean the selector's size advantage is
**not blocked on the classifier**. It is blocked, like everything else in this lane, on
P29's encode throughput.

---

## Ready-to-run: the one remaining docker measurement

`selector_policyA.sh <project> <profile>` runs both codecs to completion concurrently on
the fixed 16-core budget (disjoint 8 P29 / 8 GRZ), takes the per-cell minimum, and reports
P29's own rate, policy A's rate, the selected bytes, and the GRZ round-trip. No classifier
is involved -- policy A yields the per-cell oracle by construction. To point it at the
fast-interner build, set `P29=<path>`; nothing else changes.

Validated on two cells with today's research-interner two-pass P29:

| project | profile | P29 | GRZ2 | min | winner | P29 GB/s | GRZ GB/s | **policy A GB/s** | gate | exact |
|---|---|---:|---:|---:|---|---:|---:|---:|---|---|
| re2 | debian-gcc | 453,331 | 362,280 | 362,280 | GRZ2 | 0.110 | 0.529 | **0.109** | slow | YES |
| rocksdb | debian-gcc | 2,468,928 | 2,535,516 | 2,468,928 | P29BSC | 0.330 | 1.080 | **0.330** | slow | YES |

Note `policy A GB/s == P29 GB/s` on both rows, to three decimals. That is the structural
claim confirmed by direct measurement rather than by arithmetic on separate runs: **policy
A's makespan is set entirely by P29**, so it clears the gate exactly when P29 does, and
GRZ2's complete encode is genuinely free alongside it.

Both rows also reproduce their census labels (GRZ2 wins re2, P29+BSC wins rocksdb/docker),
so the runner is consistent with the frozen size ledger.

When the fast interner exists, sweeping all 44 answers the question in one pass: does P29
clear >= 1 GB/s, and does policy A therefore deliver 0.9705x z19 at rate.
