# P29: selective embedded-blob compression with bounded workers

## Outcome

P29 keeps P28's exact structural and material formats and changes one selected
material lane: complete recovered embedded-object batches use zstd-9/LDM with
four workers, 5 MiB jobs, and overlap-log 3.  Control, literal, and ordinary
byte-array lanes remain zstd-3.  All 48 complete cold/bit-0/bit-1 executions
reconstruct all 9,292 TUs exactly, and no corpus/state pair regresses.

| receiver state | P28 wire | P29 wire | saving | P29 ratio | target result |
|---|---:|---:|---:|---:|---:|
| cold | 93,968,728 | **91,864,787** | **2,103,941** | **310.834x** | open by 20,478,108 |
| cache bit 0 | 62,140,320 | **60,040,447** | **2,099,873** | **475.591x** | passes by 82,732,911 |
| cache bit 1 | 43,852,134 | **43,848,226** | **3,908** | **651.216x** | passes by 98,925,132 |

The fixed suite contains 28,554,671,510 raw bytes.  Cold-400 permits
71,386,678.775 bytes, so this is a measured continuation rather than closure:
**20,478,108.225 more bytes must still be removed**.  Both deterministic
half-cache rows remain well above 200x.  Chronological H200, edited-input
learning curves, and reordered schedules remain separate open gates.

## Why this lane is selective

Only Godot contains selected embedded-object batches in this fixed suite.  Its
cold path recovers 112 members containing 124,407,478 inflated bytes.  P27's
MO factor converts 107 members to exact catalog definitions/translations; P29
then compresses the selected factor or inflated batch more deeply.  This is
only 2.10% of Godot's 5,932,762,185 raw input bytes, so level 9 is not applied
to the entire `.ii` stream.

P29 changes only the `line_def` ledger:

| complete cold category | P28 | P29 | change |
|---|---:|---:|---:|
| Root | 3,681,710 | 3,681,710 | 0 |
| Line/material definitions | 65,743,189 | **63,639,248** | **-2,103,941** |
| Region definitions/control | 19,746,257 | 19,746,257 | 0 |
| Block definitions | 581,462 | 581,462 | 0 |
| Paths | 828,100 | 828,100 | 0 |
| missing replies | 3,317,206 | 3,317,206 | 0 |
| framing | 70,804 | 70,804 | 0 |
| **total** | **93,968,728** | **91,864,787** | **-2,103,941** |

All fifteen zero-blob corpora are byte-identical to P28 in all three receiver
states.  Godot is the only changed row:

| Godot state | P28 | P29 | saving | P29 ratio | selected inflated / wire |
|---|---:|---:|---:|---:|---:|
| cold | 41,693,465 | **39,589,524** | 2,103,941 | 149.857x | 124,407,478 / 14,617,593 |
| cache bit 0 | 31,311,763 | **29,211,890** | 2,099,873 | 203.094x | 123,613,862 / 14,550,829 |
| cache bit 1 | 13,696,024 | **13,692,116** | 3,908 | 433.298x | 793,616 / 66,564 |

## Exact encoder/decoder contract

The chosen operation remains an ordinary standard zstd frame:

1. C identifies complete zlib members in existing byte-array material and
   inflates them exactly.
2. The existing P27 decision optionally factors canonical catalogs into
   original definitions, translations, control, and ordinary members.
3. C selects that exact factored stream or the complete inflated batch using
   the existing measured policy.  Only this selected payload is encoded at
   zstd-9/LDM, window-log 27.
4. The C encoder uses four zstd workers, 5 MiB jobs, and overlap-log 3.  The
   resulting bytes are a normal zstd frame; these worker settings do not enter
   the wire contract.
5. F uses an ordinary single-threaded zstd decoder, then the unchanged P27
   factor decoder and canonical-member reconstruction path.
6. The existing lazy exact-member reply remains available when a regenerated
   member differs.  Every fallback request and reply remains charged.

The implementation now checks every zstd context parameter.  A build whose
zstd library lacks worker support fails with a named capability error instead
of silently running the serial encoder under a multithreaded label.

