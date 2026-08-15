# PTGC definition-coding and pretrained-model bakeoff

## Verdict

The independently decoded capability harness is complete enough to reject several tempting shortcuts,
but not complete enough to close the project-attributed PTGC question.

On all 689 DuckDB translation units (1,985,715,205 raw bytes), the best exact row currently implemented
is a per-TU actual-cost union of local parameterized templates, semantic stream splitting, raw fallback,
and a 16 KiB cross-project pretrained package:

```text
P0 literal definition frames                    11,060,591 bytes
P4 alpha-normalized one-Line templates           9,182,120 bytes
P10 best local frame                              9,157,694 bytes
P10 + charged 16 KiB pretrained union             9,105,123 bytes
```

The best row is 1,955,468 bytes smaller than P0 (1.215x), but remains 8.683 MiB. Pretraining is a
small finishing component here, not the missing factor-of-two representation.

The decisive residual is visible rather than guessed: P9 still emits 5,261,865 compressed bytes of
literal definitions for 298,659 lines that its parameterized rule representation does not cover.
That one channel alone is larger than the proposed 3.8 MiB project-content continuation line.

Do not integrate this definition codec into the live two-process path yet. Preserve P4, the semantic
split, the 16 KiB package, and the small encoder ranker as controls. The next capability row must use
source-location variants and then, if necessary, the minimal contributing-source token pack.

## What this harness measures

The input is the chronological stream of first-seen exact Line definitions from complete `.ii`
translation units. For every row:

1. C sees all new definitions in the current TU and may reorder them because stable IDs are explicit.
2. The row serializes actual control and payload bytes and wraps them in zstd frames at level 3.
3. A separate decoder receives only the row's decoded frame bytes and any explicitly charged model.
4. The decoder installs every definition by stable ID.
5. Every reconstructed definition is byte-compared with the independent truth store.

All rows pass the exact reconstruction gate on all 637,610 distinct DuckDB Lines. Training manifests
are read and frozen before the DuckDB manifest is loaded.

This harness currently covers the whole first-use definition plane, including project and toolchain
content. BigOracle's approximately 7.04 MiB P0 and 3.8 MiB continuation line refer to a
project-attributed/source-aware decomposition. The byte totals below must therefore not be presented
as a direct pass/fail of that narrower line. Source-location attribution is the next required pass.

## Implemented counterfactual ladder

| row | implemented representation |
|---|---|
| P0 | Appearance-order literal definitions with explicit stable IDs |
| P1 | TU-local lexicographic reorder with explicit stable IDs |
| P4 | Conservative byte lexer, alpha-normalized one-Line templates, repeated-slot equality, TU-local rules and lexicon, columnar instances |
| P5 | Actual best of fixed 2/4/8/16-Line parameterized units |
| P6 | Bounded TU-local token COPY/ADD forest over coarse-shape neighbors |
| P7 | Cross-TU persistent parameterized rules and tokens, updated after each encoded TU |
| P7b | TU-local rules plus cross-TU persistent tokens |
| P8 | Frozen cross-project parameterized templates/tokens, columnar superblocks, online additions, and optional model-derived zstd history |
| P9 | P4 split into control, rule-literal, raw-definition, identifier, number, and string streams |
| P10 | Actual serialized per-TU union of P1/P4/P5/P6/P9 with raw fallback |
| P10+P8 | P10 extended with charged pretrained dictionary/model frames |

The P7 name in BigOracle's full ladder is reserved for source-location medoids and token edits. The
current cross-TU template row is therefore labelled P7 only as an intermediate and must not be
confused with the still-missing source-location row.

## Authoritative DuckDB result

Command basis:

```sh
g++ -O3 -DNDEBUG -march=native -std=c++17 \
  -Wall -Wextra -Wpedantic -Werror \
  linecache/ptgc_bench.cpp -o /tmp/ptgc-bench -lzstd

/tmp/ptgc-bench \
  --manifest /tanksmall/scratch/ictmp/corpus3/manifest.txt \
  --z 3
```

