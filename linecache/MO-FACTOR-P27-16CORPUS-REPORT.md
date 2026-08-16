# P27 exact causal GNU MO catalog factor: 16-corpus result

## Result

P27 is a measured exact factor beneath P26's generated `BYTE_ARRAY` blob path. It recognizes only
canonical GNU MO catalogs, replaces their repeated original-string tables with append-only
generation-local string IDs, carries every translation and every non-catalog member explicitly,
and reconstructs the original inflated catalog bytes exactly before P26 regenerates the original
zlib members.

All 48 complete executions reconstruct every byte of all 9,292 TUs:

| complete 16-corpus state | same-machine P26 | P27 | P27 saving | P27 ratio | target result |
|---|---:|---:|---:|---:|---:|
| cold | 97,015,268 | **93,980,202** | **3,035,066** | **303.84x** | cold-400 open by **22,593,523 B** |
| cache bit 0 | 65,178,114 | **62,151,794** | **3,026,320** | **459.43x** | 80,621,564 B below the half-200 allowance |
| cache bit 1 | 43,872,708 | **43,863,608** | **9,100** | **650.99x** | 98,909,750 B below the half-200 allowance |

The two half-cold aggregate targets remain closed. Cold-400 is not closed. P27 removes another
3.04 MB, reducing the cold gap from 25.63 MB in the published P26 ledger to 22.59 MB in the
current same-machine ledger.

Only Godot has P26 blob members, so only Godot executes the P27 factor. Every wire category in the
other fifteen corpora is byte-identical with the same executable's P26 path. The current aggregate
is 6,666 bytes below the earlier published P26 environment because OpenCV and LevelDB see the
already-documented local source-file availability difference. The P27 saving itself is isolated
with same-binary, same-input Godot controls and is entirely attributable to this factor.

P27 does **not** meet an 8 MB continuation threshold if that threshold is applied independently to
every follow-on candidate. It is nevertheless a small composable factor, it passes the measured
encode/decode subphase floor, and it reduces both cold and the hard Godot half. BigOracle should
explicitly accept or reject it on that basis rather than treating it as cold-400 closure. The
complete C ingest-to-wire product speed gate remains open.

## What was found in P26's inflated material

The P26 Godot dump contains 112 complete inflated members totaling 124,407,478 bytes:

| member class | members | inflated bytes |
|---|---:|---:|
| canonical GNU MO catalogs | **107** | **114,059,263** |
| JSON | 2 | 656,184 |
| other text/XML/UTF-8 material | 3 | 9,692,031 |
| total | **112** | **124,407,478** |

All 107 catalogs use the same canonical little-endian layout and rebuild exactly from their
logical tables. Their repeated material is substantial:

| catalog material | occurrences | unique values | occurrence bytes | unique string bytes |
|---|---:|---:|---:|---:|
| original/source strings | 471,784 | **34,542** | 44,087,525 | **4,877,391** |
| translations | 471,784 | 445,179 | 62,420,198 | carried per occurrence |

The source/original side repeats heavily across language catalogs. The translated side does not,
so P27 factors only the source/original strings and leaves translations as exact length-prefixed
bytes. This is the simplest split supported by the data.

## Exact wire algorithm

### C state

For one latched `SourceGeneration`, C owns:

```text
original_string -> u32 original_id
next_original_id
```

IDs are append-only and never renumbered inside the generation. The current Godot cold state ends
with 34,542 entries and 4,877,391 bytes of string payload. The C implementation stores each string
once as the key of the lookup table; an earlier prototype stored it twice and was removed before
the final timing.

### Per-TU encode

P26 first discovers complete zlib streams represented by generated C/C++ byte arrays and inflates
them. P27 then processes the ordered inflated members:

1. Parse a member as a canonical MO catalog only if all of these hold:
   - little-endian MO magic and revision zero;
   - original table at byte 28;
   - translation table immediately after the original table;
   - no hash table;
   - all string lengths, offsets, and terminating zero bytes are in range;
   - rebuilding the canonical catalog from the parsed strings produces every original byte.
2. For an accepted catalog, emit its string count and the ID of each original string.
3. Put each previously unseen original string in a pending-definition list and assign it the next
   provisional ID. Repeated originals in the same TU reuse the same provisional ID.
4. Emit every translation as `length, bytes` in catalog order.
5. For a member that is not an accepted catalog, emit `ordinary length` and copy all member bytes
   into the ordinary leg.
6. Serialize the pending definitions as `count, (length, bytes)*`.

This produces four exact raw parts:

```text
CONTROL       member count; per-member tag; original-ID sequences or ordinary lengths
DEFINITIONS   new original strings, in append order
TRANSLATIONS  exact length-prefixed translation strings
ORDINARY      exact bytes for non-catalog members
```

The four parts are concatenated in that order and encoded as one per-TU zstd-3/LDM frame. Four
uncompressed part lengths in the array-control stream delimit the decoded frame.

### Live selector without double compression

The exhaustive bake-off initially compressed all three candidates:

1. P26 inflated-member frame;
2. untouched original DEFLATE bytes;
3. P27 factor frame.

That found the right size but reduced Zen 4 C throughput to 0.937 GB/s. The final live selector
uses a cheap structural admission rule:

```text
admit P27 candidate iff
    canonical_catalog_count > 0
    and factor_raw_bytes * 4 <= inflated_blob_bytes * 3
```

For an admitted TU, C compresses only the factor candidate and compares its complete framed cost
with the already-known untouched-byte cost. If the factor loses, C sends untouched bytes. For a TU
that does not pass structural admission, C runs the ordinary P26 transformed-versus-untouched
choice and does not compress the factor form.

The 25% raw-reduction threshold is format-based, not corpus-named. On the exhaustive Godot trace,
all four admitted TUs win on final zstd-3 wire. The optimized selector differs from exhaustive
three-way selection by only 40 total wire bytes: it keeps P26 for one 9.45 MB non-catalog member
where wrapping the same bytes in mode 4 happened to save 35 payload bytes.

### Transactional dictionary rule

Encoding does not mutate the C dictionary. It returns explicit pending definitions alongside the
candidate. C commits those definitions only after mode 4 wins and is selected for transmission.
If P26 transformed bytes or untouched bytes are selected, the provisional P27 state is discarded.

The exact state transition is:

```text
(C_state, members)
      -> encode against C_state
      -> {candidate bytes, pending definitions}
      -> choose mode
      -> mode 4: transmit, then append pending definitions
         other : transmit other mode, leave C_state unchanged
```

This prevents an unsent candidate from changing later IDs.

### Mode 4 framing

P27 extends P26's blob selector with mode 4:

```text
ARRAY_CONTROL:
  blob_mode = 4
  canonical_zlib_version
  canonical_zlib_level
  member_count
  control_raw_size
  definitions_raw_size
  translations_raw_size
  ordinary_raw_size
  member descriptors...

BLOB_FRAME:
  zstd3_ldm(CONTROL || DEFINITIONS || TRANSLATIONS || ORDINARY)
```

All existing P26 descriptors remain: first array entry, entry count, original DEFLATE size,
inflated size, and exact original digest.

### F decode and reconstruction

F owns an independent generation-local vector indexed by `original_id`:

```text
u32 original_id -> original string bytes
```

For mode 4, F:

1. decodes the factor frame to the sum of the four advertised raw sizes;
2. installs the explicit definitions in append order;
3. decodes each member command;
4. looks up original strings by direct vector index;
5. consumes the exact translations;
6. rebuilds the canonical MO header, tables, strings, and terminators;
7. copies ordinary members directly;
8. checks every reconstructed inflated member length against its P26 descriptor;
9. passes the exact reconstructed inflated members into the unchanged P26 canonical-zlib path;
10. checks regenerated original-member length and digest;
11. requests only member ordinals that did not reproduce and receives their original DEFLATE
    bytes from C.

F does not run the C lookup table or the selector. Everything needed to rebuild the catalog is in
the explicit frame plus F's previously installed append-only vector.

## Godot per-TU decision evidence

The exhaustive three-way trace explains the final policy. Sizes include the payload frame and the
candidate-specific metadata used by the selector.

| TU | members | MO members | inflated raw | factor raw | P26 transformed | untouched | factor | exhaustive winner | final live route |
|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 21 | 1 | 0 | 328,092 | 328,098 | 22,386 | **20,801** | 22,409 | untouched | untouched |
| 257 | 1 | 0 | 9,454,258 | 9,454,265 | 1,438,372 | 1,566,109 | **1,438,337** | factor wrapper | P26 transformed |
| 503 | 1 | 0 | 225,076 | 225,082 | **126,204** | 128,499 | 126,221 | P26 transformed | P26 transformed |
| 525 | 11 | 11 | 91,644,100 | 56,782,568 | 13,201,612 | 22,219,952 | **11,735,684** | factor | factor |
| 531 | 39 | 39 | 17,223,579 | 9,308,782 | 3,670,101 | 5,447,597 | **2,581,280** | factor | factor |
| 532 | 32 | 31 | 137,432 | 83,134 | 37,979 | 65,000 | **29,118** | factor | factor |
| 535 | 26 | 26 | 5,066,849 | 2,569,151 | 1,240,766 | 1,720,799 | **769,310** | factor | factor |
| 539 | 1 | 0 | 328,092 | 328,098 | 22,386 | **20,801** | 22,409 | untouched | untouched |

