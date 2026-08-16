# M1 direct ordinals: exact cold and cache-half execution

## Result

M1 removes P25's per-Region `dense_id -> key64` association. One generation is latched for the
conversation, and the same typed generation-local `u32` Region ordinal is used by Roots, the C
object store, the F cache, and the F-generated `NEED` response. Root-referenced Blocks are decoded
before F decides which Regions it needs, so the removal does not hide Block-child closure on C.

All 48 complete executions reconstruct every byte of all 9,292 translation units. Direct ordinals
return the cold ledger to P24's size and make both cache-half rows smaller than P25:

| complete 16-corpus execution | P24 | P25 key map | M1 direct ordinals |
|---|---:|---:|---:|
| cold wire | 108,378,454 | 113,834,804 | **108,363,498** |
| cold weighted ratio | 263.47x | 250.84x | **263.51x** |
| cold equal-corpus ratio | 276.38x | 258.23x | **276.46x** |
| cold gap to 400x | 36,991,775 over | 42,448,125 over | **36,976,819 over** |
| bit-0 cache-half wire | — | 81,994,885 | **76,516,628** |
| bit-0 weighted ratio | — | 348.25x | **373.18x** |
| bit-1 cache-half wire | — | 49,359,310 | **43,881,042** |
| bit-1 weighted ratio | — | 578.51x | **650.73x** |
| minimum cold / bit-0 / bit-1 pipeline | 1.07 | 1.70 / 2.00 / 2.28 | **1.68 / 1.87 / 2.34 GB/s** |

The direct cold row is 5,471,306 bytes smaller than P25 and 14,956 bytes smaller than P24. The
small win over P24 is measured rather than assumed: Block ordinals leave the Region `NEED` message,
while generation-lived Block manifests move ahead of it.

This completes the direct-ordinal replay requested in the P25 ruling. It does not close cold-400,
the chronological H200 experiment, or the public-Line cache boundary.

## Executed message order

For each TU, the harness executes this order:

```text
C                                                       F

Root over typed Region/Block ordinals  ----------------> decode Root

required generation-lived Block definitions ----------> install Blocks
                                                         derive every Block child

                                                F checks its own Region cache

                                          <------------- NEED(missing Region ordinals)

requested Region programs + material -----------------> decode/install immutable Regions

Root expansion                                         reconstruct exact complete .ii
```

The exact state rules exercised by this row are:

1. The generation is latched once; it is not repeated per object or per TU.
2. A Region has one generation-local ordinal. No conversation remap or second key namespace exists.
3. F decodes the Root and the required Block definitions before computing `NEED`.
4. Blocks remain for the generation and are not independently evicted in M1.
5. F tests only its own Region store and constructs the actual missing-Region list.
6. C decodes that returned list and sends only the named fills.
7. F expands from its independently built Block and Region stores and reconstructs the complete TU.
8. The harness compares every reconstructed byte with the submitted `.ii` bytes.

The Block manifest is sent only until its successful installation is known. It carries explicit
Block ordinals, accepts no implicit global decode order, and precedes Region `NEED`. This removes
the P25 problem where a missing Block's children were known on C but were not yet discoverable from
F's state.

## Complete category ledger

| wire category | P24 cold | P25 cold | M1 cold | M1 bit 0 | M1 bit 1 |
|---|---:|---:|---:|---:|---:|
| Root | 3,687,803 | 3,687,803 | 3,687,803 | 3,687,803 | 3,687,803 |
| Line/material payload | 80,123,571 | 80,123,571 | 80,123,571 | 59,606,410 | 26,812,600 |
| Region programs | 19,749,171 | 19,749,171 | 19,749,171 | 10,040,415 | 10,198,502 |
| Blocks | 428,568 | 428,568 | 586,843 | 586,843 | 586,843 |
| paths | 828,100 | 828,100 | 828,100 | 818,384 | 818,342 |
| identity/NEED | 3,488,397 | 8,944,747 | 3,317,206 | 1,707,125 | 1,707,344 |
| other framing | 72,844 | 72,844 | 70,804 | 69,648 | 69,608 |
| **total** | **108,378,454** | **113,834,804** | **108,363,498** | **76,516,628** | **43,881,042** |

The direct row spends 158,275 more bytes in the Block category than P24 because the early Block
manifest explicitly names and frames Blocks. It removes 171,191 bytes from the old combined
missing list and 2,040 bytes of later fill framing, a net 14,956-byte win. More importantly, it
removes P25's 5,456,350-byte association plane without moving the cache decision back to C.

## Cache-half result

The two deterministic complements still partition every Region exactly:

| state | preloaded Regions | preloaded raw Region bytes | complete wire | weighted ratio | equal-corpus ratio | half-200 margin |
|---|---:|---:|---:|---:|---:|---:|
| bit 0 | 776,293 | 335,879,233 | 76,516,628 | 373.18x | 443.56x | 66,256,730 under allowance |
| bit 1 | 776,300 | 506,094,161 | 43,881,042 | 650.73x | 532.40x | 98,892,316 under allowance |

Both aggregate cache-half gates pass. Fifteen of sixteen corpora pass 200x on both complements;
Godot's byte-skewed bit-0 complement remains the visible tail at 129.88x. Unlike P25, direct
ordinals lift fmt above 200x on both complements.

## Per-corpus execution