| row | wire bytes | MiB | raw/wire | vs P0 | encode-effective GB/s | decode-effective GB/s | trailing 5%-raw |
|---|---:|---:|---:|---:|---:|---:|---:|
| P0 appearance | 11,060,591 | 10.548 | 179.5x | 1.000x | 6.80 | 16.28 | 144.2x |
| P1 lexicographic | 11,070,283 | 10.557 | 179.4x | 0.999x | 7.36 | 20.90 | 148.2x |
| P4 one-Line templates | 9,182,120 | 8.757 | 216.3x | 1.205x | 0.26 | 7.57 | 182.6x |
| P5 multiline | 10,480,697 | 9.995 | 189.5x | 1.055x | 0.18 | 12.81 | 146.9x |
| P6 token forest | 12,612,618 | 12.028 | 157.4x | 0.877x | 0.29 | 16.34 | 122.0x |
| P7 online templates | 9,461,691 | 9.023 | 209.9x | 1.169x | 0.49 | 5.42 | 168.4x |
| P7b local rules/online tokens | 9,500,553 | 9.060 | 209.0x | 1.164x | 0.49 | 6.50 | 169.4x |
| P9 semantic split | 9,316,268 | 8.885 | 213.1x | 1.187x | 0.27 | 7.85 | 182.8x |
| P10 local actual-cost union | **9,157,694** | **8.733** | **216.8x** | **1.208x** | 0.06 | 9.63 | 185.9x |

The P10 encoder number is the cost of running every research candidate, not a production selector.
The existing 54,136-byte, 32-tree encoder ranker remains the production-shaped way to reduce exact
candidate evaluation. P4's current implementation also parses and serializes both normalization
variants; it has not received a throughput optimization pass because its byte result misses the
continuation line.

P10 cumulative raw/wire ratios at 10/25/50/75/100% of chronological raw input are:

```text
100.4x, 232.4x, 189.3x, 190.3x, 216.8x
```

The non-monotonic curve is real: early common headers amortize rapidly, followed by project-owned
content later in the build.

## P4 structural ledger

```text
rules                         24,877
template instances           339,430
template-covered input        24.82 MiB
literal fallbacks            298,180
unique rule slots            106,158
slot occurrences             132,399
TU-local lexicon entries      92,258
TU-local lexicon references 1,467,565
wire control                   2.661 MiB
wire mixed data                6.096 MiB
```

Alpha normalization and equality-constrained slots are real wins: P4 removes 1,878,471 bytes from
P0. The limitation is coverage and slot/control coding, not exact reconstruction.

Fixed multiline rules do not help. P5 selects only 2,539 rules and 40,115 instances, then finishes
at 10,480,697 bytes. Fixed chunk boundaries are too brittle; any future multiline attempt should be
marker/statement aligned or use rolling parameterized sub-superblocks.

The current P6 forest also loses. It performs 1,425,118 bounded comparisons and reconstructs 300,489
deltas, but its COPY/ADD program and base-ID cost expand the stream to 12,612,618 bytes. This rejects
that bounded coarse-neighbor forest, not source-location medoids or a stronger candidate set.

## Semantic stream result

P9 carries the same exact definition program as P4 but places payload classes in separate frames.

| stream | exact zstd-3 wire bytes | MiB |
|---|---:|---:|
| control | 2,790,496 | 2.661 |
| rule literals | 127,801 | 0.122 |
| raw definitions | **5,261,865** | **5.018** |
| identifiers | 628,789 | 0.600 |
| numbers | 166,663 | 0.159 |
| strings | 340,654 | 0.325 |
| total | 9,316,268 | 8.885 |

P9 by itself loses to P4 because extra frames cost more on some TUs. The exact per-TU P10 union keeps
only winning P9 frames and saves 24,426 bytes versus P4.

This breakdown sharply narrows further probability work. Better coding of identifiers, numbers,
strings, and rule literals cannot close the gap while the 5.018 MiB raw-definition channel remains.

## Pretrained superblocks and model-size curve

The model is trained on disjoint LLVM, RocksDB, and OpenCV `.ii` streams:

```text
training raw                    10.59 GiB
training Line occurrences     407,414,015
per-corpus distinct Lines       1,944,767
candidate parameterized rules     375,228
candidate exact tokens            615,767
```

Selection requires presence in at least two training corpora and ranks saved skeleton/token bytes per
serialized model byte. The model is serialized, independently decoded, compressed at zstd-3, and
charged once. The held-out manifest is not opened until the package is frozen.

Full DuckDB charged results:

| raw model cap | model raw | model wire | selected rules/tokens | P10+P8 wire bytes | MiB |
|---:|---:|---:|---:|---:|---:|
| **16 KiB** | 15.0 KiB | 5.8 KiB | 472 / 634 | **9,105,123** | **8.683** |
| 32 KiB | 30.2 KiB | 11.3 KiB | 868 / 1,283 | 9,129,136 | 8.706 |
| 64 KiB | 60.7 KiB | 21.5 KiB | 1,621 / 2,404 | 9,132,696 | 8.710 |
| 256 KiB | 244.6 KiB | 78.3 KiB | 5,783 / 8,351 | 9,155,539 | 8.731 |

