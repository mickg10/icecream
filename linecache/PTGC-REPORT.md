# PTGC definition-coding and pretrained-model bakeoff

## Verdict

The independently decoded capability harness now includes the minimal contributing-token-span row,
TU-local parameterized token sub-superblocks, pretrained source-basis superblocks, and an actual-cost
union with a frozen 16 KiB cross-project model package.

On all 689 DuckDB translation units (1,985,715,205 raw bytes), the current best exact definition-plane
row is:

```text
P0 literal definition frames                    11,060,591 bytes
P4 alpha-normalized one-Line templates           9,182,120 bytes
P10 best local frame                              9,154,792 bytes
P11 + contributing token-span union               9,071,583 bytes
P12 + TU-local parameterized spans                9,071,583 bytes
P13 + charged pretrained-superblock union         9,027,584 bytes
```

The best row is 2,033,007 bytes smaller than P0 (1.225x), but remains 8.609 MiB, or 220.0x raw/wire.
It improves the previously published charged row by 77,539 bytes. Pretraining and token spans are
real finishing components here; neither is the missing factor-of-two representation.

The decisive residual remains visible rather than guessed: P9 emits 5,261,865 compressed bytes in
its raw-definition channel. Token spans reduce the achievable per-TU union of that channel by about
62 KiB. The tested TU-local parameterized token windows reduce it by only 381 bytes as a standalone
row and add nothing to P11. The hard channel therefore remains far above the 3.4--3.8 MiB
continuation range.

Do not integrate this definition codec into the live two-process path yet. Preserve P4, the semantic
split, the token-span union, the 16 KiB package, and the small encoder ranker as controls. The next
representation must be stronger than local alpha-normalized token windows; more framing or a larger
generic package does not address the measured residual.

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
as a direct pass/fail of that narrower line. Source-location attribution and the minimal contributing
source representations are reported separately below.

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
| P7t/P9t | Exact contributing source-token spans over all definitions / only P9 raw definitions |
| P9sg | Source bases encoded by TU-local alpha-template grammar |
| P9sm | Source bases encoded by frozen cross-project superblocks plus TU-local additions; package also optional zstd history |
| P9g | TU-local parameterized 4/8/16/32-token sub-superblocks over P9 raw definitions |
| P11 | P10 extended with actual per-TU contributing-token-span candidates |
| P12 | P11 extended with source-basis grammar and parameterized token sub-superblocks |
| P13 | Flat actual per-TU union of P12 and all frozen-package candidates, charging the package once |

The older P7 cross-TU template label is an intermediate historical name. The source-location,
contributing-source, and token-span successors are all implemented later in this report and should
be used for conclusions about that part of BigOracle's ladder.

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
| P0 appearance | 11,060,591 | 10.548 | 179.5x | 1.000x | 7.08 | 18.07 | 144.2x |
| P1 lexicographic | 11,070,283 | 10.557 | 179.4x | 0.999x | 8.13 | 22.96 | 148.2x |
| P4 one-Line templates | 9,182,120 | 8.757 | 216.3x | 1.205x | 0.27 | 7.68 | 182.6x |
| P5 multiline | 10,480,697 | 9.995 | 189.5x | 1.055x | 0.18 | 13.71 | 146.9x |
| P6 token forest | 12,612,618 | 12.028 | 157.4x | 0.877x | 0.28 | 18.13 | 122.0x |
| P7 source-location | 10,444,367 | 9.961 | 190.1x | 1.059x | 0.01 | 14.49 | 152.7x |
| P7t all-definition token spans | 9,586,185 | 9.142 | 207.1x | 1.154x | 0.03 | 17.22 | 177.4x |
| P9 semantic split | 9,316,268 | 8.885 | 213.1x | 1.187x | 0.27 | 7.99 | 182.8x |
| P9t raw-channel token spans | 9,251,614 | 8.823 | 214.6x | 1.196x | 0.05 | 9.56 | 182.8x |
| P9g parameterized token spans | 9,315,887 | 8.884 | 213.2x | 1.187x | 0.03 | 9.59 | 182.8x |
| P8 pretrained package as P4 history | 9,125,550 | 8.703 | 217.6x | 1.212x | 0.26 | 7.72 | 184.3x |
| P10 local actual-cost union | 9,154,792 | 8.731 | 216.9x | 1.208x | 0.01 | 9.18 | 185.9x |
| P11 token-span union | 9,071,583 | 8.651 | 218.9x | 1.219x | 0.00* | 8.67 | 186.9x |
| P12 parameterized-span union | 9,071,583 | 8.651 | 218.9x | 1.219x | 0.00* | 8.86 | 186.9x |
| **P13 charged pretrained-superblock union** | **9,027,584** | **8.609** | **220.0x** | **1.225x** | 0.00* | 7.11 | **187.8x** |

