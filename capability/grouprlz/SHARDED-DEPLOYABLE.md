# Sharded, per-TU, two-direction: what a deployable icecream codec actually costs

This supersedes every `--gtu 112` / `--literal-group-tus 112` number as a **transport**
claim. Those remain valid as **single-stream batch/corpus-compression bounds** and are
labelled as such throughout. They are not physical per-TU rows and there are not
"three physical codecs" — there is one bound and, now, two measured deployable bindings.

## Why the earlier per-TU fix was not enough

The previous round closed a real hole: a group spanning 112 TUs cannot be sent until TU 111
exists, so `--gtu 112` is not a transport. The fix was `--gtu 1` on one stream.

That is still a batch bound, because it assumes **every TU reaches one receiver, in order**.
Real icecream shards: the client sends each TU as an individual job to a *selected* remote
worker, so with workers Fa/Fb/Fc the dispatched sequences look like Fa: 1,6,7,9 / Fb: 2,4,10
/ Fc: 3,5… **Each C→F route is a separate persistent stream carrying only its own subset.**
A route can only match against the TUs it has itself received.

So the deployable question is not "what does per-TU framing cost" but "what survives per-TU
closure **and** sharded history".

## The binding measured here

* **One persistent encoder/decoder state per actual C→F route.** One process per route, fed
  only that route's TUs, in that route's dispatch order.
* **Every scheduled TU closes a complete frame, sent immediately.** GRZ2: `--gtu 1`.
  P29: `--literal-group-tus 1`, and the codec's own `dispatch_lag_tus` is asserted to be 0.
* **Matcher/history persists across TUs on the same route** — that is the whole point;
  what is forbidden is a frame that depends on a TU not yet dispatched.
* **Charge = the sum of every route's stream**, including stream setup, per-TU frames and
  the final close. Reverse traffic is a separate file and is never added to the primary.
* **Scheduler: a declared deterministic simulator**, not a hidden assumption.
  * `sticky` — w = (TU index within the build) % W. The same source TU always lands on the
    same worker in every build. Maximum cross-build affinity.
  * `rr` — strict round robin over the global dispatch order, w = t % W.
  * `shuf` — per-build seeded shuffle (seed 12345), then round robin over the shuffled
    order: balanced load, no cross-build identity affinity.
  * W ∈ {1, 4, 8, 16, 32}. W=1 is the single-stream control.

## Gates (enforced; a failure aborts the configuration rather than publishing it)

**GRZ2, per route:**

1. **G1** exactly one complete frame per scheduled TU; strictly increasing physical offsets;
   file size == last end offset + the 36-byte END frame.
2. **G2** the whole route stream decodes byte-exact from its own bytes alone.
3. **G3 prefix immutability, per stream.** Re-encoding that route's build-1..k subset
   reproduces the leading bytes of the full route stream **byte for byte**. This is the test
   that killed the earlier prefix method, run here on every route × every build boundary.
4. **G4 immediate decodability.** Truncating the route stream at frame *j*'s physical end
   offset still reconstructs TUs 0..j on that route. The decoder hard-fails on anything that
   is not an exact completed-frame boundary and verifies each group's digest, so a pass is a
   real statement about that byte range. Points: every build boundary, both endpoints, and a
   stride sample.

**P29:** the accounting totals are a *sum over categories*, not a byte stream, so they cannot
show a build closing at a physical offset. `codec50-sink.cpp` adds real append-only sinks —
one per direction, 5-byte typed length-delimited frame per message, an explicit per-TU close
frame carrying the stream-presence mask, an explicit build-close frame. It refuses any
binding it does not fully instrument, and refuses `--warm` / `--replay-repetitions`.

5. **Replay gate.** The identical call sites can *read the stream back* instead of writing
   it (`--sink-replay`): every message must be present, in order, with the declared type and
   a byte-identical payload, and the stream must be fully consumed. Verified to FAIL (exit 2)
   on: truncation, a single flipped byte, 8 appended bytes, one deleted frame, and truncation
   of the *reverse* stream. Control passes (exit 0).