The 16 KiB package is the charged winner. It serves 88,401 held-out definitions through 11,383
pretrained columnar superblocks. Used only as zstd history over P4, it saves 62.4 KiB before charging
its 5.8 KiB wire package. The final actual-cost union saves 52,571 bytes versus untrained P10.

The larger package covers more content but loses on debt. The 256 KiB static-superblock row reaches
9,392,460 bytes; its final union improves untrained P10 by only 2,155 bytes after charge.

The package is intentionally a superblock model first and a compression dictionary second. Its
primary entries are parameterized templates and reusable slot values; its serialized bytes can then
also initialize zstd history. That gives one charged artifact two independent uses. A decoder may
omit the history path whenever ordinary zstd wins without changing the superblock language.

### Live-codec literal-superblock control

An exact Region-identity package was also wired into the existing codec as a control. LLVM, RocksDB,
and OpenCV produced a 1.00 MiB raw package (179,904 bytes after zstd-3) containing 1,762 exact Regions
and 2,656 multi-Region phrases. On held-out DuckDB it matched only 1,748 of 210,074 distinct Regions,
covering 0.98 MiB, or 0.05% of raw input. Its phrases covered 1,826 Region occurrences. F first
decompresses and reconstructs its own frozen package from the charged model frame; its Region and
phrase expansion does not read C's model objects.

| live-codec row | total charged wire | result |
|---|---:|---:|
| no pretrained package | 12,442,305 bytes | byte-exact PASS |
| 1 MiB exact-Region package | 12,498,093 bytes | byte-exact PASS; **55,788 bytes larger** |

The control still reaches a split-path proxy of 1.40 effective GB/s at C and 5.38 GB/s at F, so the
failure is not execution speed. Exact literal Regions generalize too rarely to repay their model.
This is why the positive package uses parameterized columnar superblocks rather than a large bank of
verbatim headers.

## Fixed integer-table experiment

The semantic exporter independently reconstructs all frames for:

```text
training: LLVM + RocksDB + OpenCV, 3,366 TUs, 10.59 GiB
test:     DuckDB, 689 TUs, 1.986 GB
```

Six independent 64-table models were trained over two-byte byte contexts for control, rule literals,
raw definitions, identifiers, numbers, and strings. A C++ range decoder reconstructs every exported
frame exactly. Every whole-stream table row loses to zstd-3 before model charge.

Per-TU actual-cost fallback finds isolated table wins, but separate 40,984-byte model charges erase
them for five of six streams. The only positive row is the control stream. Its size sweep is:

| tables | model bytes | actual union before model | charged union | zstd-3 baseline | charged saving |
|---:|---:|---:|---:|---:|---:|
| 16 | 16,408 | 2,760,560 | 2,776,968 | 2,793,252 | 16,284 |
| **32** | **24,600** | **2,750,663** | **2,775,263** | **2,793,252** | **17,989** |
| 64 | 40,984 | 2,738,916 | 2,779,900 | 2,793,252 | 13,352 |

The 32-table control row encodes at 14.8 effective raw GB/s and decodes at 64.4 GB/s in the isolated
test. A single shared 64-table model across all semantic streams still loses after its one model
charge. This is a useful finishing component only after a stronger representation removes the raw
fallback channel.

## Pretraining conclusion

The measured hierarchy is consistent across both the earlier structure-plane study and this new
definition-plane study:

1. Exact Region-identity sub-superblocks are valuable for the Root/structure plane.
2. Parameterized columnar superblocks generalize across projects in the definition plane.
3. A very small pretrained package pays back; larger packages do not.
4. A pretrained zstd history or integer table is a finishing effect.
5. No pretrained model tested here solves project-specific raw definition content.

The model should remain a set of independent blocks: templates/tokens, optional zstd history,
encoder-only ranker, and optional control table. A losing block can be omitted without changing the
wire language or decoder identity.

## Source attribution and source-location P7

The marker-derived source row now reproduces the implementer's independent attribution exactly:

| first-use definition category | raw bytes | independent zstd-3 |
|---|---:|---:|
| marker | 7.564 MiB | 0.327 MiB |
| toolchain | 1.902 MiB | 0.362 MiB |
| project | 42.732 MiB | 7.042 MiB |