The four real catalog TUs are admitted and selected. Their definitions are causal: 24,036 new
originals in the first large catalog TU, then 6,348, 117, and 4,041. The final total is 34,542.

## Wire ledger

### Complete cold ledger

| category | P27 bytes |
|---|---:|
| Root | 3,687,803 |
| Block definitions | 586,843 |
| path definitions | 828,100 |
| F-generated missing dialogue | 3,317,206 |
| Region/control | 19,746,257 |
| Line/material | 65,743,189 |
| framing | 70,804 |
| **total** | **93,980,202** |

The Line/material leg is:

| material sub-leg | bytes |
|---|---:|
| RAW_RUN literal | 41,161,991 |
| byte-array control | 556,403 |
| non-blob byte-array values | 7,294,859 |
| selectors | 8,409 |
| selected blob payloads | **16,721,527** |
| **Line/material total** | **65,743,189** |

P26's same-input selected blob payload is 19,756,636 bytes. P27 reduces that payload by 3,035,109
bytes; the array-control metadata changes by a few dozen compressed bytes, producing the final
3,035,066-byte complete-wire saving.

### Factor state by cache complement

| state | policy/selected TUs | MO members | member raw | new originals | string bytes per side | factor candidate wire |
|---|---:|---:|---:|---:|---:|---:|
| cold | 4 / 4 | 107 | 114,059,263 | 34,542 | 4,877,391 | 15,115,392 |
| bit 0 | 3 / 3 | 76 | 113,934,528 | 34,429 | 4,874,534 | 15,086,160 |
| bit 1 | 1 / 1 | 31 | 124,735 | 161 | 3,424 | 28,879 |

## Performance

The binding speed host is `tt-quietbox2`, an AMD EPYC 8124P Zen 4 system. The full 2,207-TU,
5,932,762,185-byte Godot run used zstd-3, eight canonical-zlib workers, and CPUs 0–15.

| full Godot path | total wire | C encode | F decode | pipeline minimum |
|---|---:|---:|---:|---:|
| same-binary P26 control | 44,731,020 | 1.096 GB/s | 1.227 GB/s | **1.096 GB/s** |
| exhaustive three-way P27 prototype | 41,695,914 | 0.937 GB/s | 1.190 GB/s | **0.937 GB/s** |
| optimized P27 repetition 1 | 41,695,954 | 1.073 GB/s | 1.189 GB/s | **1.073 GB/s** |
| optimized P27 repetition 2 | 41,695,954 | 1.080 GB/s | 1.188 GB/s | **1.080 GB/s** |
| optimized P27 repetition 3 | 41,695,954 | 1.081 GB/s | 1.190 GB/s | **1.081 GB/s** |

The final already-ingested encode/decode subphases clear 1 GB/s in all three repetitions. Relative
to P26, its measured C encode subphase is about 1.4–2.1% slower and its F decode subphase about 3%
slower on this input. It avoids compressing 124 MB of P26 inflated candidates on the four
qualifying TUs and instead compresses about 68.7 MB of factored material.

These rates do not include the harness's initial C read/parse/intern pass. A separate GNU-time run
of the same final binary and input measured the complete one-process capability harness:

| phase | wall time | raw-rate equivalent |
|---|---:|---:|
| C read, parse, and intern | 6.5 s | 0.91 GB/s |
| S1 construction | 0.1 s | 59 GB/s |
| already-ingested C encode | 5.51 s | 1.076 GB/s |
| F decode and expansion | 4.99 s | 1.190 GB/s |
| complete harness | 18.16 s | 0.327 GB/s |

The process consumed 18.56 user seconds and 7.09 system seconds: 25.65 CPU-seconds, 141% average
CPU, or 4.32 CPU-seconds per raw GB including both simulated endpoints, exact verification, and
harness overhead. Sequential C ingestion plus S1 plus encode is about 12.1 seconds, or 0.49 GB/s.
Streaming ingestion and encode in separate lanes has an observed stage ceiling near 0.91 GB/s
before job-level concurrency. P27 therefore passes the existing encode/decode subphase gate, but a
complete preprocessor-pipe-to-wire product measurement remains required.