The starred union encoder numbers are the cost of running every research candidate, including 120
token-span configurations per TU, not a production selector.
The existing 54,136-byte, 32-tree encoder ranker remains the production-shaped way to reduce exact
candidate evaluation. P4's current implementation also parses and serializes both normalization
variants; it has not received a throughput optimization pass because its byte result misses the
continuation line.

P13 cumulative raw/wire ratios at 10/25/50/75/100% of chronological raw input are:

```text
101.6x, 235.3x, 192.6x, 193.1x, 220.0x
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
| P7p source superblock over all definitions | 10,989,654 | 180.7x | exact |
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

## Contributing token spans and parameterized sub-superblocks

The successor removes unused source bytes instead of moving whole source lines. For each target
definition C resolves its contributing source location, computes an exact token-aligned COPY/ADD
program, and retains only copied spans. Span contents are deduplicated into a dense TU-local basis.
The complete candidate is encoded as:

```text
basis control
basis bytes
definition program control
exact ADD/literal bytes
```

The harness sweeps whole COPY runs and 2/4/8/16-atom chunks, minimum span sizes 4/4/6/8/12,
residual limits 0/6/12/25/50/100%, and source-order versus lexical-order bases. For every candidate
it compares four split zstd frames with one length-delimited combined zstd frame. A final per-TU
selector compares the best complete span candidate with the original literal frame. F receives no
source file: it reconstructs only from the selected frame bytes.

Full DuckDB results:

| row | charged wire | change versus matching baseline | exact |
|---|---:|---:|---|
| P7t spans over every definition | 9,586,185 | -1,403,469 versus P7p | PASS |
| P9t spans over P9 raw channel | 9,251,614 | -64,654 versus P9 | PASS |
| P11 actual union | 9,071,583 | -83,209 versus P10 | PASS |

P9t's selected raw channel is 4.956 MiB versus 5.018 MiB for P9. Only five TU span frames beat the
literal raw frame, so the gain is concentrated. No one fixed configuration explains it: the best
fixed row is 5.021 MiB and loses after its selector. The accepted capability is the bounded actual
per-TU search, not one universal span encoding.

P9g tests the stronger local grammar hypothesis over the same raw channel. It enumerates
alpha-normalized 4/8/16/32-atom windows, preserves repeated-slot equality, ranks candidate rules by
MDL benefit, uses a shortest-path program per definition, charges only definitions that survive the
program pass, prunes one-use/losing rules to a fixed point, and compares inline versus columnar
instances plus split versus combined frames. Its full ledger is:

```text
candidate shapes             2,054,723
selected / used rules          245,315 / 35,356
rule instances                 387,893
program / literal records      193,566 / 105,093
covered / residual / target       8.26 / 4.09 / 27.41 MiB
ordinary P9 raw channel                         5.018 MiB
parameterized candidate channel                 5.863 MiB
actual selected channel                         5.018 MiB
winning TU frames                                      1
```

P9g is 381 bytes below P9 as a standalone selected row, but it adds zero bytes of benefit to P11;
P12 therefore equals P11 exactly at 9,071,583 bytes. Local window grammar is expressive in raw
coverage and still loses after rule definitions, slot payloads, programs, and outer compression.

## Frozen pretrained-superblock union

The pretrained artifact is a superblock model first, not merely a zstd dictionary. Disjoint LLVM,
RocksDB, and OpenCV training produces frozen parameterized rules and reusable exact slot tokens.
The 16 KiB raw package contains 472 rules and 634 tokens, compresses to about 5.8 KiB, and is charged
once before any DuckDB TU. The previously measured 16/32/64/256-KiB package curve selects 16 KiB
after charge.

The P13-specific 20-TU dominance replay confirms that the stronger union does not move the optimum:

| raw package cap | package wire | P13 charged bytes | delta versus untrained P12 |
|---:|---:|---:|---:|
| **16 KiB** | **5.8 KiB** | **964,432** | **+3,954** |
| 32 KiB | 11.3 KiB | 970,694 | +10,216 |
| 64 KiB | 21.5 KiB | 980,779 | +20,301 |
| 256 KiB | 78.3 KiB | 1,029,979 | +69,501 |

The short slice does not amortize any package, but 16 KiB strictly dominates every larger package.
Only that size received the authoritative full P13 replay, where the package debt is repaid.

The current harness gives this one artifact three independent uses:

1. frozen parameterized rules plus current-TU private additions (`P8c`);
2. optional zstd history over P4's structural streams;
3. frozen parameterized superblocks over the contributing source basis (`P9sm`), again with
   current-TU private additions and optional history.

Each use may lose independently. The receiver first decompresses the charged package, constructs
its own rule/token objects and zstd dictionary, then decodes every selected target frame. The final
P13 selector is flat: it can select any untrained P12 codec or one model-assisted representation
with one explicit selector byte.

Full selector ledger:

```text
candidate                              selected TUs    selected frame wire
untrained P12                                  268             3.249 MiB
frozen + current-TU private superblocks          8             0.253 MiB
package as P4 history                          413             5.101 MiB
pretrained contributing-source basis             0             0.000 MiB
```

The source-basis model wins 15 comparisons against its own P9 source/literal choice and its package
history wins 9,652 evaluated frame alternatives, but it never wins the final P13 selector. Its
complete source candidate is 5.501 MiB versus the ordinary 5.018 MiB raw channel. This is another
clean separation between a useful model and a losing source representation.

P13 finishes at 9,027,584 bytes, 8.609 MiB, or 220.0x. It improves P12 by 43,999 bytes after the
package charge and improves the earlier published 9,105,123-byte pretrained result by 77,539 bytes.
The package helps on 421 TUs, but the gain remains a finishing effect rather than a cold-400 path.

## PTGC ruling and remaining representation work

The minimal contributing-token-span row is complete and positive, but small. The tested local
parameterized token-window grammar and pretrained source-basis grammar do not close the residual.
No new definition representation in this branch should be integrated into the transport yet.

Keep these capability components:

- P9 homogeneous semantic streams and raw fallback;
- per-TU actual candidate selection;
- P9t contributing spans as a sparse candidate;
- the 16 KiB frozen parameterized-superblock package;
- the 32-tree encoder-side ranker from the separate ML bakeoff.

Do not carry forward whole source bases, the P9g local window grammar, or the P9sm source-basis model
as product requirements. They remain useful measured controls.

The next research representation must reduce the first-use content itself: a hierarchical token/slot
language with cheap new-identifier spelling and reusable statement/multi-Line rules, or an exact
source-token replay that avoids paying nearly one basis per emitted definition. It should begin from
P11/P13's actual frame accounting and must show a material raw-channel reduction before another
selector/model family is added. The existing ML bakeoff already covers FTRL, 32/256-tree GBDT,
CNN/MLP, GRU/TCN teachers, dual retrieval, distillation controls, fixed integer tables, trained zstd
dictionaries, and the external teacher ceiling; repeating those families over this losing
representation is not justified.

## P14/P16 follow-up: frozen parameter windows and portable bootstrap input

Two additional exact controls test whether P9g failed only because its rules were TU-local or its
program streams were separately compressed.

- P14 mines a frozen 16 KiB package of alpha-normalized 4/8/16/32-atom windows from disjoint
  projects. It uses dense TU-local rule IDs, a typed TU-local value lexicon, inline/columnar instance
  alternatives, optional model-derived zstd history, and actual raw fallback.
- P16 uses the same exact program but folds its control into P9 control, appends identifier/number/
  string values to P9's existing typed streams, replaces only P9's raw-definition payload with the
  residual, and compresses each homogeneous stream once. Its decoder splits those streams,
  reconstructs the original raw definitions, and then invokes the ordinary P9 decoder.

The first training control uses the earlier disjoint LLVM + RocksDB + OpenCV expanded streams. On
the same first 20 held-out DuckDB TUs:

| row | charged bytes | result |
|---|---:|---|
| P9 semantic baseline | 964,895 | exact |
| P12 best untrained union | 960,478 | exact |
| P14 separate frozen-window program | 969,505 | exact |
| P16 fused frozen-window program | 969,505 | exact |

The frozen package is 14.4 KiB raw / 4.5 KiB wire. It covers 0.63 MiB of a 1.78 MiB target, but its
raw-channel candidate is 0.474 MiB versus 0.399 MiB for grouped literals. P14 wins zero TU frames.
P16's complete fused candidates total 0.989 MiB versus P9's 0.920 MiB and also win zero of 20 TUs.
Fusing streams therefore removes the separate-frame hypothesis: the small match program itself is
too expensive.

Expanded output is also the wrong input for a portable bootstrap package. Compiler headers, macro
expansion, generated paths, and toolchain versions vary between installations even when project
source is identical. The harness now accepts `--param-pretrain-source-root` and recursively mines
only raw C/C++ source files, with build output and repository metadata directories excluded. The
held-out `.ii` manifest is not opened until this package is frozen.

The full raw-source control trains on LLVM + RocksDB + OpenCV source trees:

```text
raw source bytes                 0.84 GiB
source lines                   20,265,199
per-project distinct lines      9,231,937
sampled lines                     288,774
candidate windows              10,585,773
candidate normalized shapes     1,207,059
selected rules                        977
package raw/wire               14.0/5.0 KiB
```

On the same 20 DuckDB TUs it raises covered bytes to 0.70 MiB, but the separate candidate is 0.479
MiB versus the same 0.399 MiB literal channel. P14 and P16 finish at 970,070 charged bytes, P16's
complete candidates total 0.993 MiB, and both again win zero of 20 TUs. Every decoder reconstructs
all 85,493 held-out definitions exactly.

The result supports a portable bootstrap model but rejects this vocabulary. More training finds
more small matches; it does not make thousands of small opcodes and slot values cheaper. A next
portable model must learn coarser statement/multi-Line/source-token superblocks and measure package
debt as TUs observed -> charged ratio. Installation-specific expanded-output superblocks belong in
a separate chronological online layer and must be encoded before the current TU is learned.

The broader follow-up is a common raw-source model trained on the 14 available projects other than
LLVM and DuckDB, followed by complete LLVM and DuckDB holdouts on quietbox2. The implementer was
given the full 16-corpus map, package/support sweeps, retained-artifact contract, and complete exact
measurement gates in issue #16 comment 5302494188.

### Frozen package artifact boundary

P14/P16 can now cross a real process boundary without any trainer state:

- `--param-model-out FILE` writes the exact compressed decoder-visible package after training;
- the writer immediately reloads that file, reconstructs its encoder lookup table, and verifies the
  original frame and raw package byte-for-byte;
- `--param-model-in FILE` loads that same frame in a fresh process, rebuilds every `ParsedLine` key
  from decoder-visible literals, slot types, and equality links, and rejects a non-canonical package;
- the loaded frame, rather than a recompressed equivalent, is charged to P14/P16.

A two-TU DuckDB smoke trained a 629-rule raw-source package, wrote a 3,721-byte frame, and loaded it
in a fresh process. All 22 charged row totals, model ID `a60c8c6a0dcaa489`, rule count, and fused P16
total matched the training process. A separate LLVM invocation loaded the identical frame. A one-TU
ASan/UBSan loaded-package replay completed without diagnostics. This makes it possible to train the
common 14-project package once and prove that both complete holdouts consume literally identical
bytes.

### Common 14-project portable small-window rejection control

The first common-package control trained once on raw source from RocksDB, Abseil, OpenCV, Godot,
fmt, spdlog, Catch2, nlohmann/json, range-v3, Eigen, RE2, LevelDB, simdjson, and cereal. LLVM and
DuckDB were excluded from training. The scan produced:

```text
selected raw source                  0.50 GiB
source lines                       10,984,935
per-project distinct lines          5,661,219
sampled lines                         176,836
candidate windows                  15,708,459
candidate normalized shapes          716,426
selected rules                            812
raw package / exact frame           14.3 KiB / 4,412 bytes
training scan / wall time           47.48 s / 52.65 s
peak RSS                         1,152,816 KiB
```

The resulting frame has SHA-256
`e2e5d3789f4950e32bfede9d41346485590ef35725533aa1a8e020ab00abcf34` and internal model ID
`7537901301232b73`. Every row below loaded that identical file in a fresh process:

| holdout | zstd | P9 | P12 local | charged P14/P16 | candidate raw vs literal | frame wins |
|---|---:|---:|---:|---:|---:|---:|
| DuckDB, first 20 | 1 | 1,022,131 | 1,018,721 | 1,026,567 | 0.488 vs 0.419 MiB | 0/20 |
| DuckDB, first 20 | 3 | 964,895 | 960,478 | 969,331 | 0.471 vs 0.399 MiB | 0/20 |
| LLVM, first 20 | 1 | 987,572 | 985,129 | 992,008 | 0.556 vs 0.468 MiB | 0/20 |
| LLVM, first 20 | 3 | 939,453 | 935,323 | 943,889 | 0.533 vs 0.443 MiB | 0/20 |

In all four runs P14/P16 are exactly P9 plus the 4,416-byte framed-package charge and one selector
byte per TU. The broader training set increases rule coverage, but the small parameter windows still
create more control/value bytes than grouped literals. This row is therefore stopped at the 20-TU
gate rather than spending four complete-holdout runs on a candidate with no selected frame. The next
portable vocabulary must use coarser statement, multi-Line, or source-token blocks; the empty-start
local learner must use the same units so the bootstrap and online curves are directly comparable.

`--param-coarse-lines` adds the cheapest larger-unit control without changing the default: 64- and
128-atom windows plus one whole-Line normalized rule for nonstandard widths. The common 14-project
scan then considered 16,633,882 windows and 1,011,760 shapes, selected 779 rules, and emitted a
4,292-byte frame. Training took 59.22 seconds wall and 2,020,056 KiB peak RSS. On zstd-3:

| holdout | P9 | charged coarse P14/P16 | candidate raw vs literal | frame wins |
|---|---:|---:|---:|---:|
| DuckDB, first 20 | 964,895 | 969,211 | 0.470 vs 0.399 MiB | 0/20 |
| LLVM, first 20 | 939,453 | 943,769 | 0.532 vs 0.443 MiB | 0/20 |

The larger spans save only about 1 KiB of candidate stream and still select no frame. Whole-Line
normalization is therefore also too fine-grained. The flag remains as a rerunnable negative control;
the unflagged trainer was regression-checked to reproduce the prior smoke artifact byte-for-byte
(`b18c8136...`). The next row must cross Line boundaries or learn source-level statement blocks.

## Reproduction and retained artifacts

Environment for the authoritative local run:

```text
code commit: ce530ed
compiler:    g++ 11.4.0
libzstd:     1.4.8
trainer:     python-zstandard 0.25.0
machine:     Intel Xeon Gold 6136, Linux 5.15 x86-64
```

Token-span and pretrained-superblock reproduction:

```sh
g++ -O3 -DNDEBUG -march=native -std=c++17 -pthread \
  -Wall -Wextra -Wpedantic -Werror linecache/ptgc_bench.cpp -lzstd \
  -o /tmp/ptgc-pretrained-superblocks