## Size/speed frontier

The binding speed screen is the full 2,207-TU Godot corpus on
`tt-quietbox2`, pinned to CPUs 0-15.  Four zstd workers and 5 MiB jobs are the
best measured point with repeatable margin above 1 GB/s:

| selected blob setting | Godot wire | C encode | F decode | decision |
|---|---:|---:|---:|---|
| zstd-6, serial | 40,663,442 | 1.010 | 1.193 | smaller gain; passes |
| zstd-9, serial | **39,219,667** | 0.850 | 1.190 | too slow |
| zstd-9, 4 workers, 4 MiB jobs | 39,670,985 | 1.043 | 1.186 | passes; larger |
| **zstd-9, 4 workers, 5 MiB jobs** | **39,589,524** | **1.018** | **1.180** | selected |
| zstd-9, 4 workers, 6 MiB jobs | 39,523,695 | 1.003 | 1.176 | insufficient margin |
| zstd-9, 4 workers, 7 MiB jobs | 39,523,143 | 1.002 | 1.173 | insufficient margin |
| zstd-9, 4 workers, 8 MiB jobs | 39,460,817 | 0.991 | 1.183 | too slow |
| zstd-9, 4 workers, 16 MiB jobs | 39,351,585 | 0.986 | 1.175 | too slow |

Three fresh pinned repetitions of the selected point produce identical wire
and exact output:

| repetition | C encode | F decode | pipeline minimum | log SHA-256 prefix |
|---|---:|---:|---:|---|
| 1 | 1.028 | 1.185 | **1.028** | `6db989d59a9a` |
| 2 | 1.027 | 1.172 | **1.027** | `5ef1d5f1d4f6` |
| 3 | 1.025 | 1.172 | **1.025** | `5844c49123d2` |

The local 48-run matrix is evidence for bytes and exact reconstruction, not
the binding speed host.  Its cold Godot run reaches 0.853 GB/s.  The three
pinned Zen 4 repetitions above are the designated encode/decode subphase
evidence.  A complete preprocessor-pipe-to-wire measurement is still open.

### Compute shape

On the selected full Godot run, C encode takes about 5.77-5.79 seconds and F
decode/reconstruction about 5.00-5.06 seconds.  The earlier measured C
read/parse/intern stage takes about 6.5 seconds.  Serial C ingestion plus encode
is therefore about 12.3 seconds, or 0.48 GB/s; independent streaming lanes have
an observed stage ceiling near 0.91 GB/s.

Canonical member regeneration uses up to eight threads, followed by the
four-worker zstd frame phase; these are separate phases, not an 8-by-4 nested
pool.  A product implementation should give the long-lived C cache/service one
bounded host-level worker pool rather than create a pool for every compiler
fork.  The current research harness's approximately 7.65 GiB peak RSS includes
the entire 5.93 GB corpus and all verification stores and is not a per-TU
product-state estimate.

## Rejected neighboring points

Applying zstd-6 to every lane yields 88,334,723 bytes over all 16 cold corpora,
323.255x, and would save 5,634,005 bytes from P28.  It is rejected because the
designated Godot C encoder measures 0.873 GB/s.  The following selective forms
also fail the complete size/speed trade:

| candidate | Godot wire | C encode | result |
|---|---:|---:|---|
| literal lane at zstd-6 | 40,220,363 | 0.995 | too slow |
| literal-6 plus blob-6 | 39,190,340 | 0.759 | too slow |
| all material lanes at zstd-6 | 39,057,138 | 0.820 | too slow |
| literal-4 plus selected P29 blob lane | 39,594,483 | 1.015 | 4,959 bytes larger than P29 |

### Causal whole-Region delta screen

An independent exact capability benchmark tests whether first-seen Regions can
be represented as prefix/middle/suffix or bounded sparse-XOR changes from
Regions learned only after earlier completed TUs.  Godot supplies 347,194,250
raw first-definition Region bytes:

| exact causal representation | selected uncompressed records | zstd-3 wire |
|---|---:|---:|
| prefix/middle/suffix | 315,193,795 | **74,190,083** |
| plus sparse same-offset XOR | 313,173,003 | **127,901,768** |
| current P29 Region control plus material | — | **38,702,941** |

Although 85,116 Regions find a nominal prefix/suffix patch, the existing typed
material streams plus zstd preserve substantially more useful neighborhood.
Sparse XOR destroys those neighborhoods.  This whole-Region base family is
closed; the next candidate must operate inside typed material rather than wrap
the completed Region bytes.

### MO translation-base screen

The standalone exact MO benchmark now retains two additional causal layouts
and can dump the exact factored bytes for follow-on entropy screens:

| whole-generation zstd-3 factor layout | wire | change from original-relative patch |
|---|---:|---:|
| translation patch from its original | **16,198,300** | — |
| patch from the previous translation of the same original | 16,241,448 | +43,148 |
| cheapest original or any earlier translation | **16,190,221** | -8,079 |

Searching all earlier translations removes only 8,079 compressed bytes in this
favorable whole-generation form and roughly doubles decode time in the
standalone run.  It is retained as a reproducible ceiling, not added to the
complete codec.

## Complete per-corpus execution

| corpus | cold P28 | cold P29 | cold save | cold ratio | bit-0 P29 / ratio | bit-1 P29 / ratio |
|---|---:|---:|---:|---:|---:|---:|
| LLVM | 9,328,055 | 9,328,055 | 0 | 388.106x | 4,913,411 / 736.814x | 5,420,734 / 667.856x |
| RocksDB | 9,823,481 | 9,823,481 | 0 | 317.028x | 6,012,706 / 517.957x | 6,011,416 / 518.068x |
| DuckDB | 9,589,726 | 9,589,726 | 0 | 207.067x | 5,906,538 / 336.189x | 4,991,893 / 397.788x |
| Abseil | 5,863,183 | 5,863,183 | 0 | 440.206x | 3,538,606 / 729.386x | 3,604,726 / 716.007x |
| OpenCV | 8,563,234 | 8,563,234 | 0 | 540.800x | 5,042,880 / 918.323x | 4,758,921 / 973.119x |
| Godot | 41,693,465 | **39,589,524** | **2,103,941** | 149.857x | 29,211,890 / 203.094x | 13,692,116 / 433.298x |
| fmt | 1,046,398 | 1,046,398 | 0 | 130.304x | 613,040 / 222.416x | 627,974 / 217.127x |
| spdlog | 539,513 | 539,513 | 0 | 182.358x | 301,099 / 326.751x | 293,188 / 335.568x |
| Catch2 | 1,006,039 | 1,006,039 | 0 | 941.566x | 619,774 / 1,528.383x | 611,743 / 1,548.448x |
| nlohmann/json | 1,168,818 | 1,168,818 | 0 | 251.466x | 687,318 / 427.630x | 684,633 / 429.307x |
| range-v3 | 874,082 | 874,082 | 0 | 723.100x | 510,681 / 1,237.659x | 517,174 / 1,222.121x |
| Eigen | 1,377,958 | 1,377,958 | 0 | 2,563.408x | 868,909 / 4,065.177x | 882,376 / 4,003.134x |
| RE2 | 488,243 | 488,243 | 0 | 225.772x | 287,285 / 383.701x | 256,325 / 430.046x |
| LevelDB | 685,350 | 685,350 | 0 | 209.919x | 398,438 / 361.081x | 407,195 / 353.315x |
| simdjson | 1,408,595 | 1,408,595 | 0 | 332.514x | 847,697 / 552.529x | 801,745 / 584.197x |
| cereal | 512,588 | 512,588 | 0 | 637.743x | 280,175 / 1,166.769x | 286,067 / 1,142.737x |

## Validation

- Warning-clean release builds pass with the stock zstd library and with the
  zstd-1.4.8 multithreaded static encoder.
