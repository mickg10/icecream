# Issue #16 pretrained-model and ML bakeoff

## Verdict

The useful result is a deliberately simple combination:

1. learn **exact sub-superblocks**, not whole semantic-context runs;
2. use a small immutable count-based top-K context map for their order stream;
3. keep zstd-1 (optionally with a roughly 512 KiB pretrained dictionary) as the outer coder;
4. use a small encoder-only GBDT to reduce exact candidate evaluation to K=4/8;
5. keep GRU/TCN/large language models as teachers only.

The key distinction is now measured:

- A pretrained dictionary on raw Line-definition records is weak: 0–2% charged gain.
- A pretrained dictionary on semantic Root/superblock structure is strong: roughly 1.35–2.4x
  charged gain depending on corpus and stream.
- Exact **whole** superblocks transfer poorly across unrelated projects.
- Exact **sub-superblocks** of 2/4/8/16/32 Regions transfer very well. On held-out DuckDB, a
  1 MiB phrase package plus a 64 KiB top-K map reduces the exact dynamic Region-digest stream from
  31.03 MB to 10.59 MB; after charging the model once, the result is 11.71 MB (2.65x smaller).

This validates “the trick is superblocks,” with one qualification: the reusable unit is a learned
subsequence inside a marker-aligned stream. Whole-run identity is too brittle, and a large neural
next-block model is unnecessary.

The best charged 1 MiB reference row decodes at 0.97 GB/s in Python and therefore does not clear the
runtime gate as measured. The smaller 256 KiB row reaches 1.04 GB/s and is the current speed-eligible
reference point. Neither becomes a production result until the same representation is implemented in
the C++ two-process codec and compared incrementally with its existing dense IDs.

This does **not** establish a new complete cold-400x result. The production codec already uses dense
Region/Block IDs, while the standalone pretrained-superblock harness carries exact Region digests
and definitions. The next acceptance step is therefore incremental integration into the real
two-process codec, where it must beat the existing dense Root/FILL representation after all model
bytes are charged. Repeated-build 400x remains solved by semantic Root reuse.

## Experimental contract

Training projects and the held-out project are disjoint:

| role | project | TUs | raw `.ii` bytes | first Lines | candidate rows |
|---|---|---:|---:|---:|---:|
| train | LLVM | 1,238 | 3,620,271,340 | 771,055 | 5,001,079 |
| train | RocksDB | 622 | 3,114,320,596 | 587,613 | 4,541,121 |
| train | OpenCV | 1,506 | 4,630,994,774 | 586,099 | 3,920,909 |
| test | DuckDB | 689 | 1,985,715,205 | 637,610 | 4,300,903 |

The compact binary event schema contains chronological TU, Region, Line, provenance, candidate,
and actual program-cost records. The four exports contain 2.58 million first Lines, 17.76 million
candidate rows, and 12.44 GiB of original preprocessed input.

The rows have different acceptance meanings and are labelled accordingly:

- `region_codec_bench`: actual Region records, zstd frames, independent Region store, and exact full
  TU byte reconstruction.
- `pretrained_superblocks`: actual static/dynamic/predicted records, zstd frames, independent
  decoder, and exact Region-digest sequence reconstruction. It is a structure/identity capability,
  not a standalone full-TU transport.
- `table_codec_bench`: actual integer-table range-coded bytes, independent decoder, and exact
  superblock payload reconstruction in C++.
- rankers: encoder-only selection models. F receives the selected explicit exact program; it does
  not need the ranker.
- external language model: cross-entropy ceiling only, explicitly not an exact codec row.

All online FTRL scores obey encode-before-learn and update only after the scored TU. All pretrained
rows are frozen before DuckDB is read. Every accepted byte row decompresses and compares its entire
output; no ratio is inferred from a proxy.

## 1. Exact heterogeneous Region materializer

The C++ Region harness implements `RAW`, `STATIC_EXACT`, `SOURCE_PROGRAM`, `PRIOR_REGION`, and
optional `LINE_VIEW`/mixed records. Each TU is independently framed with zstd-1 and compared with a
whole-frame raw fallback.

DuckDB full run:

| row | actual wire bytes | ratio to 1.986 GB input | encode GB/s | decode GB/s | result |
|---|---:|---:|---:|---:|---|
| raw Region | 11,779,650 | 168.57x | 3.47 | 4.23 | exact |
| prior Region | 11,750,704 | 168.99x | 4.04 | 4.41 | exact |
| optional Line views | 11,657,252 | 170.34x | 0.82 | 4.46 | exact |
| static exact, 64 MiB cap | 11,244,068 | 176.60x | 3.22 | 4.51 | exact |
| source + prior | 11,577,960 | 171.51x | 3.21 | 4.68 | exact |
| full union | **11,162,776** | **177.89x** | 0.90 | 4.58 | exact |