/tmp/ptgc-pretrained-superblocks \
  --manifest /tanksmall/scratch/ictmp/corpus3/manifest.txt \
  --pretrain-manifest /tanksmall/scratch/ictmp/corpus/manifest.txt \
  --pretrain-manifest /tanksmall/scratch/ictmp/corpus2/manifest.txt \
  --pretrain-manifest /tanksmall/scratch/ictmp/corpus5/manifest.txt \
  --model-kib 16 --model-min-corpora 2 --source-k 4 --z 3
```

Portable raw-source parameter-window control:

```sh
g++ -O3 -DNDEBUG -march=native -std=c++17 \
  -Wall -Wextra -Wpedantic -Werror linecache/ptgc_bench.cpp -lzstd \
  -o /tmp/ptgc-p16-source

/tmp/ptgc-p16-source \
  --manifest /tanksmall/scratch/ictmp/corpus3/manifest.txt --max-files 20 \
  --param-pretrain-source-root /tanksmall/scratch/ictmp/corpus/llvm-project \
  --param-pretrain-source-root /tanksmall/scratch/ictmp/build2/rocksdb \
  --param-pretrain-source-root /tanksmall/scratch/ictmp/build2/opencv \
  --param-model-kib 16 --model-min-corpora 2 --source-k 4 --z 3
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
/tmp/ptgc-source-combined-20tu.log
/tmp/ptgc-token-span-strict-2tu.log
/tmp/ptgc-token-span-sanitize-2tu.log
/tmp/ptgc-pretrained-superblocks-v3-smoke.log
/tmp/ptgc-pretrained-superblocks-v3-sanitize.log
/tmp/ptgc-pretrained-superblocks-v3-{32,64,256}k-20tu.log
/tmp/ptgc-pretrained-superblocks-v3-full.log
/tmp/ptgc-p14-fulltrain-20tu.log
/tmp/ptgc-p14-{64,256}k-20tu.log
/tmp/ptgc-p16-smoke.log
/tmp/ptgc-p16-fulltrain-20tu.log
/tmp/ptgc-p16-source-smoke.log
/tmp/ptgc-p16-source-fulltrain-20tu.log
/tmp/ptgc-p16-source-sanitize.log
/tmp/ptgc-param-build.log
/tmp/ptgc-param-load.log
/tmp/ptgc-param-load-llvm.log
/tmp/ptgc-param-io-sanitize.log
/tmp/ptgc-14src-min2-16k-train.log
/tmp/ptgc-14src-{duckdb,llvm}-z{1,3}-20tu.log
/tmp/ptgc-14src-coarse-min2-16k-train.log
/tmp/ptgc-14src-coarse-{duckdb,llvm}-z3-20tu.log
/tmp/ptgc-param-default-regression.log
/tmp/ptgc-coarse-load-smoke.log
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
report are the durable reproduction sources. The final source passes strict optimized `-Werror`
builds, a two-TU ASan/UBSan reconstruction with a separately rebuilt receiver package, a 20-TU
full-training replay, and the complete 689-TU independently decoded replay. The authoritative full
run took 734.27 seconds and peaked at 4,598,760 KiB RSS; that is exhaustive bakeoff cost, not a
production throughput claim. The retained full-log SHA-256 is
`f9065ea4b07a1f5c8899b36846db478e59a933d8a0cce5516a5ba6ea13b82fe0`.

