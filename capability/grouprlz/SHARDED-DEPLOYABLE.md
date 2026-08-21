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
   F→C streams byte-identically up to each build-close offset — 6 checks per cell, **10 of
   10 cells** (6 projects × up to 4 docker profiles), and per route in the sharded run.
7. **Truncated-stream replay.** The pair is cut at each build close and replayed against a
   run restricted to those TUs: the byte range must hold every frame that run consumes, in
   order, byte-identically, with nothing left over, and that run must reconstruct all its
   TUs byte-exact. 3 cuts × 10 cells = 30, all pass.
8. **Prefix immutability against a genuinely SHORTER corpus.** Gate 6 compares build
   prefixes of a 4× manifest — but those repeat the same files, so the region and
   distinct-line counts are *identical* in both encodes and the test is blind to any
   dependence on a whole-corpus quantity. This one truncates the source instead: half the
   project has strictly fewer Regions than all of it, and the gate refuses to run if the
   two halves happen to report the same count. **All ten cells: byte-identical prefix in
   both directions**, with the region count differing by 6% to 101% (leveldb/fedora-clang:
   11,945 vs 24,048). That is a real falsification test of what `--stable-root-tags` claims
   — every Root token a function of state available at that TU (region r → 2r, block k →
   2k+1, no NREG on the wire) — and it survives it. I only thought to run it while scoping
   the receiver; the earlier prefix result was correct but could not have caught this class.

`selector_p29sinkproof.sh` is the gate and exits nonzero on any failure; all ten cells
report `GATE PASS`. One over-fitted assertion was found and removed along the way: an
earlier version required the cold build's literal frame to be *displaced* into a later
build, which is only true when the build is smaller than the 112-TU group — it wrongly
failed spdlog (n=168). The hard assertion is now the codec-reported `dispatch_lag_tus`,
which is 111 for the batch binding at any project size.

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

The `spdlog` cell (n=168) shows the same violation in its partial form and is worth
reading: there the cold build is *larger* than the group, so it does contain its own first
group — but group 0 covers TUs 0–111 while build 1 runs to TU 167, so **56 of the cold
build's 168 TUs still have no literals on the wire at its own close**. An earlier version
of this gate asserted the *byte* displacement and wrongly failed spdlog; the hard assertion
is now the codec-reported `dispatch_lag_tus`, which is 111 regardless of project size, and
the displacement is reported rather than asserted.

Two further findings, now across **ten cells (6 projects × up to 4 docker profiles)**, all
GATE PASS:

* Batching 112 TUs buys P29 only **+4.7% … +10.1%** and costs 111 TUs of lag. P29 is far
  more robust to per-TU closure than GRZ2 (+15…+57%).
* **`lg1` beats the pure streaming variant by 11.5–18.0%** on every one of the ten. A
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

* **sticky** — 100% overlap every rebuild, so warm cost is essentially independent of W.
  It drifts slightly *down* for GRZ2 (fmt 8,619 → 7,451 from W=1 to W=32; re2 14,076 →
  10,872) and slightly *up* for P29 (fmt 1,741 → 2,152; cereal 1,824 → 3,337), but every
  one of those is within 1.9× across a 32× change in width — against the 100–1000× swings
  the other two policies show. The direction of the drift is a detail; the flatness is the
  point.
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

Two more things the tables say:

* **Sharding costs the two codecs almost the same.** On the cold build at W=32, GRZ2 pays
  13.93× (fmt) and 11.90× (re2) while P29 pays 12.70× and 13.41×. The penalty is a property
  of splitting the redundancy, not of the codec — so it will not be engineered away by
  choosing a different coder. `cereal` is the outlier at **27.68×** (P29, W=32): a
  header-only library whose TUs are near-copies of one another has the most to lose from
  being cut into pieces.