The result is useful as a falsification: changing the public object boundary and adding direct
source/static/prior references does not approach DuckDB's 4.155 MB FILL allowance in this first
implementation. Optional Line views save about 122 KB alone and about 617 KB in the full union, but
their current hash/view bookkeeping also falls just below the 1 GB/s encode gate.

The separate origin probe explains why. Across held-out DuckDB, exact static Regions from the other
three projects cover only 2.8–3.6% of first Region bytes, while exact source Lines account for about
45% of attributed bytes. Exact whole prototypes are too coarse; source-conditioned programs and
smaller reusable phrases are the meaningful priors.

## 2. Pretrained zstd dictionaries

Definition, Root-digest, and dynamic-superblock streams were trained on the other three projects
and replayed exactly on each held-out project. Practical 32/64/128/256 KiB dictionaries were swept
at zstd levels 1 and 3.

Best charged zstd-3 gains:

| held-out project | Line definitions | Root digest | superblock stream |
|---|---:|---:|---:|
| DuckDB | 1.00x | 2.31x | 1.92x |
| LLVM | 1.01x | 2.01x | 1.61x |
| RocksDB | 1.01x | 1.45x | 1.35x |
| OpenCV | 1.02x | 2.04x | 1.93x |

The extended DuckDB sweep shows that “larger” is not monotonically better. At zstd-3:

| stream | no dictionary | best actual dictionary | wire | wire + dictionary | charged gain |
|---|---:|---:|---:|---:|---:|
| Root digest | 46,146,618 | 512 KiB | 18,604,487 | 19,128,775 | 2.41x |
| dynamic superblocks | 30,348,679 | 512 KiB | 14,731,089 | 15,255,377 | 1.99x |

The 1 MiB dictionaries reduce uncharged bytes slightly further on some rows but lose after their own
bytes are counted. Multi-megabyte requests are worse. A roughly 512 KiB dictionary is the useful
part of this idea.

## 3. Exact pretrained superblock programs

### Whole-run control

The first model used complete semantic-context runs as static assets. It failed cleanly: at the
4 MiB model point only 0.15% of held-out teacher tokens were known, and even a 64 MiB static package
reduced DuckDB wire by only about 109 KB before charging the model. Whole-run equality is too strict.

### Learned sub-superblocks

The second model learns exact phrases of 2/4/8/16/32 stable Region identities. Asset selection is by
training-only saved-byte value per model byte. C greedily covers each marker-aligned Region sequence
with the longest exact static phrase; unmatched gaps use dynamic DEFINE/REF. A frozen `(previous-2,
previous-1) -> top-8` table can encode a known phrase by predicted rank. F expands either form to the
same exact Region-digest sequence.

Held-out DuckDB results (zstd-1 frames):

| raw phrase cap | assets | top-K map | wire | wire + model | charged ratio | enc GB/s | dec GB/s |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 1 B | 31,028,369 | 31,028,370 | 64.00x | 5.05 | 1.28 |
| 256 KiB | 4,704 | 24,312 B | **11,603,624** | **11,890,069** | **167.01x** | 2.36 | 1.04 |
| 1 MiB | 13,729 | 65,669 B | **10,591,459** | **11,705,704** | **169.64x** | 2.09 | 0.97 |
| 4 MiB | 39,463 | 199,075 B | 10,195,008 | 14,588,383 | 136.12x | 2.48 | 1.14 |
| 16 MiB | 90,887 | 409,619 B | 9,881,624 | 27,068,452 | 73.36x | 3.17 | 1.29 |
| 64 MiB | 232,533 | 841,726 B | 9,816,032 | 77,766,607 | 25.53x | 2.89 | 1.21 |

The useful package is 256 KiB–1 MiB. Bigger packages keep shaving uncharged wire, but model debt
dominates. The 1 MiB row is the best final charged total; the 256 KiB row has the best early debt.

A trained dictionary on top of the 1 MiB phrase row is only a finishing effect:

| outer dictionary | phrase wire | phrase + model total | charged ratio |
|---:|---:|---:|---:|
| none | 10,591,459 | 11,705,704 | 169.64x |
| 64 KiB | 10,455,568 | 11,635,349 | 170.66x |
| 128 KiB | **10,371,034** | **11,616,351** | **170.94x** |
| 256 KiB | 10,498,467 | 11,874,856 | 167.22x |

The best 128 KiB dictionary saves only 89,353 charged bytes (0.76%) beyond the phrase/map row. The
sub-superblock representation creates almost all the value; the second model object is optional.