P14/P16 follow-up log SHA-256 values:

```text
af3a6a3ecf02da10b48b11bafc69674b4f4da9f52e70405bf07ff7ce1dc9aad4  /tmp/ptgc-p16-smoke.log
95a5b23a39a361f614cd3269ae173eb5438effc13629023cb598ecffba3dd9a0  /tmp/ptgc-p16-fulltrain-20tu.log
fa3b78c2b107ae040fc345beeb8760ce310d532851cdda9176f579a5602ba94d  /tmp/ptgc-p16-source-smoke.log
85446ea59db8aa1a6453c9fbf8c1bbc622cde78cce492544334072ca9fc533c5  /tmp/ptgc-p16-source-fulltrain-20tu.log
e3eb33e474310e75875bb04579bea860b432280558a1920a633ee29349e7985c  /tmp/ptgc-p16-source-sanitize.log
b18c8136a5acd36702a6e3d286ef302d93f0cb29da0d5796d452c134d61e72e4  /tmp/ptgc-param-smoke.frame
00da86cf6d31d04cc4cc6c49c975a4a433a42f21a3324ee2bffd95b53fae605c  /tmp/ptgc-param-build.log
23189c1b0baf0298fb9385ed61c6df9fa526075315e857d26f2b0986d8a8a24b  /tmp/ptgc-param-load.log
a912a8c024eb8743abfc5cc44d87a0f49687bd040cadede22745ab714323513e  /tmp/ptgc-param-load-llvm.log
e3865367f9a2fe68dad00345303288c3872df07c80891f7e5d26238eba501fb8  /tmp/ptgc-param-io-sanitize.log
263a2de297783c506293fe90a1745e8448bdd4984a9f47e2d5fc7ba0d33936ae  /tmp/ptgc-14src-min2-16k-train.log
84f26ad0af3c6b7b1d88ee8d1a78b0b0971b3d2ac6ee9ec47a13201d8fd46f15  /tmp/ptgc-14src-duckdb-z1-20tu.log
89ddff291da93d6ad45e7d7374e44f0a49c960c3e2512c6ce66d75a86dac6635  /tmp/ptgc-14src-duckdb-z3-20tu.log
9e525cedceacd35d52a60f50838ac44ff0f2b473d2a0ccd5d960dac72c3b063f  /tmp/ptgc-14src-llvm-z1-20tu.log
ae164d8e07ed79022624f4bff834e6c6e01b87f33b2741f24f799a0b9505d7df  /tmp/ptgc-14src-llvm-z3-20tu.log
b4e31c8d070a63f46a8ef5d67f371cd573bb1eeb76530b14905393a7468962bf  /tmp/ptgc-14src-coarse-min2-16k.frame
86017d03ba41f3edd5f7abd10bd1b39c24f5671bfda5b41cbf534a031a627187  /tmp/ptgc-14src-coarse-min2-16k-train.log
88afbc89a9ed7617cd9e78a841b8e601e0dc0325f41ec6558379470e8cfd367c  /tmp/ptgc-14src-coarse-duckdb-z3-20tu.log
4331f011df90c278cb1dc983f40f7422e3030412377a4adaa0694efbd82a04ea  /tmp/ptgc-14src-coarse-llvm-z3-20tu.log
130fb08edaecca56294f4abd72f03dabb1bf57fb2adc581a3b6068aef041c06d  /tmp/ptgc-param-default-regression.log
a1a71d373d530f9562df32217b2d0804cbd9be1eb942aa86f0858db52d7aaf9c  /tmp/ptgc-coarse-load-smoke.log
```
