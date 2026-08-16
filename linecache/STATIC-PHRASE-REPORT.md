# Exact static and online byte-phrase capability report

Issue: `mickg10/icecream#16`

## Outcome

This experiment closes the flat byte-phrase branch over the hard first-use Line channel. Static
raw-source COPY bases, static raw-source BPE phrases, and a mirrored causal online pair-promotion
learner all reconstruct every tested byte, but none materially improves the actual zstd-3 frame.

The negative result is not caused by low phrase coverage. The largest static package covers
97--98% of the target bytes. It loses because those bytes require hundreds of thousands of phrase
references, whose control entropy exceeds the saving from replacing bytes that zstd already codes
well. A dense per-TU palette reduces that control stream only slightly and does not change selected
wire.

Do not port this phrase family into the product codec. Retain the exact package, trie, program,
fallback, and mirrored-state implementations as reproducible controls. The next Line-plane row
should target a representation-specific opportunity rather than enlarge the vocabulary.

## Experimental boundary

The target is the chronological P9 raw-definition stream exported by
`SemanticFrameWriter`. P9's existing control stream retains exact definition lengths and IDs. The
candidate codec reconstructs the complete raw payload before the ordinary P9 decoder installs any
Line.

The two held-out controls are the first 20 TUs of DuckDB and LLVM:

| target | raw `.ii` input | raw-definition bytes | zstd-3 literal frame |
|---|---:|---:|---:|
| DuckDB | 71,456,710 B | 1,868,873 B | 414,149 B |
| LLVM | 67,762,546 B | 2,089,612 B | 459,861 B |

Static training reads raw source from the other 14 projects. It does not read either held-out
expanded stream until the model is frozen. The online row starts empty, encodes TU `t` using state
through `t-1`, independently decodes it, and only then lets both peers learn from the exact decoded
payload.

Every row compares actual combined and split zstd-3 frames. Static rows also compare plain frames
with frames using the package as raw zstd history. A one-byte per-TU selector retains the literal
frame whenever a program loses.

## A. Static contiguous COPY basis

A zstd dictionary trainer selects one immutable raw-source byte basis. The target program is:

```text
ADD(exact residual bytes)
COPY_STATIC(basis_offset, length)
```

A four-byte seed index proposes at most 32 basis offsets, exact byte comparison extends each match,
and the encoder chooses COPY only when its uncompressed record is shorter than the bytes replaced.
The receiver rebuilds the charged basis package and checks every offset and length.

The useful extremes show why an offset basis is not the answer:

| model | target | minimum match | coverage | COPY ops | candidate / literal | selected before package |
|---:|---|---:|---:|---:|---:|---:|
| 16 KiB | DuckDB | 4 | 1,282,610 B | 209,401 | 574,870 / 414,149 | 414,169 B |
| 16 KiB | LLVM | 4 | 1,388,465 B | 220,254 | 638,841 / 459,861 | 459,881 B |
| 64 KiB | DuckDB | 24 | 4,895 B | 187 | 413,405 / 414,149 | 412,986 B |
| 64 KiB | LLVM | 24 | 8,335 B | 314 | 458,939 / 459,861 | 458,330 B |

The 16 and 64 KiB packages cost 8,452 and 31,947 compressed bytes respectively. Short matches give
useful raw coverage but far too many `(offset,length)` records. Long matches produce a tiny
uncharged finishing gain that cannot repay the package.

## B. Static raw-source BPE phrases

Byte-level BPE was trained from 20,344,602 raw-source bytes and retained only phrases with positive
training-side MDL value. The largest package contains 56,771 exact phrases:

| property | value |
|---|---:|
| raw package | 620,339 B |
| compressed package | 302,948 B |
| package SHA-256 | `b7f93264fc01e50438bd89f0f852f5fff91601a70b54affe4a24dee79a5f626d` |

The target uses a trie and a shortest-path parse over:

```text
ADD(exact residual bytes)
STATIC_PHRASE(id)
```

It tests both byte-cost and fewest-operation parses, global phrase IDs and a TU-local dense palette,
and split/combined plain/history frames. The largest measured global-ID row is:

| target | covered / target | phrase / ADD ops | candidate / literal | selected | wins |
|---|---:|---:|---:|---:|---:|
| DuckDB | 1,818,962 / 1,868,873 B | 340,000 / 41,485 | 432,762 / 414,149 | 413,273 B | 10/20 |
| LLVM | 2,035,090 / 2,089,612 B | 385,704 / 42,327 | 482,136 / 459,861 | 459,384 B | 5/20 |

Even before charging the 302,948-byte package, the complete candidate is 4.5% larger on DuckDB and
4.8% larger on LLVM. The phrase table therefore solves byte coverage but not description length.

## C. Empty-start mirrored online phrases

The online learner uses one immutable phrase namespace. After each TU, both peers repeatedly count
adjacent tokens over a bounded exact history and promote the same deterministic highest-frequency
pairs until the 256 KiB phrase-byte budget is full. The encoder and receiver hold independent
objects; phrase lists and SHA-256 identities are compared after every TU.

Configuration:

```text
phrase-byte budget       256 KiB
history                   64 MiB
promotion rounds          6 per TU
promotions                512 per round
minimum pair count        4
maximum phrase length     256 B
```