* **Memory: the direction that matters is the client's.** Sharding moves memory *onto* the
  one machine that can least afford it. For P29 on re2, peak RSS summed over live routes
  grows 445 MB → 2,861 MB (W=1 → W=32, **6.4×**) while the *largest single* route encoder
  shrinks 445 MB → 113 MB (3.9× smaller). Each worker gets cheaper; the client pays for all
  of them at once. GRZ2 on the same cell grows more gently (351 MB → 1,246 MB, 3.5×) only
  because `--hist 1024` already dominates its footprint at W=1 — and for the same reason its
  per-F *decoder* stays at ~2.06 GB regardless of W, allocating the whole 1 GB history ring
  even for a route carrying a handful of TUs. That is per worker machine, so it is
  survivable, but it is a sizing constraint nobody has priced: a route's ring should be
  bounded by what that route can actually reference.

## Point-by-point against the nine requirements

Checked per codec rather than as a single verdict, because they are not equally covered.

| # | requirement | GRZ2 | P29 |
|---|---|---|---|
| 1 | one persistent encoder/decoder state per real C→F route | **yes** — one process per route, that route's TUs in dispatch order | **yes**, same construction |
| 2 | every scheduled TU closes a complete frame, sent immediately | **yes** — G1: frame count == scheduled TU count, every frame covers exactly one TU | **yes** — `--literal-group-tus 1`, `dispatch_lag_tus=0` asserted by the gate |
| 3 | history may persist across TUs on the same route; first honest binding `--gtu 1` | **yes** | **yes** (`lg1`) |
| 4 | declared deterministic scheduler across realistic worker counts, each F's order preserved | **yes** — sticky / rr / shuf × W ∈ {1,4,8,16,32}, seed 12345 | **yes**, the *same* simulator (imported, not re-implemented) |
| 5 | sum actual C→F bytes over every route incl. setup, per-TU headers, closes; reverse separate | **yes** — whole file per route, END frame included; reverse is 0 by construction | **yes** — C→F sink summed; F→C a separate file, never added in |
| 6 | after each TU frame, that F reconstructs the TU from its own stream, before any future TU exists | **yes** — G4 truncates at frame *j*'s physical offset and decodes TUs 0..j | **partial** — the pair is cut at each build close and replayed against a run restricted to those TUs: the byte range holds every frame that run consumes, in order, byte-identically, with nothing left over, and that run reconstructs all its TUs byte-exact (30 truncation replays, 10 cells). Still in-process, so the reconstruction is not yet *proven* independent of C-side state |
| 7 | prefix immutability proven independently on EACH per-F stream | **yes** — every route × every build boundary | **yes** — every route × every build boundary × **both directions**, plus 24/24 on the single stream across ten cells |
| 8 | cold + repeated builds under sticky, round robin and shuffled; stickiness reported, not assumed | **yes** | **yes** |
| 9 | aggregate C memory + per-F memory, esp. retained history × active routes | **yes** — summed encoder RSS per W, max per-F decoder RSS | **yes** — summed and max encoder RSS per W |

So: **GRZ2 satisfies all nine; P29 satisfies eight, with point 6 partial.** The one gap is
the same one the independent receiver would close, and it is not claimed as closed.

A note on how point 7 is tested for P29, because getting it wrong is easy: both sides of
the comparison must run `--open-final-entropy`. A prefix is a stream that has *not* been
terminated, so comparing a build-1 encode against a *terminated* four-build encode measures
the end-of-stream tails and nothing else. The first version of this check did exactly that
and reported a 19-byte mismatch that was entirely tails. The reported byte totals still come
from the properly terminated encode; only the immutability reference is the open one.

## Honest scope

* Four small projects, one docker profile (`debian-gcc`). Breadth after the gates close.
* **P29 sharding runs one codec process per route, which also gives each route its own
  dictionary**, so Region/Block ordinals are dense over that route's subset. Real icecream
  has one client keyspace whose ordinals are sparser. This **understates the id cost of
  sharding for P29**; the definition and literal costs it measures are unaffected.