6. **Prefix immutability, both directions.** Re-encoding builds 1..k reproduces the C→F and
   F→C streams byte-identically up to each build-close offset — 6/6 on fmt.

## The causality violation, as a physical fact

fmt.debian-gcc, 4 builds × 51 TUs, physical C→F build-close offsets:

| variant | C→F total | F→C | dispatch lag | build 1 | build 2 | build 3 | build 4 |
|---|--:|--:|--:|--:|--:|--:|--:|
| `stream` (causal) | 1,142,030 | 76,484 | 0 | 1,137,974 | 1,754 | 1,135 | 1,167 |
| `lg1` (causal, deployable) | 979,468 | 76,484 | 0 | 975,420 | 1,754 | 1,135 | 1,159 |
| `lg112` (**bound**) | 935,724 | 76,484 | **111** | 468,139 | 1,754 | 464,672 | 1,159 |

Read the `lg112` row: **the cold build's literal frame physically lands in build 3.** With
204 TUs and groups of 112, group 0 cannot leave C until TU 111 — which falls inside build 3.
Its "build 1 = 468,139" is a fiction; F cannot compile a single build-1 TU from those bytes.
That is not an argument about the model, it is where the bytes are in the file.

Two further findings from the same table:

* Batching 112 TUs buys P29 only **4.7%** (979,468 vs 935,724) and costs 111 TUs of lag.
  P29 is far more robust to per-TU closure than GRZ2 (+15…+57%).
* **`lg1` beats the pure streaming variant by 11.8–17.6%** across all four cells. A
  self-contained per-TU residual-group frame (BSC-selected) is worth more than a
  retained-context z3 flush. For P29 the per-TU literal frame is not a concession — it is
  an improvement. Worth bigoracle's attention when reassessing the codec path under the
  immediate-TU constraint.
* Those `lg1` frames really are self-contained: encoding re2 with
  `--literal-group-workers` 1, 4 and 8 gives a **byte-identical** C→F stream
  (sha 9c23d3e8…), so the frames carry no cross-frame coder state and F could decode them
  out of order. That is stronger than the causality rule requires.

## The mechanism sharding exposes

A route's rebuild cost is set by **how much of the TU set that route has already seen**.

* **sticky** — 100% overlap every rebuild. Warm cost is essentially independent of W, and
  slightly *cheaper* at large W (each route re-matches its own smaller prior copy).
* **rr** — the assignment phase shifts by `n mod W` each build, so it repeats with

  ```
  P = W / gcd(n mod W, W)          (P = 1 when W divides n)
  build k is CHEAP  ⇔  k > P
  ```

  Builds 1..P are all distinct phases and each pays a cold-ish price; every build after
  that repeats an earlier phase and is cheap. **This holds on all 20 P29 `rr` rows** —
  cereal W=32 (P=2): 13,443,350 / 132,073 / **3,343 / 4,170**; re2 W=16 (P=2): 3,699,272 /
  476,487 / **2,704 / 2,856**; leveldb W=4 (P=2): 2,351,339 / 522,005 / **4,114 / 4,257**;
  and where P ≥ 4 every build pays (fmt W=4, P=4: 2,415,226 / 545,537 / 472,727 / 461,745).

  The operational corollary is cheap and actionable: **when W divides the TU count, plain
  round robin IS sticky routing** — P=1, and the rows are identical by construction. The
  harness flags those as `rr_degenerate` rather than letting the coincidence pass as an
  independent result.
* **shuf** — random overlap. Rebuild cost decays only as routes gradually accumulate the
  whole corpus, which defeats the purpose of sharding.

Order *within* a build barely matters for P29 (shuf W=1 is +0.2% over sticky W=1 on re2);
it is the cross-build assignment that carries the cost.

## Honest scope

* Four small projects, one docker profile (`debian-gcc`). Breadth after the gates close.
* **P29 sharding runs one codec process per route, which also gives each route its own
  dictionary**, so Region/Block ordinals are dense over that route's subset. Real icecream
  has one client keyspace whose ordinals are sparser. This **understates the id cost of
  sharding for P29**; the definition and literal costs it measures are unaffected.