The capability harness retains the entire corpus and all analysis stores, so its peak RSS is about
7.56 GiB on Godot. That is not P27's product-state cost. P27's persistent string payload is
4,877,391 bytes on C and the same on F; C adds one hash-table entry per string and F adds one vector
string per ID. Per-TU temporary material is bounded by the selected inflated-member batch and the
factor frame. A product port can decode the four slices without the harness's verification copies.

## Reorder and edit/revert behavior

P27 was run over full Godot in standard, reverse, and deterministic shuffled TU order:

| order | exact | total wire | delta from standard | factor members | final dictionary | factor candidate wire |
|---|---|---:|---:|---:|---:|---:|
| standard | yes | 41,695,954 | — | 107 | 34,542 | 15,115,392 |
| reverse | yes | 41,642,748 | -53,206 | 107 | 34,542 | 15,116,122 |
| shuffle seed 50 | yes | 42,515,097 | +819,143 | 107 | 34,542 | 15,115,923 |

The complete codec changes by -0.13% / +1.96% as S1 Blocks, first-use Regions, and streaming
contexts see a different history. P27 itself stays nearly fixed: all 107 catalogs are represented,
the final dictionary is identical by content and count, and factor-candidate wire changes by at
most 730 bytes.

A focused `original -> one-byte compressed-array change -> original` sequence was also executed in
one retained state:

- original TU: 11/11 catalog members selected, 24,036 definitions installed;
- changed TU: the edited zlib member is no longer recognized, the remaining 10 catalogs reuse the
  existing dictionary with **zero** new definitions;
- reverted TU: the original Region is already present and requires no new blob transfer;
- all three reconstructed files are exact.

This demonstrates that an input change cannot advance the dictionary through an unsent
candidate, and that revert returns to the prior exact cached object.

## Validation

- Warning-clean release builds of the integrated codec and standalone factor benchmark.
- Cold, bit-0, and bit-1 matrices reconstruct all 9,292 TUs exactly: **48/48 corpus runs**.
- All seven wire categories close every per-corpus total.
- All zero-blob corpora retain identical category ledgers.
- Three final-source Zen 4 encode/decode subphase executions exceed 1 GB/s and have identical wire.
- Standalone 112-member replay reconstructs all 124,407,478 bytes exactly at zstd levels 1, 3, 6,
  and 9.
- A late-invalid factor command is rejected without committing any pending F dictionary entry; the
  same decoder then accepts the valid frame and reconstructs exactly.
- ASan+UBSan focused run covers an 11-catalog, 97.9 MiB TU and forces three member recoveries; it
  reconstructs exactly with no sanitizer finding.
- Full reverse and deterministic-shuffle executions reconstruct all Godot TUs exactly.
- Focused one-byte change/revert execution reconstructs all three states exactly.
- `git diff --check`, Python compilation, deterministic summary regeneration, and the existing
  codec parsers pass.

## Per-corpus execution

| corpus | cold P26 | cold P27 | save | cold ratio | bit-0 P27 / ratio | bit-1 P27 / ratio | matrix pipeline | MO members |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| llvm | 9,330,294 | 9,330,294 | 0 | 388.01x | 4,915,650 / 736.48x | 5,422,973 / 667.58x | 3.828 | 0 |
| rocksdb | 9,825,414 | 9,825,414 | 0 | 316.97x | 6,014,639 / 517.79x | 6,013,349 / 517.90x | 3.312 | 0 |
| duckdb | 9,590,734 | 9,590,734 | 0 | 207.05x | 5,907,546 / 336.13x | 4,992,901 / 397.71x | 2.084 | 0 |
| abseil | 5,864,879 | 5,864,879 | 0 | 440.08x | 3,540,302 / 729.04x | 3,606,422 / 715.67x | 3.683 | 0 |
| opencv | 8,564,687 | 8,564,687 | 0 | 540.71x | 5,044,333 / 918.06x | 4,760,374 / 972.82x | 6.198 | 0 |
| godot | 44,731,020 | **41,695,954** | **3,035,066** | 142.29x | 31,314,252 / 189.46x | 13,698,513 / 433.10x | 1.015 | 107 |
| fmt | 1,046,398 | 1,046,398 | 0 | 130.30x | 613,040 / 222.42x | 627,974 / 217.13x | 1.251 | 0 |
| spdlog | 539,513 | 539,513 | 0 | 182.36x | 301,099 / 326.75x | 293,188 / 335.57x | 1.745 | 0 |
| catch2 | 1,006,074 | 1,006,074 | 0 | 941.53x | 619,809 / 1,528.30x | 611,778 / 1,548.36x | 6.861 | 0 |
| nlohmann-json | 1,168,818 | 1,168,818 | 0 | 251.47x | 687,318 / 427.63x | 684,633 / 429.31x | 2.824 | 0 |
| range-v3 | 874,465 | 874,465 | 0 | 722.78x | 511,064 / 1,236.73x | 517,557 / 1,221.22x | 5.459 | 0 |
| eigen | 1,378,045 | 1,378,045 | 0 | 2,563.25x | 868,996 / 4,064.77x | 882,463 / 4,002.74x | 8.060 | 0 |
| re2 | 488,243 | 488,243 | 0 | 225.77x | 287,285 / 383.70x | 256,325 / 430.05x | 2.236 | 0 |
| leveldb | 685,363 | 685,363 | 0 | 209.92x | 398,451 / 361.07x | 407,208 / 353.30x | 1.998 | 0 |
| simdjson | 1,408,733 | 1,408,733 | 0 | 332.48x | 847,835 / 552.44x | 801,883 / 584.10x | 3.547 | 0 |
| cereal | 512,588 | 512,588 | 0 | 637.74x | 280,175 / 1,166.77x | 286,067 / 1,142.74x | 4.858 | 0 |