P7 retrieves prior exact/medoid/basename/skeleton candidates and current-TU same-location or
same-skeleton candidates, evaluates exact PrefixSuffix and bounded token COPY/ADD programs, emits
real frames, and reconstructs every definition in an independent decoder. The learner observes a TU
only after it has been scored.

Full held-out DuckDB at zstd-3:

| row | wire bytes | raw/wire | result |
|---|---:|---:|---|
| P7 source-location | 10,444,367 | 190.1x | exact |
| P10 actual per-TU union with P7 | 9,154,792 | 216.9x | exact |

P7's candidate programs remove 17.90 MiB before the outer coder, but the mixed frames are larger
than literal frames for nearly every category/TU. Location-first ordering improves P7 by 131,363
bytes over the first implementation, yet improves P10 by only 128 bytes.

The decisive chronology result is that all 44.63 MiB of non-marker variant content is first-variant
content; later-TU variant bytes are zero. The 1,177,265 observed locations have the following final
variant histogram:

```text
one variant      1,150,999
two variants           606
three variants       7,708
four variants          379
five or more        17,573
```

Prior-location history therefore cannot solve the cold definition plane. Same-TU candidates account
for 16.66 MiB of raw candidate saving, but record-at-a-time corrections disrupt outer-frame locality
and lose to grouped literals.

## Contributing-source superblock result

A 20-TU direct source-basis probe gives a stronger continuation:

```text
first-use definition bytes                         4,386,735
bytes with an accessible project source line       2,761,414
exact source-line matches                            532,091
PrefixSuffix residual against the source line        190,395
unique contributing source-line bytes              2,729,728
```

The implemented P7p codec collects only source lines referenced by current-TU first-use definitions,
deduplicates them, sorts them by file and logical line, assigns dense frame-local IDs, and emits four
separable frames:

```text
source-basis control
source-basis bytes
definition control
exact residual bytes
```

It sweeps maximum residual fractions of 0/6/12/25/50/100%, reconstructs every definition from the
chosen basis plus exact PrefixSuffix correction, and compares the complete candidate against the
complete literal frame. Bases are TU-local in this row, so a losing frame cannot create uncharged
decoder state.

P9s applies the same source basis only to P9's hard raw-definition channel. The P9 control stream
already contains raw-definition IDs, order, and lengths, so P9s carries only a source-use bitmap and
source correction metadata; it does not duplicate those fields. Its decoder independently parses P9
control, rebuilds the raw payload from the four source frames, and then runs the ordinary semantic
decoder.

Full DuckDB:

| row | wire bytes | raw/wire | result |
|---|---:|---:|---|
| P7p source superblock over all definitions | 10,989,593 | 180.7x | exact |
| P9 semantic baseline | 9,316,268 | 213.1x | exact |
| P9s semantic + source frame fallback | 9,316,957 | 213.1x | exact |
| P10 including the source rows | 9,154,792 | 216.9x | exact |

P7p wins 181 individual TU comparisons but loses in aggregate. P9s wins no raw-channel TU frame;
its 689-byte difference from P9 is exactly its per-TU selector cost. The full raw-channel accounting
is:

```text
ordinary P9 raw channel                 5.018 MiB
best plain source-superblock channel    5.516 MiB
accessible source targets              24.48 MiB / 257,301 definitions
chosen source bases                     15.25 MiB / 93,467 bases
chosen exact residual                    0.01 MiB
```

The tiny residual confirms that source location predicts emitted content well. The loss comes from
paying for almost one source basis per definition; compressing the source lines in file order does
not repay the basis/control split versus zstd over the emitted literals.

## Pretrained source-superblock dictionary

The dictionary trainer reads only disjoint LLVM, RocksDB, and OpenCV manifests, extracts their
marker-referenced project files, and groups consecutive source lines into immutable 32-line/8-KiB
training superblocks. DuckDB is not opened until every package is frozen.

```text
training project files           8,080
training source lines        3,794,831
training superblocks           122,233
training sample bytes      142,862,614
training time                  198.2 s
```

The 32/64/128/256-KiB packages all reconstruct exactly. The 20-TU charged dominance check is:

| raw dictionary | package wire | P10 + dictionary | delta versus P10 |
|---:|---:|---:|---:|
| 32 KiB | 13.4 KiB | 974,802 | +13,530 |
| 64 KiB | 31.0 KiB | 992,874 | +31,602 |
| 128 KiB | 51.3 KiB | 1,013,768 | +52,496 |
| 256 KiB | 99.9 KiB | 1,063,415 | +102,143 |

The 32-KiB package strictly dominates the larger rows and received the authoritative full replay.
The receiving dictionary is rebuilt from a separately decompressed charged package; every source
frame carries an explicit plain/dictionary selector.