| corpus | direct cold wire / ratio | direct bit 0 wire / ratio | direct bit 1 wire / ratio | cold delta vs P24 | cold pipeline GB/s |
|---|---:|---:|---:|---:|---:|
| llvm | 9,330,294 / 388.01x | 4,915,650 / 736.48x | 5,422,973 / 667.58x | -1,749 | 4.80 |
| rocksdb | 9,825,414 / 316.97x | 6,014,639 / 517.79x | 6,013,349 / 517.90x | -4,534 | 4.05 |
| duckdb | 9,590,734 / 207.05x | 5,907,546 / 336.13x | 4,992,901 / 397.71x | -3,005 | 2.65 |
| abseil | 5,864,879 / 440.08x | 3,540,302 / 729.04x | 3,606,422 / 715.67x | -5,434 | 5.04 |
| opencv | 8,564,687 / 540.71x | 5,044,333 / 918.06x | 4,760,374 / 972.82x | -160 | 5.87 |
| godot | 56,079,250 / 105.79x | 45,679,086 / 129.88x | 13,715,947 / 432.54x | +395 | 1.68 |
| fmt | 1,046,398 / 130.30x | 613,040 / 222.42x | 627,974 / 217.13x | -1,032 | 1.85 |
| spdlog | 539,513 / 182.36x | 301,099 / 326.75x | 293,188 / 335.57x | -4 | 1.96 |
| catch2 | 1,006,074 / 941.53x | 619,809 / 1,528.30x | 611,778 / 1,548.36x | -553 | 6.71 |
| nlohmann-json | 1,168,818 / 251.47x | 687,318 / 427.63x | 684,633 / 429.31x | -226 | 2.93 |
| range-v3 | 874,465 / 722.78x | 511,064 / 1,236.73x | 517,557 / 1,221.22x | +1,074 | 5.32 |
| eigen | 1,378,045 / 2,563.25x | 868,996 / 4,064.77x | 882,463 / 4,002.74x | +413 | 6.81 |
| re2 | 488,243 / 225.77x | 287,285 / 383.70x | 256,325 / 430.05x | +91 | 2.20 |
| leveldb | 685,363 / 209.92x | 398,451 / 361.07x | 407,208 / 353.30x | -526 | 2.12 |
| simdjson | 1,408,733 / 332.48x | 847,835 / 552.44x | 801,883 / 584.10x | +319 | 3.38 |
| cereal | 512,588 / 637.74x | 280,175 / 1,166.77x | 286,067 / 1,142.74x | -25 | 4.91 |

## Validation and scope

- Warning-clean release build with `-O3 -Wall -Wextra -Werror`.
- Cold, bit-0, and bit-1 matrices reconstruct all 9,292 TUs exactly: 48/48 corpus runs.
- ASan+UBSan reconstruct fmt cold and both complements exactly with release-identical wire totals.
- All seven wire categories close every per-corpus total.
- Direct `NEED` closes the complete missing category with zero association bytes.
- The two preload counts sum to every corpus's Region count.
- Root, Line/material, Region, and path bytes are unchanged from P24 cold.
- Root and generation-lived Block bytes are invariant across direct cold and both cache halves.
- All 48 measured release pipelines exceed 1 GB/s.
- Sixteen focused learning/property tests, Python compilation, Ruff, and `git diff --check` pass.

M1 deliberately does not claim the complete product boundary. Public Lines are still rebuilt in
chronological order rather than participating in typed `NEED/FILL`; zstd component state still
persists across TUs; the chronological C50/H200 plus 50%-snapshot experiment is not yet executed;
multi-F, bounded eviction, and the actual two-process socket path remain with the implementer.
Cold-400 remains open by 36,976,819 bytes before the separate blob and alpha-family contenders.

## Reproduction

Build:

```sh
g++ -O3 -march=native -std=c++17 -DICE_LINE_CAP_LOG2=23 \
  -Wall -Wextra -Werror linecache/codec50.cpp -lzstd -o /tmp/codec50-direct
```

Run each manifest:

```sh
/tmp/codec50-direct --manifest MANIFEST --z 3 \
  --mixed-regions --byte-array-lines --direct-ordinals

/tmp/codec50-direct --manifest MANIFEST --z 3 \
  --mixed-regions --byte-array-lines --direct-ordinals --half-cold-bit 0

/tmp/codec50-direct --manifest MANIFEST --z 3 \
  --mixed-regions --byte-array-lines --direct-ordinals --half-cold-bit 1
```

Regenerate the checked summary:

```sh
python3 linecache/summarize_direct_ordinal.py \
  --p24-dir /tmp/issue16-complete-p24-final-z3 \
  --p25-cold-dir /tmp/issue16-p25-final-cold \
  --p25-bit0-dir /tmp/issue16-p25-final-bit0 \
  --p25-bit1-dir /tmp/issue16-p25-final-bit1 \
  --direct-cold-dir /tmp/issue16-direct-final-cold \
  --direct-bit0-dir /tmp/issue16-direct-final-bit0 \
  --direct-bit1-dir /tmp/issue16-direct-final-bit1 \
  --output linecache/ml-artifacts/direct-ordinal-m1-16corpus-summary.json \
  --tsv linecache/ml-artifacts/direct-ordinal-m1-16corpus.tsv
```

Retained complete logs:

- `/tmp/issue16-direct-final-cold`
- `/tmp/issue16-direct-final-bit0`
- `/tmp/issue16-direct-final-bit1`

Every log digest is recorded in the machine summary and TSV.