* The P29 replay gate proves the streams carry every frame the run consumes, in order,
  byte-identically. It is still **in-process**: a separate receiver binary that reconstructs
  `.ii` from the two files alone is not built. That is the remaining bar. Scoping it gave a
  useful answer, though: the F side is ~273 lines (`codec50-sink.cpp`, the block after
  `--- DECODER (F)`) plus state consistently named `F*`/`mixedF*`, and almost every C-side
  name it touches is a harness assertion (`recovered[i] != mixedRaw[i]`, `dict.region_data`
  comparisons) rather than a data dependency. In `--direct-ordinals` mode F already derives
  its own `missReg`/`missBlk` from its own store. So the extraction is real surgery but it
  is not a rewrite, and — per gate 8 — it does **not** need NREG or any other whole-corpus
  quantity shipped to it. Enumerating what the F block actually captures from the enclosing
  scope leaves four items beyond the mechanical extraction:
  1. F currently reads the *raw* `fill_paths` (and `np`) rather than decompressing the
     `WT_PATHDEF` frame. That is a harness shortcut and the one place where F genuinely
     consumes a C-side buffer instead of the wire.
  2. The blob-fallback *reply* is generated by C inside the F region (`compressedBlobs`,
     `mixedArrayEntries`) and has to move back to the C side.
  3. `endOfEntropyStream` is an encoder-side flag; the receiver needs it from the stream —
     a spare bit in the per-TU close frame is the obvious home.
  4. Everything else (`recovered[i] != mixedRaw[i]`, `dict.region_data`/`dict.ref`
     comparisons, `blobRaw`, `newLineIds`) is a harness assertion and simply drops.
* GRZ2 has no F→C codec channel, so its reverse total is 0 by construction — a structural
  difference from P29, not a free pass.
* cereal's single-stream `gtu=1` build-4 spike (137,908) is the previously reported
  no-copy-reanchoring artifact; it is granularity-independent and unrelated to sharding.

## Reproducing

