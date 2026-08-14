# Semantic Root contender: exact online reuse crosses 4,000x on all four corpora

From: `mickg10/local-oracle`
For: issue #16 bake-off

## Result

This branch adds the missing S0 semantic-Root plane to the local-oracle's real
two-process Protocol-50 codec. It is not a token-count model:

- C and an exec'd F have independent stores and communicate only through actual
  four-byte-length-framed messages over a socket pair.
- zstd-1 compression and decompression are executed on the bytes that cross the
  socket.
- F receives no manifest, expands its own decoded objects, and writes the complete
  reconstructed TU through a pipe.
- An independent consumer compares every output byte with the selected original,
  edited, reversed, or shuffled TU.
- Reported wire is `C->F + F->C`, including frames and replies.

For each corpus, forty builds were run in one conversation. Build 1 uses manifest order, build 2
uses reverse order, and builds 3-40 use a different deterministic shuffle seed.
All four complete-codec intervals exceed 1 GB/s, all outputs are byte-exact, and
all cumulative total ratios exceed 4,000x:

| Corpus | Cold build wire | Warm build wire | Warm ratio | Build crossing 4,000x | 40-build total wire | 40-build total ratio | Codec GB/s |
|---|---:|---:|---:|---:|---:|---:|---:|
| LLVM | 20,053,106 | 69,328 | 52,219.5x | 24 | 22,756,986 | 6,363.36x | 1.664 |
| RocksDB | 18,597,473 | 34,832 | 89,409.8x | 25 | 19,956,009 | 6,242.37x | 1.523 |
| DuckDB | 17,347,279 | 38,584 | 51,464.7x | 38 | 18,852,143 | 4,213.24x | 1.583 |
| OpenCV | 16,000,495 | 84,336 | 54,911.2x | 15 | 19,289,687 | 9,603.05x | 1.666 |

The result is deliberately labelled repeated-build total compression. It does
not claim 4,000x on the first cold build. The cold first-use definition stream
remains the dominant independent research lane.

## Minimal protocol change

The existing full-Root message gains two fields:

```text
FULL_ROOT := txid:u32, mode:0:u8, root_key:u64,
             token_count:uvar, token[token_count]:uvar
```

After decoding and expanding this Root, F stores the immutable semantic meaning:

```text
root_key -> [region_key_0, region_key_1, ...]
```

The stored meaning is the complete Region sequence, not the current Block parse.
Consequently, later changes in the C-only Block learner cannot change Root
identity or F expansion.

A previously published exact Root uses one standalone message:

```text
ROOT_REF := txid:u32, raw_length:u64, root_key:u64
ACK      := txid:u32, raw_length:u64
```

With the common frame envelope, this is 32 C->F bytes plus 24 F->C bytes, or 56
actual bytes per reused TU. ROOT_REF requires no DICT, ROOT, FILL, or READY frame:
the earlier successful full Root implies that its complete immutable closure is
already resident in this sticky-F conversation.

## Strict online order

For every TU, C executes this logical order:

1. Intern the complete current TU into its immutable Line/Region store.
2. Look up the complete semantic Region sequence only in Roots published by prior
   TUs.
3. Choose either ROOT_REF or a full Root encoding.
4. Let the lower Block plane cover/observe the TU.
5. If no Root existed, allocate a new immutable Root key for use by later TUs.
6. Serialize and transmit the chosen representation.

The lookup occurs before publication. Therefore a TU cannot reference a Root
learned from itself. A later duplicate in the same chronological build may reuse
an earlier TU's published Root, which is normal online reuse.

F processes frames in socket order. C can send a pipelined ROOT_REF before it has
received the defining TU's ACK, but F cannot process that reference until it has already
processed and installed the preceding full Root. No shared runtime state is used.

## Reordering and edit/revert evidence

`--order mixed` is a fixed perturbation schedule:

```text
build 1: original
build 2: reverse
build 3+: deterministic Fisher-Yates shuffle, new seed per build
```

The four 40-build rows therefore contain one reverse order and 38 distinct
shuffled orders after cold learning. Every warm build has exactly the same wire
within a corpus, and every TU is a semantic Root hit.

`--edit-cycle` uses this five-build schedule:

```text
1 original cold
2 original content, reversed
3 one common-header line inserted, shuffled
4 same changed content, newly shuffled
5 revert to original content, newly shuffled
```

On DuckDB the edit changed all 689 TUs. The complete exact results were:

| Build | State | Root definitions/references | Wire | Ratio |
|---:|---|---:|---:|---:|
| 1 | original cold | 560 / 129 | 17,347,279 | 114.5x |
| 2 | original warm, reversed | 0 / 689 | 38,584 | 51,464.7x |
| 3 | changed, first occurrence | 560 / 129 | 2,766,591 | 717.8x |
| 4 | changed steady, shuffled | 0 / 689 | 38,584 | 51,465.5x |
| 5 | reverted, shuffled | 0 / 689 | 38,584 | 51,464.7x |

This demonstrates immediate reuse of unchanged duplicate Roots during the first
changed build, acquisition of new meanings for later builds, and immediate reuse
of the old meanings after revert.

## Control

On full DuckDB, `--no-root-memo --passes 2 --order reverse` is byte-exact but its
second-build wire is 2,352,225 bytes (844.2x). Semantic Root reuse reduces that to
38,584 bytes (51,464.7x), a 60.96x wire reduction for the same complete output.

## Commands and retained evidence

Build:

```bash
g++ -O3 -DNDEBUG -march=native -std=c++17 linecache/codec50.cpp \
    -o linecache/codec50-semantic-root -lzstd -pthread
```

Canonical row shape:

```bash
taskset -c 2-4 ./linecache/codec50-semantic-root \
    --manifest MANIFEST --passes 40 --order mixed --z 1 --window 8
```

Retained raw logs:

- `linecache/traces/local-semantic-root-llvm-40-mixed.log`
- `linecache/traces/local-semantic-root-rocksdb-40-mixed.log`
- `linecache/traces/local-semantic-root-duckdb-40-mixed.log`
- `linecache/traces/local-semantic-root-opencv-40-mixed.log`
- `linecache/traces/local-semantic-root-duckdb-edit-cycle.log`
- `linecache/traces/local-semantic-root-duckdb-no-memo.log`

Additional gates:

- ASan+UBSan, 20 LLVM TUs, five mixed-order edit/revert builds: exact PASS,
  child PASS, no report.
- Lockstep, same input: exact PASS and byte-for-byte identical wire accounting to
  the bounded-window run.
- zstd level is 1 throughout the headline rows.

## Scope and next contenders

S0 is now a working, order-stable incumbent for exact recurring Roots. It should
remain a cheap first choice in the Root-plane selector. It does not eliminate the
other independent lanes:

1. A novel but similar Root still needs S1 relative slice/patch or the lower
   online Block plane; the edited first-occurrence row shows that remaining cost.
2. First-use Line/Region definitions still determine cold-build compression.
3. Generic-C++ pretraining should compete as a charged cold-start prior for exact
   definition/template candidate selection, never as a replacement for the
   explicit exact residual.
4. A product implementation needs bounded Root-store retention integrated with
   the existing per-C GUID lifetime policy.

The simplest selector is therefore a set of independent choices that snap
together: exact semantic Root hit first; otherwise relative-Root candidate versus
online Block covering versus literal Root, chosen by actual serialized byte cost.
