# P22: exact causal prior-root range coding across 16 corpora

## Verdict

Retain prior-root range coding as the next structural codec candidate, but keep it behind the
actual-byte selector until the product-shaped C++ and per-F cache work is complete.

Across all 9,292 TUs and 28,554,671,510 raw `.ii` bytes, the exact structural wire falls from
46,690,193 to 32,838,486 bytes: a 13,851,707-byte, 29.67% reduction. The structural ratio rises from
611.58x byte-weighted / 462.92x equal-corpus to 869.55x / 622.44x. Every corpus wins and every frame
is independently reconstructed exactly.

This is a factor-sized structural gain, not cold-400 closure. Combining it with the exact P21 Line
plane improves complete cold transfer from 133,555,793 bytes / 213.80x to 119,704,086 bytes /
238.54x. Cold 400x still permits only 71,386,679 bytes, leaving a measured 48,317,407-byte excess.

The complementary executed half-warm Line-cache scenarios both pass the aggregate 200x target:
352.66--353.45x byte-weighted and 308.36--308.47x equal-corpus. Thirteen of sixteen individual
corpora pass both complements. Godot is narrowly below 200x; fmt and LevelDB remain materially
below it.

## Exact algorithm

For each C cache generation, treat a completed TU Root as an immutable sequence of stable Regions.
The prototype maintains:

- the ordinary dense first-observation Region dictionary;
- every previously completed exact Root sequence;
- a C-only sparse index from a verified four-Region seed to recent `(root, position)` locations.

The current measured index stores one source position out of every eight and at most four recent
locations for each 64-bit fingerprint. The fingerprint is only a lookup accelerator: every seed and
every extended match is compared against the exact Region tuples before it can be emitted.

For TU `t`:

1. Construct the complete exact Region sequence using state committed through TU `t-1`.
2. At each target position, query completed Roots for the longest exact variable-length range.
3. Compare the raw cost of that range operation with the DEFINE/REFERENCE atoms it replaces.
4. Greedily emit only positive-saving ranges. A dynamic-programming control on fmt differs by only
   79 compressed bytes over the complete corpus, so greedy is the retained parser.
5. Serialize and zstd-compress the complete prior-root candidate.
6. Independently decompress and reconstruct it even if another representation wins.
7. Select it only when its actual compressed bytes plus framing are smaller than every existing
   atom, cross-context, and hybrid candidate.
8. Decode the selected representation, compare the entire Root with trace truth, and only then add
   the completed Root to the learner and F-side Root store.

The research frame is:

```text
version, output_region_count,
  DEFINE(dense_region_id, exact_region_key)
| REFERENCE(dense_region_id)
| COPY_ROOT(prior_root_back, start_region, region_count)
```

`DEFINE` remains the universal first-use form, `REFERENCE` remains the ordinary dense form, and
`COPY_ROOT` merely names an exact contiguous range already present in a completed Root. A changed
Region splits a long range into ranges on either side; reorder and insertion therefore retain large
unchanged spans without changing existing object meanings.

The prototype's relative `prior_root_back` is suitable only for the single-conversation capability
measurement. The product form should use the generation-local stable Root object ID already known
to the assigned F:

```text
ROOT_SLICE(source_root_id32, start_region_u32, region_count_u32)
```

This should be one optional Root-program operation, not a new transport, model, or independent
cache. F expands the named packed-u32 Root vector and then follows the ordinary output path.

## Complete balanced result

| corpus | P18 structure | P22 structure | saving | P21 Line | complete cold | executed half-warm worst |
|---|---:|---:|---:|---:|---:|---:|
| llvm | 3,432,366 | 1,642,398 | 52.15% | 7,362,680 | 402.03x | 609.48x |
| rocksdb | 14,479,861 | 12,857,339 | 11.21% | 4,178,690 | 182.81x | 201.64x |
| duckdb | 5,242,891 | 4,167,260 | 20.52% | 6,914,109 | 179.19x | 242.69x |
| abseil | 7,510,940 | 5,804,742 | 22.72% | 3,138,190 | 288.61x | 333.92x |
| opencv | 4,820,749 | 2,123,347 | 55.95% | 6,380,541 | 544.57x | 774.97x |
| godot | 5,247,034 | 2,580,411 | 50.82% | 51,805,645 | 109.09x | 198.71x |
| fmt | 929,416 | 825,016 | 11.23% | 699,008 | 89.47x | 109.96x |
| spdlog | 275,140 | 185,959 | 32.41% | 512,060 | 140.95x | 202.35x |
| catch2 | 981,891 | 674,388 | 31.32% | 691,243 | 693.64x | 869.95x |
| nlohmann-json | 502,142 | 350,548 | 30.19% | 846,478 | 245.54x | 342.52x |
| range-v3 | 621,391 | 296,158 | 52.34% | 720,583 | 621.64x | 866.10x |
| eigen | 1,140,650 | 299,675 | 73.73% | 1,078,330 | 2,563.32x | 3,772.34x |
| re2 | 243,561 | 139,059 | 42.91% | 453,470 | 186.04x | 269.41x |
| leveldb | 577,535 | 459,685 | 20.41% | 539,172 | 144.03x | 184.48x |
| simdjson | 533,104 | 320,996 | 39.79% | 1,044,406 | 343.03x | 496.02x |
| cereal | 151,522 | 111,505 | 26.41% | 500,995 | 533.71x | 796.74x |