Charged learning curve:

| raw fraction | no pretraining | 256 KiB phrases + map | 1 MiB phrases + map |
|---:|---:|---:|---:|
| 10% | 82.0x | 192.7x | 113.1x |
| 25% | 68.4x | 233.0x | 184.1x |
| 50% | 61.5x | 227.8x | 213.6x |
| 75% | 61.7x | 184.7x | 184.3x |
| 100% | 64.0x | 167.0x | 169.6x |

The 256 KiB package crosses 200x early but does not sustain it through the diverse late portion of
DuckDB. The 1 MiB package repays its larger initial debt only near the half-build point. This is why
model size and chronology must be reported together.

## 4. ML candidate rankers

The label is actual byte reward, and the metric is byte regret after exact evaluation of top K.

### C++ online FTRL, full chronological DuckDB

| K | best-candidate recall | byte regret/event |
|---:|---:|---:|
| 1 | 38.37% | 5.2418 |
| 2 | 59.31% | 3.2483 |
| 4 | 89.52% | 1.1384 |
| 8 | 99.89% | 0.0048 |

The same full-corpus K=8 regret is 0.0024 bytes/event on LLVM, 0.0085 on RocksDB, and 0.0267 on
OpenCV. FTRL is a strong cheap online baseline when eight exact candidates are affordable.

### Disjoint-project GBDT, full DuckDB

| model | bytes | native candidate scores/s | effective raw GB/s, inference only | regret K=1 | K=4 | K=8 |
|---|---:|---:|---:|---:|---:|---:|
| 32 trees, depth 5 | **54,136** | **5.51 M/s** | **2.54** | 0.5305 | 0.0717 | 0.00035 |
| 256 trees, depth 8 | 814,565 | 0.686 M/s | 0.317 | **0.1153** | **0.0161** | 0.00046 |

The 32-tree row is the product-shaped winner. It clears the inference gate with margin, is smaller
than the neural models, and makes K=4 cheap enough for nearly negligible coding regret. The larger
GBDT buys only 0.055 byte/event at K=4 while making inference about eight times slower.

### PyTorch candidate teachers, 30,000 held-out DuckDB events

| model | float bytes | quantized state | candidate scores/s | regret K=1 | K=4 | K=8 |
|---|---:|---:|---:|---:|---:|---:|
| shared byte-CNN pair ranker | 346,724 | 172,208 | 10.4 K/s | **0.2086** | **0.0148** | 0 |
| dual context/base encoder | 361,828 | 180,556 | 9.8 K/s | 0.4624 | 0.0714 | 0.00027 |
| source/context MLP | 27,652 | 12,095 | **43.8 K/s** | 0.6091 | 0.0697 | 0.00057 |

The pair model matches the large GBDT's K=4 quality but is roughly 66 times slower. The dual encoder
does not find a quality/speed advantage on this candidate pool. The small metadata model is useful
as a teacher, but the 32-tree GBDT is both faster and at least as accurate at the selected K.

## 5. Neural superblock teachers and distillation

With a 4,096-phrase vocabulary, the phrase representation raises held-out known-token coverage from
0.15% (whole runs) to 57.1%.

| teacher | float model | recall@1 | recall@8 | teacher tokens/s | distilled map | exact distilled wire |
|---|---:|---:|---:|---:|---:|---:|
| 2-layer GRU | 2.49 MB | 89.5% | 94.8% | 14.2 K/s | 92.8 KB | 12,257,255 |
| causal TCN | 2.84 MB | 79.8% | 89.7% | 20.9 K/s | 92.5 KB | 12,465,196 |
| count top-K control | — | — | — | table lookup | 24.3 KB at 256 KiB assets | **11,603,624** |

The teachers demonstrate real sequence predictability, but their distilled maps lose to the small
count table after actual zstd frames are measured. The predictor is not the bottleneck once exact
sub-superblocks exist. Do not put a neural model in F for this path.

## 6. Integer mixture/table teacher

A training-only mixture model clusters 4,096 deterministic two-byte contexts into 64/128/256
integer probability tables. The C++ range coder uses the selected table, derives context solely from
already decoded bytes, reconstructs every target payload exactly, and includes model bytes.

| tables | model | DuckDB wire | charged wire | charged ratio | encode GB/s | decode GB/s |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 40,984 | 30,244,878 | 30,285,862 | 65.57x | 4.75 | 2.25 |
| 128 | 73,752 | 30,087,715 | 30,161,467 | 65.84x | 4.80 | 2.16 |
| 256 | 139,288 | **29,910,757** | **30,050,045** | **66.08x** | 4.77 | 2.01 |
| zstd-1 control | 0 | 31,056,778 | 31,056,778 | 63.94x | 25.9 | 55.4 |

