# GRZ2 production rate: the small-cell question, answered

**Verdict: the sub-1 GB/s small cells are fixed-cost bound, not a throughput floor, and
the fixed cost is 81% removable.**

## The cost model separates cleanly

Encode-only, input already in page cache (production holds the TU stream in memory, so
the disk materialization that dominated the charged number is not paid). Least-squares
fit of `encode_time = a + raw / T` over all 44 docker cells:

| policy | fixed cost `a` | asymptotic throughput `T` |
|---|---:|---:|
| frozen (`-b 8 -t 21 --hist 1024`) | **0.2241 s** | 1.575 GB/s |
| small-corpus (`-b 1 -t 16 -s 8 --hist 32`) | **0.0431 s** | 1.522 GB/s |

**The fixed cost falls 81% while the marginal throughput barely moves (-3.4%.)** That is
the proof: the small cells were paying a per-invocation setup cost -- the 16 MiB index
table and the 1 GiB history ring -- not running into a throughput limit. Under the frozen
policy the fixed cost is half the encode time at 353 MB raw, and every cell at or above
1 GB already measures 1.010-1.648 GB/s.

## What it buys on the 19 sub-1 GB/s cells

All 19 re-measured, 3 reps each, best kept, **all 19 decode byte-exact**:

| raw | cells | frozen GB/s | small-policy GB/s | speedup | crosses 1 GB/s |
|---|---|---|---|---|---|
| 85-131 MB | re2 x3, fmt x3 | 0.402-0.554 | 0.803-0.996 | 1.78-2.00x | **no** (6) |
| 175-428 MB | leveldb x4, fmt/fedora, re2/fedora, spdlog x3, json x4 | 0.564-0.911 | 1.117-1.400 | 1.36-2.07x | **yes** (13) |

**13 of 19 cross the gate.** Combined with the 25/44 that already passed encode-only at
the frozen policy, that is **38/44 production-rate-legal**. The 6 residual failures are
all at or below 131 MB raw, where even a 43 ms setup is 30-40% of the whole encode.

## The catch: the size cost is real and very non-uniform

| cell | frozen bytes | small-policy bytes | cost |
|---|---:|---:|---:|
| spdlog / linuxbrew | 498,347 | 794,922 | **+59.5%** |
| spdlog / debian-gcc | 502,334 | 799,421 | **+59.1%** |
| spdlog / conan-gcc | 507,502 | 803,799 | **+58.4%** |
| leveldb / fedora-clang-libcxx | 621,604 | 755,392 | +21.5% |
| json x4, leveldb x3, fmt x4, re2 x4 | | | +3.7% to +16.9% |

So this is **not a free switch**. spdlog pays nearly 60% more wire to gain 1.36x encode
speed. Any deployment must apply the small-corpus policy selectively, and spdlog is the
counter-example proving a naive size-keyed rule would be wrong. The C-side runs both
candidates anyway under policy B, so the right framing is a third candidate whose wire is
kept only when it is both rate-legal and not badly larger -- not a global policy switch.

## Answer to the question as posed

> is the small cells' sub-1GB/s a per-cell/startup overhead that streaming amortizes, or a
> real small-corpus throughput floor?

**Startup overhead.** It is per-invocation, it is 0.2241 s under the frozen policy, and
81% of it is removable by sizing the index and history ring to the corpus. A long-lived
encoder that pays the setup once across many builds amortizes it entirely; a single small
build cannot, which is why 6 corpora at or below 131 MB still miss even after the cut.
GRZ2 is therefore production-rate-legal **universally on large cells** (15/15 at or above
1 GB) and **on mid-size cells with the small-corpus policy**, with a residual tail of very
small corpora that is a setup-cost problem, not a codec-throughput problem.

## Handoff: the blocking rate item is P29, and it is not mine

The interim rate pass established that P29+BSC clears the C gate on **0 of 44** cells, and
on **0 of the 10 cells where it wins the complete size** -- the only cells a selector needs
it for. One-pass alone does not fix it (0.370 -> 0.693 GB/s aggregate, still 0/44). Every
P29 C number is labelled research-interner.

**The decisive measurement is integrating the fast M5 interner into P29's encode path and
re-measuring on those 10 cells.** That is codec engineering on local-oracle's P29 and is
flagged here as the key rate lever, not attempted. Until it lands, the size advantage the
selector could realise -- oracle 0.9705x z19 versus GRZ2-always 1.1071x -- is not
rate-reachable, and a rate-legal selector is forced to GRZ2 everywhere.
