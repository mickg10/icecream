# M5 feasibility, and the retained rate-pass root

## Retained root for the 309f6b3 rate pass

```
ttuser@tt-quietbox2:/home/ttuser/selbind/rate/          245 MB, 44 cells
```

Per cell `<project>-<profile>/`:

| path | contents |
|---|---|
| `row.tsv` | the emitted row (bytes, rates, gate flags, bases) |
| `probe.reps`, `p29.full.reps`, `grz.full.reps`, `grz.dec.reps` | every repetition, none discarded |
| `p29/plan.{1,2}.{out,err,res}`, `p29/g.{1,2}.{out,err,res}` | both P29 passes, stdout + stderr + `/usr/bin/time` |
| `grz/enc.{1,2}.*`, `grz/dec.{1,2}.*`, `grz/cat.*.res`, `grz/tu.*.res` | GRZ encode, decode, and its charged input conversion |
| `probe{1,2,3}/` | the three TU112 probe races, including the gated wire comparison |
| `manifest.txt`, `manifest.probe.txt` | exact TU order used |

Aggregates: `~/selbind/selector-policyB-rate-INTERIM.{tsv,notes}` and
`~/selbind/selector-policyB-rate-decomp.txt`. Runner `selector_pb_rate.sh`.

## Interning is 68.4% of P29's complete two-pass encode

Measured from the retained logs, not projected -- `loaded+interned` against the codec's own
`total` for both passes, summed over all 44 cells:

```
intern 127.3 s of 186.2 s = 68.4% of the two-pass encode wall
```

Per cell it reaches 79% (eigen/linuxbrew 13.2 s of 16.7 s); the eigen and catch2 rows are
all above 69%.

Upper bounds, taking the stage to **zero** (no interner is free, so these bound what M5
can buy):

| configuration | GB/s | gate |
|---|---:|---|
| two-pass, research interner (**measured**) | 0.370 | 0/44 |
| two-pass, interning free | **1.170** | would clear |
| one-pass, research interner (**measured**) | 0.693 | 0/44 |
| one-pass and interning free | **2.191** | would clear |

**This confirms the ruling that M5 is the crux, with a measured share rather than a
projection -- and sharpens it: interning dominates so heavily that M5 alone could suffice
without the one-pass rewrite.** Two-pass with a free interner already clears at 1.170 GB/s.
The requirement is that M5 takes interning from 68.4% of the wall to roughly under 20%.

## Can I do the integration? Partly -- and not the part that matters most yet

Honest read after reading `codec50.cpp`'s `Interner` (lines 95-147).

**What it actually is.** `Interner` is not a line interner with a fast path bolted on; it is
a **Region** interner. It maintains a learned successor graph (`RegionRecord::next1/next2`,
built under the `train` flag) that provides the hot path, a region hash index, a region
byte store, and region to line-id sequences. The encoder consumes `region_count()`,
`region_ids_ptr()`, `region_data()`, `region_raw_len()`, `region_key()`, `ref()`,
`line_data()`, `distinct()`. `region_key()` is documented in-source as the stand-in for the
persistent C-cache key.

**What M5 is.** `~/linebench/production-fused.cpp` is a **line** interner -- bytes to stable
line ids at the rates that lane demonstrated. It does not produce regions, the successor
graph, or the region byte store.

So this is not a drop-in swap. Splitting it:

- **`intern_line(const char*, uint32_t) -> uint32_t` is a narrow, well-defined seam**, called
  once per line in the region cold path. Substituting M5's line hashing behind it, while
  preserving the `id_refs_` / `line_bytes_` mapping the encoder reads, is bounded work and
  **is within my scope.**
- **The region layer is not.** The successor graph and `region_key` carry codec semantics
  that feed the encode plan and the persistent-cache contract. Changing them risks
  byte-exactness and the prefix-identity gate, which is local-oracle's to own. **I should
  not touch it, and whether M5 belongs at the region layer is local-oracle's call.**

**The blocker before either is measurement, not code.** The 68.4% above is
`load_corpus` + `Interner::process` together, and `process` covers region scanning
(`next_region`), region hashing (`sampled_hash`), the region index probe, **and** the
per-line `intern_line` loop. I do not know that split. If most of the 68.4% is region-level
scanning and hashing, swapping the line interner buys little, and the effort belongs at the
region layer -- which is exactly the part I should not be doing unilaterally.

**Recommendation.** Do the observation-only stage instrumentation first -- already
green-lit, byte-for-byte verified against the identity wire, instrumented binary SHA
pinned. It splits the 68.4% into region-scan / region-hash / line-intern and says whether
the `intern_line` seam is worth taking. That I can do now. On the answer:

- if line interning dominates, I take the `intern_line` seam and measure;
- if region work dominates, it is local-oracle's codec change and I hand it the numbers.

Either way the measurement is cheap and removes the guesswork from a change nobody should
be making blind.