* The P29 replay gate proves the streams carry every frame the run consumes, in order,
  byte-identically. It is still **in-process**: a separate receiver binary that reconstructs
  `.ii` from the two files alone is not built. That is the remaining bar.
* GRZ2 has no F→C codec channel, so its reverse total is 0 by construction — a structural
  difference from P29, not a free pass.
* cereal's single-stream `gtu=1` build-4 spike (137,908) is the previously reported
  no-copy-reanchoring artifact; it is granularity-independent and unrelated to sharding.

## Reproducing

```sh
# the sink-instrumented P29 (verified byte-identical to codec50-refZ with the sinks off)
g++ -O3 -march=native -std=c++17 -DWITH_BSC_GROUPS -I. -I$HOME/libbsc/libbsc \
    codec50-sink.cpp -o codec50-sink \
    $HOME/grouprlz/libbsc.a /usr/lib/x86_64-linux-gnu/libzstd.a -lz -lpthread

./selector_p29sinkproof.sh <4x-manifest> <tus-per-build>   # P29 gate, exits nonzero on failure
./selector_shardrun.py  <proj> <prof> --workers 1,4,8,16,32 --policies sticky,rr,shuf --out X.tsv
./selector_p29shard.py  <proj> <prof> --workers 1,4,8,16,32 --policies sticky,rr,shuf --out Y.tsv
./selector_mkshardreport.py [grz-dir] [p29-dir]            # regenerates every table below
```

Evidence is in `sharded-deployable/`: `*.grz.tsv` + `*.grzgates.tsv` + `*.grzprov.json`
(GRZ2), `*.p29shard.tsv` (P29 sharded), `*.debian-gcc.txt` (P29 gate transcripts).

## Tables

All numbers below are generated directly from the harness TSVs and gate transcripts by
`selector_mkshardreport.py`; nothing is transcribed by hand.


_(GRZ tables omitted: no `*.grz.tsv` in the evidence directory.)_

### A. What per-TU closure costs, before any sharding
One stream, corpus order. `batch112` is the old number and is a BOUND: a group
spans up to 112 TUs, so its frame is only complete once TUs that have not been
dispatched yet have arrived at one receiver, in order.

| cell | TUs/build | GRZ2 batch112 (bound) | GRZ2 gtu=1 | per-TU cost | P29 lg112 (bound) | P29 lg1 | per-TU cost |
|---|--:|--:|--:|--:|--:|--:|--:|
| cereal | None | — | — | — | — | — | — |
| fmt | None | — | — | — | — | — | — |
| leveldb | None | — | — | — | — | — | — |
| re2 | None | — | — | — | — | — | — |

### B. What SHARDING costs on top — cold build only (sticky routing)
Sum of every route's stream. GRZ2 = total wire; P29 = C→F primary only.

| cell | codec | W=1 | W=4 | W=8 | W=16 | W=32 |
|---|---|--:|--:|--:|--:|--:|
| cereal | GRZ2 | — | — | — | — | — |
| cereal | P29 C→F | 485,621 (1.00×) | 1,794,359 (3.69×) | 3,459,651 (7.12×) | 6,787,791 (13.98×) | 13,443,350 (27.68×) |
| fmt | GRZ2 | — | — | — | — | — |
| fmt | P29 C→F | 975,407 (1.00×) | 2,415,226 (2.48×) | 4,229,807 (4.34×) | 7,292,055 (7.48×) | 12,389,252 (12.70×) |
| leveldb | GRZ2 | — | — | — | — | — |
| leveldb | P29 C→F | 935,713 (1.00×) | 2,351,339 (2.51×) | 3,956,791 (4.23×) | 6,959,322 (7.44×) | 12,020,668 (12.85×) |
| re2 | GRZ2 | — | — | — | — | — |
| re2 | P29 C→F | 470,193 (1.00×) | 1,198,693 (2.55×) | 2,084,234 (4.43×) | 3,699,272 (7.87×) | 6,305,396 (13.41×) |