Complete aggregate ledger:

| quantity | P21/P18 | P22 |
|---|---:|---:|
| structural wire | 46,690,193 | **32,838,486** |
| structural byte-weighted ratio | 611.58x | **869.55x** |
| structural equal-corpus ratio | 462.92x | **622.44x** |
| exact P21 Line wire | 86,865,600 | 86,865,600 |
| complete cold wire | 133,555,793 | **119,704,086** |
| complete cold byte-weighted ratio | 213.80x | **238.54x** |
| complete cold equal-corpus ratio | 200.61x | **225.67x** |
| cold-400 corpora | 5 / 16 | **6 / 16** |
| remaining aggregate cold excess | 62,169,114 | **48,317,407** |

## Online learning curve

The learner starts empty. TU 1 is therefore exactly tied. Eleven corpora have a cumulative win by
TU 2, and every corpus has a cumulative win by TU 5. All matching and indexing occur after the
scored TU, so these are causal results rather than rescoring with future state.

| TU checkpoint | eligible corpora | control wire | P22 wire | saving | control ratio | P22 ratio | winning corpora |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 16 | 965,765 | 965,765 | 0.00% | 40.87x | 40.87x | 0 |
| 2 | 16 | 1,535,931 | 1,489,814 | 3.00% | 58.41x | 60.22x | 11 |
| 5 | 16 | 2,283,742 | 2,005,691 | 12.18% | 105.67x | 120.32x | 16 |
| 10 | 16 | 3,045,598 | 2,506,108 | 17.71% | 156.48x | 190.16x | 16 |
| 25 | 16 | 4,672,107 | 3,649,062 | 21.90% | 251.31x | 321.77x | 16 |
| 50 | 15 | 6,721,249 | 5,268,026 | 21.62% | 317.48x | 405.05x | 15 |
| 100 | 10 | 11,419,923 | 9,740,119 | 14.71% | 274.16x | 321.45x | 10 |
| 200 | 9 | 17,094,723 | 14,421,104 | 15.64% | 329.31x | 390.36x | 9 |

The changing eligible count is explicit: corpora shorter than a checkpoint are not silently carried
forward.

## Reorder and input-change stability

The paired matrix uses identical first-200-TU boundaries, the same target-disjoint C-only seed, and
the same existing hybrid controls. The content-change scenario replaces one shared Region identity
throughout the target before encoding.

| scenario | control wire | P22 wire | saving | weighted ratio | equal-corpus ratio | exact wins |
|---|---:|---:|---:|---:|---:|---:|
| standard | 20,134,011 | 16,813,872 | 16.49% | 357.97x → 428.66x | 395.95x → 489.33x | 16 / 16 |
| reverse | 20,197,588 | 16,633,619 | 17.65% | 356.85x → 433.31x | 399.30x → 497.03x | 16 / 16 |
| fixed shuffle | 19,953,640 | 16,713,700 | 16.24% | 361.21x → 431.23x | 402.58x → 493.32x | 16 / 16 |
| shared-Region change | 20,134,069 | 16,809,223 | 16.51% | 357.97x → 428.78x | 396.13x → 489.81x | 16 / 16 |

All 128 reports in the paired matrix are exact: 16 corpora × four scenarios × control/P22. P22 is
strictly smaller in all 64 pairs. The saving range, 16.24--17.65%, is narrow enough to reject the
hypothesis that the result depends on one fortunate TU order.

## Executed half-warm Line cache

The old `structure + 0.5 × Line wire` row was only arithmetic. P22 replaces it with two executed,
complementary cache states:

1. Hash every exact first-use Line with `blake2b-64(person=ice-hc50)`.
2. Preinstall one low-bit partition at F.
3. Encode and independently reconstruct the other partition with the complete P21 codec.
4. Verify per TU that the preinstalled and received partitions are disjoint and merge to the exact
   original Line set.
5. Repeat with the two partitions exchanged.

| cold Line partition | complete wire | weighted ratio | equal-corpus ratio | individual 200x passes | exact |
|---:|---:|---:|---:|---:|---:|
| bit 0 | 80,787,440 | 353.45x | 308.36x | 13 / 16 | 16 / 16 |
| bit 1 | 80,968,672 | 352.66x | 308.47x | 13 / 16 | 16 / 16 |

This is deliberately conservative about structure: every structural byte remains cold. It charges
the actual nonlinear Line compression cost, which is larger than one half of the cold Line wire.
The individual failures in both complements are fmt, LevelDB, and Godot. Godot reaches 199.56x and
198.71x, making it a useful narrow optimization target; fmt and LevelDB require a different Line
mechanism.

## Per-F availability and state

The complete structural headline uses one persistent F Root store. A separate exact probe assigns
the first 200 TUs round-robin to independent F caches, gives each F its own dense dictionary and
Root index, and charges definitions again at every F that needs them. A range may name only a Root
already decoded by that F.

| corpus | independent Fs | baseline wire | P22 wire | saving | copied Regions |
|---|---:|---:|---:|---:|---:|
| DuckDB | 1 | 1,928,086 | 332,983 | 82.73% | 97.40% |
| DuckDB | 4 | 2,421,606 | 886,562 | 63.39% | 93.27% |
| DuckDB | 8 | 2,962,060 | 1,487,456 | 49.78% | 89.08% |
| DuckDB | 16 | 3,970,317 | 2,601,469 | 34.48% | 81.50% |
| DuckDB | 32 | 5,691,126 | 4,513,622 | 20.69% | 69.10% |
| RocksDB | 1 | 12,836,689 | 9,524,477 | 25.80% | 70.96% |
| RocksDB | 4 | 13,464,583 | 10,274,995 | 23.69% | 68.77% |
| RocksDB | 16 | 15,620,776 | 12,811,278 | 17.99% | 61.77% |
| RocksDB | 32 | 18,377,077 | 15,944,061 | 13.24% | 53.55% |

This establishes that the mechanism remains useful when jobs disperse, but it is not the complete
deployment matrix. The final implementation still has to run all 16 corpora with 1/4/8/16/32 Fs,
sticky and shuffled assignment, bounded Root eviction, and actual missing-object request/fill bytes.

Packed logical state at the complete single-F endpoint is at most 24,538,156 bytes at F for Root
u32 vectors and 25,357,540 bytes at C for those vectors plus the sparse index. These figures do not
include existing Region/Line dictionaries and are not Python process RSS. The current research
history is unbounded; production requires an LRU/ARC budget and a per-F known-Root shadow or an
explicit missing-Root response.

## Parser and index tradeoffs

- Greedy versus full dynamic programming on complete fmt differs by only 79 compressed bytes while
  the DP probe takes about 2.6x as long. Retain greedy.
- On DuckDB's first 200 TUs, indexing every Region instead of every eighth Region changes the
  isolated candidate from 332,983 to 321,730 bytes, but increases index state from 293,240 to
  586,936 bytes and reduces measured Python effective input rate from 0.057 to 0.042 GB/s.
- The complete 2,207-TU Godot control confirms the density ceiling: stride one changes structural
  wire from 2,580,411 to 2,473,008 bytes, saving 107,403 bytes while increasing C Root-plus-index
  state from 25,038,008 to 27,576,880 bytes. With the two executed half-warm Line complements this
  yields 200.29x and 199.43x. It therefore clears only one complement and does not close even the
  narrow Godot half-warm failure. Keep stride eight as the balanced baseline; index density remains
  a C-local tuning control and does not alter the operation format.
- Raising the candidate bucket from 4 to 16 on LLVM's first 200 TUs saves only 1,330 bytes while
  nearly doubling index state. Four candidates are sufficient for the product baseline.
- Seed lengths 4 and 8 are effectively tied on LLVM. Four retains better coverage of short changed
  spans and remains the measured default.

The Python harness is a capability implementation, not a speed result. A packed C++ index, Root
vectors, range encoder, and expansion loop must independently demonstrate at least 1 GB/s on the
complete path before promotion.

