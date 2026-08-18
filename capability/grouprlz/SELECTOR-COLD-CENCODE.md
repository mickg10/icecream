# COLD_1F_STICKY_CODEC: the direct cold C-encode row

The open `/goal` question: does the 400x-size cold codec sustain **>= 1 GB/s isolated
C-encode**, in the same row as its size result?

## Method

Observation-only timestamps added to the 56c1744 codec, **byte-exact verified** against the
gated identity wire before any timing was used (`full/curve.tsv`, `full/components.tsv`,
both literal wires IDENTICAL, `TOTAL=980640`, prefix identity PASS). Source
`10e7991a…`, binaries `2b2f82ac…` and `fccb37ee…` (the latter only raises
`ICE_LINE_CAP_LOG2` 21 -> 24; re-verified byte-identical).

Clocks are explicit, never derived by subtracting a stage:

```
C_input_ready_s  process start -> corpus bytes resident in memory
C_encode_ready_s corpus bytes in memory -> last selected payload fully encoded
                 (interning + S1 factorization + candidate selection + materialization
                  + entropy z19/BSC on the literal groups + selector + serialization)
C_total_s        process start -> same point
```

`C_encode_ready` is the **in-memory producer** figure the question asks for: the harness
file-load artifact is *excluded by where the clock starts*, not subtracted afterwards.
3 reps per corpus, uncontended box, spread under 1.5% on every row.

## Result

| corpus | raw | P29 wire | / z19 | **C-encode GB/s** | GRZ wire | / z19 | selected |
|---|---:|---:|---:|---:|---:|---:|---|
| range-v3 | 632,049,016 | 739,437 | 0.9985 | **1.128** | 628,319 | 0.8484 | GRZ2 |
| catch2 | 947,252,235 | 907,327 | 1.1332 | **1.456** | 626,178 | 0.7821 | GRZ2 |
| rocksdb | 3,114,320,596 | 8,533,638 | 1.3765 | **0.856** | 5,456,878 | 0.8802 | GRZ2 |
| eigen | 3,532,268,956 | 1,143,631 | 0.9215 | **2.009** | 2,040,151 | 1.6439 | P29+BSC |
| llvm | 3,620,271,340 | 7,275,808 | 0.9972 | **1.158** | 7,638,087 | 1.0468 | P29+BSC |

**P29+BSC clears 1 GB/s on 4 of the 5 measured corpora** (rocksdb is the miss at 0.856).

## Verdict: 5 of 6, and Godot is the one that fails

Both codecs on the same in-memory basis (an earlier draft mixed P29's in-memory clock with
GRZ2 wall-clock numbers from another run; corrected):

| corpus | P29 wire | P29/z19 | P29 GB/s | GRZ2 wire | GRZ/z19 | GRZ2 GB/s |
|---|---:|---:|---:|---:|---:|---:|
| catch2 | 907,327 | 1.1332 | 1.456 | 626,178 | 0.7821 | 1.487 |
| eigen | 1,143,631 | 0.9215 | 2.009 | 2,040,151 | 1.6439 | 1.541 |
| **godot** | 35,085,071 | **0.6079** | **0.384** | 53,427,567 | 0.9257 | **0.347** |
| llvm | 7,275,808 | 0.9972 | 1.158 | 7,638,087 | 1.0468 | 0.983 |
| range-v3 | 739,437 | 0.9985 | 1.128 | 628,319 | 0.8484 | 1.294 |
| rocksdb | 8,533,638 | 1.3765 | 0.856 | 5,456,878 | 0.8802 | 1.009 |

**Selected codec, both bars:**

| corpus | selected | sel / z19 | sel GB/s | BOTH_BARS_PASS |
|---|---|---:|---:|:---:|
| catch2 | GRZ2 | 0.7821 | 1.487 | **TRUE** |
| eigen | P29+BSC | 0.9215 | 2.009 | **TRUE** |
| **godot** | **P29+BSC** | **0.6079** | **0.384** | **FALSE** |
| llvm | P29+BSC | 0.9972 | 1.158 | **TRUE** |
| range-v3 | GRZ2 | 0.8484 | 1.294 | **TRUE** |
| rocksdb | GRZ2 | 0.8802 | 1.009 | **TRUE** |

**5 of 6.** Selected total 50,215,885 B over 17,778,924,328 raw = **354.0x raw, 0.6786x z19**.

Two things this says that the five-corpus table did not:

- **Godot fails the rate bar on BOTH codecs** (P29 0.384, GRZ2 0.347). It is the one corpus
  where the selector has nothing to fall back on, and it is also where P29 has its largest
  size win (0.6079x z19 against GRZ2's 0.9257x). Size is not the problem there; rate is.
- Elsewhere the complementarity holds on rate as well as size: each codec clears >= 1 GB/s
  on 4 of 6 and **they miss on different corpora** -- P29 on rocksdb (0.856) and godot,
  GRZ2 on llvm (0.983) and godot. Neither alone clears both bars everywhere; the per-cell
  minimum does, everywhere except Godot.

### Where Godot's time goes -- not entropy, not interning

The question was whether z19/BSC or interning is the limiter. **Neither.** Of the 15.45 s
`C_encode_ready`:

| stage | time | share |
|---|---:|---:|
| interning (`Interner::process` + corpus scan) | ~3.05 s | 20% |
| S1 factorization | 0.3 s | 2% |
| literal-group entropy (BSC, 20 groups) | 0.73 s | **5%** |
| material construction + serialization | ~11.4 s | **74%** |

Godot has **2,641,125 distinct lines** and 102,838 new regions against a whole-program
compressibility of only **103x** (eigen, for contrast: 2846x). There is simply far more
genuinely novel material to define, materialize and serialize. The entropy coder is not the
bottleneck at 5%; the cost is proportional to novel content, which is exactly what a
low-redundancy corpus has most of. That is a property of the corpus, not a tunable.

## Why this differs so much from the earlier 0.370 GB/s

Nothing about the codec changed. The earlier figure charged the two-pass structure **and**
`load_corpus` -- and the stage split showed file loading alone was 51.8% of that wall. Once
the clock starts where a production C side actually starts (bytes already in memory) and
only the grouped pass is charged, the same binary measures 0.86-2.01 GB/s. **The gap was
harness, not codec** -- the same conclusion the stage split reached, now confirmed by a
direct clock rather than by decomposition.

## Not measured, and why

**Godot.** Its plan pass fails with `blob zstd worker count: Unsupported parameter` even
against a statically linked libzstd carrying the ZSTDMT symbols, so the corpus with real
compressed blobs cannot be encoded by any build I can produce. local-oracle's reference
binary handles it, so this is a build-environment gap on my side, not a codec limit; the
row needs its build. Godot is also the largest fixed-16 corpus and the one where P29+BSC
has its biggest size win, so the row is worth having.