- Cold, bit-0, and bit-1 matrices reconstruct all 9,292 TUs exactly: **48/48
  corpus runs**.  Every category closes its per-corpus total.
- A final-source full Godot replay produces the same 39,589,524-byte wire and
  exact reconstruction after the zstd parameter checks were added.
- Three pinned final-policy Zen 4 repetitions produce identical wire and clear
  1 GB/s in both measured codec subphases.
- ASan+UBSan replays the 97.9 MiB, 11-catalog Godot TU through the four-worker
  P29 path, reconstructs it exactly, and reports no finding.
- A stock zstd build presented with the worker policy exits with
  `blob zstd worker count: Unsupported parameter`; incomplete worker/job policy
  combinations are rejected before corpus loading.
- The standalone MO benchmark reconstructs all 124,407,478 bytes exactly for
  plain, original-relative, previous-translation, and best-history layouts at
  zstd levels 1, 3, 6, and 9.  Its focused sanitizer run is exact.
- The causal Region-delta benchmark is warning-clean, exact on full Godot, and
  exact under ASan+UBSan on a focused 20-TU replay.
- Python compilation, Ruff, `git diff --check`, JSON assertions, and
  byte-identical summary/TSV regeneration pass.

## Reproduction and retained evidence

P29 requires a zstd encoder built with worker support.  The decoder consumes
the same standard frame and needs no corresponding worker build.  One exact
zstd-1.4.8 reproduction is:

```sh
git clone --branch v1.4.8 --depth 1 \
  https://github.com/facebook/zstd.git /tmp/zstd-1.4.8-mt
make -C /tmp/zstd-1.4.8-mt/lib -j8 lib-mt

g++ -O3 -march=native -std=c++17 -DICE_LINE_CAP_LOG2=23 \
  -Wall -Wextra -Wpedantic -Werror \
  -I/tmp/zstd-1.4.8-mt/lib linecache/codec50.cpp \
  /tmp/zstd-1.4.8-mt/lib/libzstd.a -lz -pthread \
  -o /tmp/codec50-p29
```

Run one receiver state:

```sh
/tmp/codec50-p29 --manifest MANIFEST --z 3 \
  --mixed-regions --byte-array-lines --direct-ordinals \
  --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor \
  --s1-max-chain 1024 --blob-z 9 \
  --blob-zstd-workers 4 --blob-zstd-job-mib 5 \
  --blob-zstd-overlap-log 3
```

Add `--half-cold-bit 0` or `--half-cold-bit 1` for the complementary rows.
Regenerate the checked ledger:

```sh
PYTHONPATH=linecache python3 linecache/summarize_blob_mt_p29.py \
  --p28-summary linecache/ml-artifacts/s1-p28-16corpus-summary.json \
  --cold-dir /tanksmall/scratch/ictmp/issue16-p29-blob9-mt-cold \
  --bit0-dir /tanksmall/scratch/ictmp/issue16-p29-blob9-mt-bit0 \
  --bit1-dir /tanksmall/scratch/ictmp/issue16-p29-blob9-mt-bit1 \
  --output linecache/ml-artifacts/blob-mt-p29-16corpus-summary.json \
  --tsv linecache/ml-artifacts/blob-mt-p29-16corpus.tsv
```

Retained logs:

- 48-run matrix: `/tanksmall/scratch/ictmp/issue16-p29-blob9-mt-{cold,bit0,bit1}/corpus*.log`
- pinned repetitions: `/tanksmall/scratch/ictmp/issue16-p29-blob9-mt-designated/`
- complete Zen 4 frontier and hashes: `/tanksmall/scratch/ictmp/issue16-p29-zen4-frontier/`
- global zstd-6 screen: `/tanksmall/scratch/ictmp/issue16-p29-z6-cold/`
- causal material screens: `/tanksmall/scratch/ictmp/issue16-p29-material-frontier/`

The machine-readable JSON and TSV retain every matrix log digest, verify all
category arithmetic, require the exact P29 policy, compare every row with P28,
and reject any per-corpus/state regression.