| full 689-TU row | charged wire | delta versus matching baseline |
|---|---:|---:|
| P9s + 32-KiB trained source dictionary | 9,329,702 | +13,434 versus P9 |
| P10 + 32-KiB trained source dictionary | 9,168,276 | +13,484 versus P10 |

The dictionary wins 6,435 individual frame alternatives and removes 527.6 KiB across all evaluated
alternatives, but that is not an achievable sum because only one threshold/frame representation may
be selected. The best actual dictionary source channel is 5.425 MiB, still above P9's 5.018 MiB.
Only 29 TU raw frames select it. The model is useful; the source-line basis it decorates remains too
expensive.

## PTGC ruling and remaining representation work

No source row or pretrained model should be integrated into the transport from these measurements.
The best charged definition result remains P10 plus the earlier 16-KiB parameterized-superblock
package at 9,105,123 bytes. Source locations are highly predictive, but neither prior output variants
nor whole contributing source lines provide the missing representation.

If definition research continues, the remaining distinct row is a minimal contributing-token-span
superblock: transmit only source spans actually copied into emitted output, parameterize repeated
span/correction layouts across a TU, and leave raw fallback available. It must beat P9's raw channel
before any further ranker, neural teacher, larger dictionary, reorder/edit campaign, or product
integration is justified. More model work over the current source-line basis is now measured as a
dead end.

## Reproduction and retained artifacts

Environment for the authoritative local run:

```text
base commit: f1314a9fd404df069c2a8a72bebc28fdd11eace6
compiler:    g++ 11.4.0
libzstd:     1.4.8
trainer:     python-zstandard 0.25.0
machine:     Intel Xeon Gold 6136, Linux 5.15 x86-64
```

Source-superblock reproduction:

```sh
python3 linecache/train_source_superblock_dict.py \
  --train-manifest /tanksmall/scratch/ictmp/corpus/manifest.txt \
  --train-manifest /tanksmall/scratch/ictmp/corpus2/manifest.txt \
  --train-manifest /tanksmall/scratch/ictmp/corpus5/manifest.txt \
  --dict-kib 32 64 128 256 --sample-mib-per-corpus 64 \
  --output-prefix /tmp/ptgc-source-superblock

g++ -O3 -DNDEBUG -march=native -std=c++17 -pthread \
  -Wall -Wextra -Wpedantic -Werror linecache/ptgc_bench.cpp -lzstd \
  -o /tmp/ptgc-source-pack-final

/tmp/ptgc-source-pack-final \
  --manifest /tanksmall/scratch/ictmp/corpus3/manifest.txt \
  --source-k 4 --z 3 \
  --source-dict /tmp/ptgc-source-superblock-32k.dict
```

Primary logs:

```text
/tmp/ptgc-authoritative-full.log
/tmp/ptgc-final-corrected-full.log
/tmp/ptgc-final-16k-full.log
/tmp/ptgc-final-32k-full.log
/tmp/ptgc-final-64k-full.log
/tmp/ptgc-semantic-export-{llvm,rocksdb,opencv,duckdb}.log
/tmp/ptgc-sanitize.log
/tmp/ptgc-source-p7-v2-full-k4.log
/tmp/ptgc-source-p7-strict-smoke.log
/tmp/ptgc-source-p7-sanitize.log
/tmp/ptgc-source-pack-v3-full.log
/tmp/ptgc-source-superblock-train.log
/tmp/ptgc-source-superblock-report.json
/tmp/ptgc-source-pack-v5-{32,64,128,256}k-20.log
/tmp/ptgc-source-pack-v7-32k-full.log
/tmp/ptgc-source-pack-final-strict.log
/tmp/ptgc-source-pack-final-sanitize.log
/tmp/ptgc-source-pack-final2-smoke.log
/tmp/codec50-current-baseline.log
/tmp/codec50-pretrain-duckdb-independent.log
```

Semantic frame sets and table reports:

```text
/tmp/ptgc-semantic-{llvm,rocksdb,opencv,duckdb}.{control,rule,raw,identifier,number,string}.frames
/tmp/ptgc-semantic-*-table*.json
/tmp/ptgc-semantic-*-table-*.bin
```

The frame artifacts are generated data and are intentionally not committed. The harness and this
report are the durable reproduction sources. The final source also passes a two-TU
AddressSanitizer/UndefinedBehaviorSanitizer reconstruction run and a five-TU optimized `-Werror`
smoke run.