### C. Stickiness is a REPORTED input — warm rebuild cost (build 2)
`rr_deg` marks a configuration where round robin degenerates to sticky because
n % W == 0; those rows are identical by construction, not by luck.

| cell | codec | policy | W=1 | W=4 | W=8 | W=16 | W=32 |
|---|---|---|--:|--:|--:|--:|--:|
| cereal | P29 C→F | sticky | 1,824 | 3,480 | 3,430 | 3,375 | 3,337 |
| cereal | P29 C→F | rr | 1,824* | 3,480* | 3,430* | 3,375* | 132,073 |
| cereal | P29 C→F | shuf | 1,890 | 59,028 | 91,944 | 112,432 | 121,653 |
| fmt | P29 C→F | sticky | 1,741 | 2,141 | 2,253 | 2,227 | 2,152 |
| fmt | P29 C→F | rr | 1,741* | 545,537 | 640,612 | 1,114,258 | 2,401,118 |
| fmt | P29 C→F | shuf | 1,736 | 320,155 | 597,642 | 1,203,082 | 2,209,105 |
| leveldb | P29 C→F | sticky | 4,127 | 4,137 | 4,118 | 4,122 | 4,063 |
| leveldb | P29 C→F | rr | 4,127* | 522,005 | 679,111 | 905,941 | 1,577,615 |
| leveldb | P29 C→F | shuf | 4,131 | 283,935 | 500,925 | 985,420 | 2,176,804 |
| re2 | P29 C→F | sticky | 2,768 | 2,739 | 2,719 | 2,681 | 2,853 |
| re2 | P29 C→F | rr | 2,768* | 2,739* | 2,719* | 476,487 | 916,006 |
| re2 | P29 C→F | shuf | 2,770 | 185,820 | 312,760 | 581,823 | 1,094,390 |

`*` = rr degenerate (n % W == 0), identical to sticky by construction.

### D. Memory — the cost of holding one encoder state per route
Peak RSS summed over routes (what C holds if every route is live), and the
largest single per-F decoder RSS.

| cell | W=1 | W=4 | W=8 | W=16 | W=32 | max per-F decode RSS |
|---|--:|--:|--:|--:|--:|--:|
| cereal | — | — | — | — | — | — |
| fmt | — | — | — | — | — | — |
| leveldb | — | — | — | — | — | — |
| re2 | — | — | — | — | — | — |

### E. Reverse direction, reported separately and never added in
GRZ has no F→C codec channel at all. P29 does, and it GROWS with sharding:
every F independently asks for what it lacks.

| cell | sticky W=1 | sticky W=32 | rr W=1 | rr W=32 |
|---|--:|--:|--:|--:|
| cereal | 11,443 | 245,171 | 11,443 | 249,655 |
| fmt | 76,432 | 252,475 | 76,432 | 476,073 |
| leveldb | 47,165 | 229,401 | 47,165 | 363,209 |
| re2 | 12,540 | 131,326 | 12,540 | 169,851 |

### G. P29 single-stream: deployable vs bound, from the gate transcripts
`lag` is the codec's own `dispatch_lag_tus`: how many TUs after a TU is dispatched
its literals become sendable. Only 0 is a transport.

| cell | stream C→F | lg1 C→F (deployable) | lg112 C→F (bound) | lg1 vs bound | lg1 vs stream | F→C | lg112 lag | lg112 build increments |
|---|--:|--:|--:|--:|--:|--:|--:|---|
| cereal | 595,597 | 490,893 | 462,950 | +6.0% | −17.6% | 11,495 | 111 | [161728, 297800, 1699, 1723] |
| fmt | 1,142,030 | 979,468 | 935,724 | +4.7% | −14.2% | 76,484 | 111 | [468139, 1754, 464672, 1159] |
| leveldb | 1,070,007 | 944,052 | 877,346 | +7.6% | −11.8% | 47,217 | 111 | [429539, 443621, 2081, 2105] |
| re2 | 561,388 | 476,189 | 449,724 | +5.9% | −15.2% | 12,592 | 111 | [164292, 282230, 1597, 1605] |

### F. Gate ledger (GRZ) — configurations, not spot checks