## Reproduction and retained evidence

Build:

```sh
g++ -O3 -march=native -std=c++17 -DICE_LINE_CAP_LOG2=23 \
  -Wall -Wextra -Werror linecache/codec50.cpp \
  -lzstd -lz -pthread -o /tmp/codec50-p27
```

Run one state:

```sh
/tmp/codec50-p27 --manifest MANIFEST --z 3 \
  --mixed-regions --byte-array-lines --direct-ordinals \
  --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor
```

Add `--half-cold-bit 0` or `--half-cold-bit 1` for the complementary cache states.

Regenerate the checked JSON and TSV:

```sh
PYTHONPATH=linecache python3 linecache/summarize_mo_factor.py \
  --p27-cold-dir /tmp/issue16-p27-cold \
  --p27-bit0-dir /tmp/issue16-p27-bit0 \
  --p27-bit1-dir /tmp/issue16-p27-bit1 \
  --godot-p26-cold-log /tmp/issue16-p27-godot-controls/cold.log \
  --godot-p26-bit0-log /tmp/issue16-p27-godot-controls/bit0.log \
  --godot-p26-bit1-log /tmp/issue16-p27-godot-controls/bit1.log \
  --speed-log /tmp/issue16-p27-quietbox2/issue16-p27-final-godot-z3-r1.log \
  --speed-log /tmp/issue16-p27-quietbox2/issue16-p27-final-godot-z3-r2.log \
  --speed-log /tmp/issue16-p27-quietbox2/issue16-p27-final-godot-z3-r3.log \
  --output linecache/ml-artifacts/mo-factor-p27-16corpus-summary.json \
  --tsv linecache/ml-artifacts/mo-factor-p27-16corpus.tsv
```

Retained logs:

- `/tmp/issue16-p27-{cold,bit0,bit1}`
- `/tmp/issue16-p27-godot-controls/{cold,bit0,bit1}.log`
- `/tmp/issue16-p27-quietbox2/issue16-p27-final-godot-z3-r{1,2,3}.log`
- `tt-quietbox2:/tmp/issue16-p27-final-time.{log,txt}`
- `/tmp/issue16-p27-mo-factor-bench.log`
- `/tmp/issue16-p27-asan-mo-fallback.log`
- `/tmp/issue16-p27-asan-finalcheck.log`
- `/tmp/issue16-p27-godot-{reverse,shuffle50}.log`
- `/tmp/issue16-p27-edit-revert.log`

Every complete-matrix and designated-speed log digest is recorded in the checked machine summary
and TSV.

## Decision and remaining work

P27 is a real exact, causal, composable improvement and should be reviewed as a small mode beneath
P26. It is not a route to cold-400 by itself. The remaining cold wire is still dominated by the
41.16 MB RAW_RUN residual and 19.75 MB Region/control program. The measured generic residual,
token, alphabet, and control-split ceilings on this branch do not expose another easy 22.59 MB.

Before a product port, BigOracle should review the four-part mode-4 boundary and the 25% structural
admission rule, and the implementer should independently replay the report. If accepted, the port
can use the same factor module on C and F; only C needs the string-to-ID lookup, while F keeps the
small direct-ID vector.

The larger issue #16 acceptance remains open: cold-400, chronological C50/H200, the product-shaped
two-process path, and the full reorder/change/revert/multi-F/eviction gates still require closure.