## Line-plane range-copy falsification

The same variable-range idea was applied to every P21 byte component to test whether it also closes
the 48.32 MB cold gap. It does not:

| corpus / minimum range | P21 Line | range candidate | change | important observation |
|---|---:|---:|---:|---|
| fmt / 256 bytes | 699,008 | 700,244 | -1,236 | only 30,870 suffix bytes copied |
| DuckDB / 64 bytes | 6,914,109 | 6,990,597 | -76,488 | 2.45 MB copied through 32,397 calls; zstd already codes it better |
| Godot / 256 bytes | 51,805,645 | 51,657,604 | +148,041 | only a 0.29% win |

Godot exposes the hard boundary: none of its 38,397,333 underlying byte-array values match a prior
value Root at the 256-byte threshold. Its zstd-3 value stream alone sends 37,054,475 bytes. That is
real cold payload, not spelling overhead. Do not add a generic byte-range operation based on these
results.

## Minimal product boundary for review

Recommended first product-shaped form:

- one optional `ROOT_SLICE(source_root_id32, start32, count32)` operation inside the existing Root
  program;
- a C-side sparse exact-match index over completed packed-u32 Roots;
- the existing whole-frame actual-byte fallback;
- an F-side per-C-GUID packed Root store shared by compiler forks;
- encode-before-learn, immutable Root IDs, and no predictor at F;
- initially emit a slice only when the assigned F's known-Root shadow says the source is resident.

This keeps the blocks separable. Missing-Root fill, block promotion, and cache eviction can be added
without changing Line encoding or the P21 `BYTE_ARRAY` operation.

## Review questions

### To mickg10/bigoracle

1. Should the product operation remain an inline immutable Root slice, or should a profitable slice
   be materialized as an ordinary immutable Block object after its first use?
2. For the first implementation, is per-F known-source selection plus ordinary fallback sufficient,
   or should an F be allowed to request a missing source Root and receive its closure?
3. Choose the smallest stable source identity and eviction rule. The research relative back-distance
   must not become the multi-F protocol identity.
4. Confirm or simplify the sparse index baseline: four-Region seed, stride eight, four recent exact
   candidates, greedy longest-positive-saving parse.
5. Reconcile the remaining 48.32 MB aggregate cold excess. The 38.66 MB array-value block and the
   negative byte-range experiment must remain explicit in the budget.
6. Confirm the complementary hash-partition run as the executed half-warm acceptance scenario, or
   specify a smaller replacement before product gating.

Please keep the ruling to a few independently selectable blocks. P22 does not justify a new model
family or a second cache subsystem.

### To mickg10/implementer

Hold the product port until BigOracle chooses inline slice versus immutable Block and local-only
versus missing-Root fill. Once released, implement the strict C++ capability first and publish:

- exact serialized bytes by operation and frame;
- selected and losing candidate replay;
- complete 16-corpus cold and complementary half-warm results;
- standard/reverse/shuffle/change/revert curves;
- 1/4/8/16/32 F assignment with duplicated definitions and bounded eviction;
- encode/index/update/expand throughput, with at least 1 GB/s required;
- C and F logical state, process peak memory, retained logs, and commit IDs.

## Reproduction and retained evidence

Primary commands are encapsulated by:

```text
python3 linecache/run_prior_root_matrix.py \
  --scenarios standard --modes root --max-tus 0 --jobs 4 \
  --root-copy-index-stride 8 --output-dir /tmp/issue16-prior-root-full

python3 linecache/run_prior_root_matrix.py \
  --scenarios standard reverse shuffle1 perturb --max-tus 200 --jobs 4 \
  --root-copy-index-stride 8 --output-dir /tmp/issue16-prior-root-matrix-200

python3 linecache/half_cold_line_codec.py \
  --trace NAME=linecache/traces/ml-NAME.bin ... --levels 3 \
  --output /tmp/issue16-half-cold-line-16corpus.json
```

Retained run roots:

- `/tmp/issue16-prior-root-full`
- `/tmp/issue16-prior-root-matrix-200`
- `/tmp/issue16-half-cold-line-16corpus.json`
- `/tmp/issue16-godot-root-full-stride1`
- `/tmp/issue16-prior-root-*-200-f*.json`
- `/tmp/issue16-prior-byte-line-*.json`

Committed machine summaries contain the complete per-corpus ledger, learning curve, four stability
scenarios, executed half-warm partitions, and representative multi-F sweep.