```sh
# the sink-instrumented P29 (verified byte-identical to codec50-refZ with the sinks off)
g++ -O3 -march=native -std=c++23 -DWITH_BSC_GROUPS -I. -I$HOME/libbsc/libbsc \
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


### A. What per-TU closure costs, before any sharding
One stream, corpus order. `batch112` is the old number and is a BOUND: a group
spans up to 112 TUs, so its frame is only complete once TUs that have not been
dispatched yet have arrived at one receiver, in order.

| cell | TUs/build | GRZ2 batch112 (bound) | GRZ2 gtu=1 | per-TU cost | P29 lg112 (bound) | P29 lg1 | per-TU cost |
|---|--:|--:|--:|--:|--:|--:|--:|
| cereal | 80 | 376,965 | 589,956 | +56.5% | (see B) | 490,841 | (see B) |
| fmt | 51 | 667,353 | 768,561 | +15.2% | (see B) | 979,416 | (see B) |
| leveldb | 94 | 615,230 | 803,494 | +30.6% | (see B) | 944,000 | (see B) |
| re2 | 72 | 364,413 | 472,407 | +29.6% | (see B) | 476,137 | (see B) |

### B. What SHARDING costs on top — cold build only (sticky routing)
Sum of every route's stream. GRZ2 = total wire; P29 = C→F primary only.

| cell | codec | W=1 | W=4 | W=8 | W=16 | W=32 |
|---|---|--:|--:|--:|--:|--:|
| cereal | GRZ2 | 427,858 (1.00×) | 1,525,207 (3.56×) | 2,904,527 (6.79×) | 5,662,682 (13.23×) | 11,178,850 (26.13×) |
| cereal | P29 C→F | 485,621 (1.00×) | 1,794,359 (3.69×) | 3,459,651 (7.12×) | 6,787,791 (13.98×) | 13,443,350 (27.68×) |
| fmt | GRZ2 | 743,133 (1.00×) | 1,943,963 (2.62×) | 3,463,979 (4.66×) | 6,026,731 (8.11×) | 10,354,849 (13.93×) |
| fmt | P29 C→F | 975,407 (1.00×) | 2,415,226 (2.48×) | 4,229,807 (4.34×) | 7,292,055 (7.48×) | 12,389,252 (12.70×) |
| leveldb | GRZ2 | 761,821 (1.00×) | 1,857,894 (2.44×) | 3,164,234 (4.15×) | 5,609,918 (7.36×) | 9,815,607 (12.88×) |
| leveldb | P29 C→F | 935,713 (1.00×) | 2,351,339 (2.51×) | 3,956,791 (4.23×) | 6,959,322 (7.44×) | 12,020,668 (12.85×) |
| re2 | GRZ2 | 430,143 (1.00×) | 1,012,901 (2.35×) | 1,723,548 (4.01×) | 3,020,587 (7.02×) | 5,119,506 (11.90×) |
| re2 | P29 C→F | 470,193 (1.00×) | 1,198,693 (2.55×) | 2,084,234 (4.43×) | 3,699,272 (7.87×) | 6,305,396 (13.41×) |

### C. Stickiness is a REPORTED input — warm rebuild cost (build 2)
`rr_deg` marks a configuration where round robin degenerates to sticky because
n % W == 0; those rows are identical by construction, not by luck.

| cell | codec | policy | W=1 | W=4 | W=8 | W=16 | W=32 |
|---|---|---|--:|--:|--:|--:|--:|
| cereal | GRZ2 | sticky | 12,095 | 12,083 | 12,071 | 12,047 | 11,999 |
| cereal | GRZ2 | rr | 12,095* | 12,083* | 12,071* | 12,047* | 146,279 |
| cereal | GRZ2 | shuf | 12,109 | 72,520 | 106,521 | 126,293 | 135,014 |
| cereal | P29 C→F | sticky | 1,824 | 3,480 | 3,430 | 3,375 | 3,337 |
| cereal | P29 C→F | rr | 1,824* | 3,480* | 3,430* | 3,375* | 132,073 |
| cereal | P29 C→F | shuf | 1,890 | 59,028 | 91,944 | 112,432 | 121,653 |
| fmt | GRZ2 | sticky | 8,619 | 8,278 | 7,762 | 7,507 | 7,451 |
| fmt | GRZ2 | rr | 8,619* | 375,254 | 451,388 | 844,104 | 1,862,649 |
| fmt | GRZ2 | shuf | 8,245 | 232,254 | 440,515 | 927,324 | 1,746,415 |
| fmt | P29 C→F | sticky | 1,741 | 2,141 | 2,253 | 2,227 | 2,152 |
| fmt | P29 C→F | rr | 1,741* | 545,537 | 640,612 | 1,114,258 | 2,401,118 |
| fmt | P29 C→F | shuf | 1,736 | 320,155 | 597,642 | 1,203,082 | 2,209,105 |
| leveldb | GRZ2 | sticky | 13,879 | 13,851 | 13,837 | 13,813 | 13,561 |
| leveldb | GRZ2 | rr | 13,879* | 430,260 | 521,869 | 702,784 | 1,237,228 |
| leveldb | GRZ2 | shuf | 14,251 | 230,217 | 389,313 | 769,798 | 1,706,246 |
| leveldb | P29 C→F | sticky | 4,127 | 4,137 | 4,118 | 4,122 | 4,063 |
| leveldb | P29 C→F | rr | 4,127* | 522,005 | 679,111 | 905,941 | 1,577,615 |
| leveldb | P29 C→F | shuf | 4,131 | 283,935 | 500,925 | 985,420 | 2,176,804 |
| re2 | GRZ2 | sticky | 14,076 | 12,247 | 11,315 | 10,930 | 10,872 |
| re2 | GRZ2 | rr | 14,076* | 12,247* | 11,315* | 433,197 | 787,400 |
| re2 | GRZ2 | shuf | 10,588 | 187,944 | 296,769 | 521,604 | 938,380 |
| re2 | P29 C→F | sticky | 2,768 | 2,739 | 2,719 | 2,681 | 2,853 |
| re2 | P29 C→F | rr | 2,768* | 2,739* | 2,719* | 476,487 | 916,006 |
| re2 | P29 C→F | shuf | 2,770 | 185,820 | 312,760 | 581,823 | 1,094,390 |

`*` = rr degenerate (n % W == 0), identical to sticky by construction.

### D. Memory — the cost of holding one encoder state per route
Peak RSS summed over routes (what C holds if every route is live), and the
largest single per-F decoder RSS.

| cell | W=1 | W=4 | W=8 | W=16 | W=32 | max per-F decode RSS |
|---|--:|--:|--:|--:|--:|--:|
| cereal | 1,061 MB | 1,173 MB | 1,303 MB | 1,560 MB | 2,077 MB | 2,071 MB |
| fmt | 522 MB | 616 MB | 746 MB | 997 MB | 1,495 MB | 2,074 MB |
| leveldb | 696 MB | 790 MB | 913 MB | 1,157 MB | 1,644 MB | 2,066 MB |
| re2 | 351 MB | 440 MB | 557 MB | 792 MB | 1,246 MB | 2,064 MB |

### E. Reverse direction, reported separately and never added in
GRZ has no F→C codec channel at all. P29 does, and it GROWS with sharding:
every F independently asks for what it lacks.

| cell | sticky W=1 | sticky W=32 | rr W=1 | rr W=32 |
|---|--:|--:|--:|--:|
| cereal | 11,443 | 245,171 | 11,443 | 249,655 |
| fmt | 76,432 | 252,475 | 76,432 | 476,073 |
| leveldb | 47,165 | 229,401 | 47,165 | 363,209 |
| re2 | 12,540 | 131,326 | 12,540 | 169,851 |

### F. Gate ledger (GRZ) — configurations, not spot checks
- configurations: 60, routes gated: 732
- G1 one frame per scheduled TU: 732/732 routes
- G2 whole route stream decodes byte-exact: 732/732 routes
- G3 per-route prefix immutability: 2196/2196 (route × build boundary)
- G4 immediate decodability: 732/732 routes, 5102 truncation points

### G. P29 single-stream: deployable vs bound, from the gate transcripts
`lag` is the codec's own `dispatch_lag_tus`: how many TUs after a TU is dispatched
its literals become sendable. Only 0 is a transport.

| cell | stream C→F | lg1 C→F (deployable) | lg112 C→F (bound) | lg1 vs bound | lg1 vs stream | F→C | lg112 lag | lg112 build increments |
|---|--:|--:|--:|--:|--:|--:|--:|---|
| cereal.conan-gcc | 607,226 | 500,589 | 472,633 | +5.9% | −17.6% | 11,850 | 111 | [164067, 305144, 1699, 1723] |
| cereal.debian-gcc | 595,597 | 490,893 | 462,950 | +6.0% | −17.6% | 11,495 | 111 | [161728, 297800, 1699, 1723] |
| fmt.debian-gcc | 1,142,030 | 979,468 | 935,724 | +4.7% | −14.2% | 76,484 | 111 | [468139, 1754, 464672, 1159] |
| fmt.linuxbrew | 983,455 | 806,025 | 755,803 | +6.6% | −18.0% | 11,039 | 111 | [269154, 1752, 483738, 1159] |
| leveldb.debian-gcc | 1,070,007 | 944,052 | 877,346 | +7.6% | −11.8% | 47,217 | 111 | [429539, 443621, 2081, 2105] |
| leveldb.fedora-clang-libcxx | 1,180,905 | 1,023,994 | 929,813 | +10.1% | −13.3% | 28,497 | 111 | [441551, 484076, 2081, 2105] |
| nlohmann-json.debian-gcc | 1,405,615 | 1,224,656 | 1,114,992 | +9.8% | −12.9% | 31,291 | 111 | [543745, 566841, 2191, 2215] |
| re2.debian-gcc | 561,388 | 476,189 | 449,724 | +5.9% | −15.2% | 12,592 | 111 | [164292, 282230, 1597, 1605] |
| re2.fedora-clang-libcxx | 621,942 | 517,327 | 490,206 | +5.5% | −16.8% | 20,448 | 111 | [174429, 312575, 1597, 1605] |
| spdlog.debian-gcc | 864,560 | 765,463 | 699,423 | +9.4% | −11.5% | 26,488 | 111 | [663721, 28260, 3709, 3733] |

### H. Gate ledger (P29 sharded)
- configurations: 60, routes gated: 732
- every route: byte-exact reconstruction AND `dispatch_lag_tus=0`, or the configuration aborts
- per-route prefix immutability (route × build boundary × direction): 4392/4392
- routes additionally replayed frame-by-frame off their own files: 60 (the first route of each configuration)