The table coder is exact and comfortably clears the effective speed gate, but its roughly 3.2%
charged gain is too small to justify replacing zstd on the broad stream. It remains a good backend
candidate only after opcodes/slots have been separated into much smaller, better-conditioned streams.

## 7. External pretrained language-model ceiling

The code-specific teacher row is intentionally separated from accepted codecs. Its upstream
training corpus is not controlled against DuckDB, and it emits no exact range-coded bitstream. On a
32 KiB reservoir of exact DuckDB definition residuals, Qwen2.5-Coder-0.5B measures:

| metric | result |
|---|---:|
| parameters / resident parameter bytes | 494,032,768 / 988,065,536 |
| teacher cross-entropy | **1.903 bits/sample byte** |
| zstd-1 / zstd-3 / zstd-6 controls | 4.155 / 3.844 / 3.648 bits/sample byte |
| inference | 220.6 tokens/s; **482 sample bytes/s** |
| exact bitstream | no (ceiling only) |

The result confirms that generic code pretraining contains residual predictability that the small
byte-history controls miss. It also confirms that the direct model is unusable here: nearly 1 GB
of weights and sub-KB/s input rate are several orders away from the model and throughput budgets.
Only distillation into the already-tested small trees/tables is justified. Because upstream training
overlap is uncontrolled, even the 1.903-bit number must be treated as optimistic.

## 8. Recommendation

### Implement next

1. Export the 32-tree GBDT to the C++ encoder and evaluate exactly K=4 and K=8 in the real Region
   program builder. Keep F model-free.
2. Add a 256 KiB and 1 MiB static sub-superblock package as optional immutable model objects. Use
   training-only selection and the small count top-K map.
3. Treat the measured 128 KiB phrase-stream dictionary as optional: it improves the charged row by
   only 0.76%. Keep it only if the C++ integration makes it essentially free to package and cache.
4. Integrate the winning phrase row into the real two-process codec before claiming any end-to-end
   ratio. Compare against its existing dense Root/Block IDs, not against the 128-bit digest control.
5. Preserve S0 semantic Root reuse unchanged. It remains orthogonal and is the decisive repeated-
   build result.

### Do not implement in the runtime path

- whole-run pretrained superblocks;
- GRU/TCN inference at C or F;
- the dual encoder on the current candidate pool;
- multi-megabyte zstd dictionaries;
- the broad-stream table coder as a zstd replacement;
- a large shared language model.

### Interpretation of the implementer's 7.04 MiB DuckDB result

That measurement is strong evidence that project-owned source dominates the remaining cold content
under the tested source-conditioned representation. It is not, by itself, a proof against every
exact program representation. The sub-superblock experiment demonstrates a real cross-project
representation gain that whole-Region accounting misses. However, its best charged standalone total
is still far above the cold FILL allowance, and none of the ML rows produces the missing factor.

The honest conclusion is therefore aligned in practice: strict cold DuckDB remains below the 400x
target in the measured real codecs; repeated-build 400x is solved; and the only remaining low-risk
research bet is the compact sub-superblock package plus small encoder-only ranker, integrated and
measured incrementally.

## Reproduction

Core builds:

```sh
g++ -O3 -DNDEBUG -std=c++17 linecache/ml_bakeoff.cpp -lzstd -o linecache/ml_bakeoff
g++ -O3 -DNDEBUG -std=c++17 linecache/region_codec_bench.cpp -lzstd -o linecache/region_codec_bench
g++ -O3 -DNDEBUG -std=c++17 linecache/table_codec_bench.cpp -lzstd -o linecache/table_codec_bench
```

The retained command logs and machine-readable summaries are under `linecache/traces/` and
`linecache/ml-artifacts/`. Large `.bin` event exports and `.frames` replay files are generated local
artifacts and are intentionally not suitable for source control.

## Verification completed for this handoff

- All four new C++ programs compile with `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror`.
- All four Python programs pass `python3 -m py_compile`.
- Three-TU C++ Line and Region smoke runs independently decode every tested row: `exact=PASS`.
- The pretrained phrase smoke run independently decodes all five tested rows: `exact=PASS`.
- The complete 689-TU fixed-table replay independently decodes both the table-coded and zstd-1
  controls: `exact=PASS` (4.90/2.37 GB/s table encode/decode in the final rerun).
- The ordinary project build succeeds, and `make check` reports 1/1 pass. The local configure used
  unpacked development headers/libraries because this host only had the corresponding runtime
  packages installed; no host-wide package installation was needed.
- `git diff --check` passes.