| target | final phrases | covered / target | phrase / ADD ops | all-program / literal | selected | wins |
|---|---:|---:|---:|---:|---:|---:|
| DuckDB | 22,295 | 1,458,245 / 1,868,873 B | 371,986 / 55,659 | 471,626 / 414,149 | 412,022 B | 11/20 |
| LLVM | 18,548 | 1,341,927 / 2,089,612 B | 368,722 / 142,681 | 557,511 / 459,861 | 455,614 B | 12/20 |

The actual selector saves 2,127 B and 4,247 B, respectively, because a few late TU frames benefit.
The all-program representation is still 13.9% and 21.2% larger. More pair promotion is therefore
not a path to the required factor-sized Line reduction.

The final dense-palette replay lowers all-program wire from 471,626 to 466,092 bytes on DuckDB and
from 557,511 to 552,111 bytes on LLVM. Selected wire remains exactly 412,022 and 455,614 bytes. Only
one frame in each corpus selects a dense palette; most frames still prefer byte-cost global IDs.
Paying each used global ID once and then using a local rank is not enough.

## Why the three rows converge

The three implementations span the relevant flat-phrase trade-off:

1. arbitrary COPY offsets maximize substring reuse but pay an offset and length per use;
2. static phrases replace offsets with stable IDs but pay a package and a phrase ID per use;
3. online phrases remove startup package debt but must first learn and then repeatedly reference a
   large project-local vocabulary.

All three lose for the same measured reason: the reusable unit is too small. Outer zstd already
represents recurring C/C++ byte fragments compactly. Exposing each fragment as an explicit object
turns byte redundancy into a high-cardinality invocation stream.

This also explains why larger phrase models improve coverage without improving wire. Coverage is
not the missing variable. A successor must either make each invocation represent substantially
more output or exploit a deterministic transformation whose parameters are much cheaper than its
rendered text.

## New aggregate priority

The balanced 16-corpus Line ledger contains 544,287,053 first-use Line bytes and 103,210,181 bytes of
stateful split-front zstd-3 wire. Godot alone contributes 272,752,620 Line bytes and 67,067,667 wire
bytes: about 65% of the entire Line wire.

A direct trace audit identifies the cause. Six generated Godot paths contribute roughly 174 MiB of
canonical decimal byte-array rows. The largest is
`editor/translations/doc_translations.gen.cpp`, with about 102 MiB in one TU. Across Godot,
1,465,845 strict rows contain 38,253,925 exact values in `[0,255]` rendered as decimal text.

Those rows can be represented exactly as:

```text
BYTE_ARRAY(style_id, values_per_line, raw_u8_values)
```

and rendered back to identical decimal spelling, spacing, commas, and newlines. A whole-stream
zstd-3 probe reduces their wire from 54,848,162 to 36,934,662 bytes, a measured 17,913,500-byte
saving. Unlike generic phrases, each control item governs an entire generated row and the value
stream recovers the generator's binary substrate.

That generated-array codec is the next capability row. It must retain literal/front fallback, run
across all corpora, reconstruct every Line independently, and then be combined with the structural
ledger. The underlying already-compressed asset values remain a real cold-transfer floor and must
be reported rather than hidden.

## Reproduction

Static COPY and BPE rows:

```sh
python3 linecache/static_copy_basis.py \
  --source-root ROOT ... \
  --target duckdb /tmp/ptgc-semantic-duckdb.raw.frames \
  --target llvm /tmp/ptgc-semantic-llvm.raw.frames \
  --dict-kib 16 64 256 --minimum-match 4 6 8 12 16 24 \
  --phrase-kib 256 512 1024 --bpe-vocabulary 65536 \
  --max-tus 20 --z 3 --json /tmp/ptgc-static-phrase.json
```

The recorded common-14 roots are:

```text
/tanksmall/scratch/ictmp/build2/rocksdb
/tanksmall/scratch/ictmp/build2/abseil-cpp
/tanksmall/scratch/ictmp/build2/opencv
/tanksmall/scratch/ictmp/build2/godot
/tanksmall/scratch/ictmp/src2/{fmt,spdlog,catch2,json,range-v3,eigen,re2,leveldb,simdjson,cereal}
```

Empty-start online row:

```sh
python3 linecache/static_copy_basis.py \
  --target duckdb /tmp/ptgc-semantic-duckdb.raw.frames \
  --target llvm /tmp/ptgc-semantic-llvm.raw.frames \
  --skip-copy-basis --online-phrase-kib 256 \
  --online-promotions 512 --online-minimum-count 4 \
  --online-rounds 6 --online-maximum-length 256 \
  --online-history-mib 64 --max-tus 20 --z 3 \
  --json /tmp/ptgc-online-phrase-256k-r6.json
```

The script checks every package decode, program decode, compressed-frame decode, and mirrored online
state transition before reporting a row.

Retained-result SHA-256 values at report time:

```text
278d4546d55cb846f4ea578c8f40f606ce70ef94dc702eb26f6e201af78a1a24  ptgc-static-copy-common14.json
b890f4d515dc8ee378075665a2cd40842a88b4f4fe0576a5828f495f1f029f3d  ptgc-bpe65-common14.json
5c8de99d6d009b0bc109f140dadf66e408a59ebe51fc4f68cca646c7d33d4864  ptgc-online-phrase-256k-r6-dense.json
```

The compact committed TSV contains the headline rows without depending on `/tmp` retention.
