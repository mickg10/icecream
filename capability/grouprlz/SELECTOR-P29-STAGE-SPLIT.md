# Stage split: M5 is not the lever. The producer is.

Observation-only instrumentation of `Interner::process` in local-oracle commit `56c1744`,
splitting the previously-measured 68.4% `load+intern` share into its parts. Run on one
profile (debian-gcc) of all eleven verified projects, complete grouped pass.

## Observation-only, proven

The instrumented build adds only counters and `Clock::now()` calls -- no control flow, no
data, no output. Verified against the gated identity wire on catch2/debian-gcc:

| artifact | instrumented | gate | |
|---|---|---|---|
| `full/curve.tsv` | `8453551d…` | `8453551d…` | IDENTICAL |
| `full/components.tsv` | `31f280cb…` | `31f280cb…` | IDENTICAL |
| `full/literal.wire` | `9e0e9f9f…` | `9e0e9f9f…` | IDENTICAL |
| `prefix/literal.wire` | `5bab5e10…` | `5bab5e10…` | IDENTICAL |
| `prefix/curve.tsv` | `384988e1…` | `384988e1…` | IDENTICAL |

`TOTAL=980640` both, **prefix identity PASS**. Pins: source
`0229fe22fad34011e46462ec590c98d31e0539a227a1524483099c1337a81b5f`, binary
`f3dd0096239666f6d24a915f57c66d5902311013d290d0d7975ef80cfc55ba17`.

## Where the time actually goes

```
total encode wall                          24.7 s
load+intern phase                          16.3 s   66.0% of wall
  -> load_corpus (file read + copy)        12.8 s   51.8% of wall,  78.5% of the phase
  -> Interner::process                      3.5 s   14.2% of wall,  21.5% of the phase
     of which the per-line intern loop      0.8 s    3.3% of wall   <- the intern_line seam
```

Within `Interner::process` the split shifts with corpus size: on the large corpora the
region successor path and scan dominate (eigen predict 56.6%, scan 32.7%, lines **8.4%**;
opencv 41.4 / 26.6 / 25.7), while only the small corpora are line-heavy (re2 68.8%,
fmt 59.6%). Weighted by time, lines are a small minority.

## The answer to the question that was asked

> is the 68.4% region work, or per-line-loop work?

**Neither. It is 78.5% file loading.** Upper bounds, taking each stage to zero, against
the measured two-pass 0.370 GB/s:

| if this were free | GB/s | |
|---|---:|---|
| M5 behind `intern_line` (the line loop) | 0.383 | +3.5%, worthless |
| the whole of `Interner::process` | 0.431 | still nowhere |
| **`load_corpus` -- a shared in-memory producer** | **0.768** | the real lever |
| producer and the whole interner | 1.088 | clears |
| **one-pass (measured 0.693) plus the producer** | **1.438** | **clears comfortably** |

**So the M5 integration should not be done.** Replacing the line interner buys ~3% of the
encode wall. The `intern_line` seam I offered to take is not worth taking, and neither is
a region-layer change -- the whole interner is only 14.2%.

**The lever is the shared in-memory producer**, which was already bigoracle's condition 3
and which I flagged as unimplemented. In production the C side holds the preprocessed TU
stream in memory; `load_corpus` re-reading and re-copying it from disk is a harness
artifact, and it is more than half of P29's encode time. **One-pass plus that producer
reaches 1.438 GB/s and clears the gate without touching the interner at all.**

## What this changes

- The standing claim "M5 is the crux docker blocker" is **wrong**, and it was my own
  measured 68.4% that made it look right -- that number conflated file loading with
  interning. This split separates them.
- The two changes that matter are both structural, not algorithmic: **fuse the two P29
  passes**, and **feed the encoder from memory instead of re-reading the corpus**. Neither
  is an interner problem.
- Both are P29-side work and remain local-oracle's or the owner's call; my scoping offer
  to take the `intern_line` seam is **withdrawn as not worth doing**.
