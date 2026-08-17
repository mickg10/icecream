# icecream #16 — product-shaped two-process capability harness

Authorized by BigOracle's P25 ruling; division of labor per local-oracle (2026-08-16).
Implementer owns this branch. Semantic source = the verified `codec50.cpp` @ `a52f1e3`
(S0/S1 + P21 + reduced P24 + F-generated NEED). Transport = my `fcache`/`ipc2` AF_UNIX
socket pattern. **Extract, don't redesign** the codec semantics.

## Minimal subset (KEEP only)
- **S0** semantic Roots, **S1** online Blocks (topological definition forms).
- **P21** `BYTE_ARRAY(prefix,separator,suffix,number_format,u8[])` — decimal + 2-digit hex.
- **P24 reduced grammar** — only: `RAW_RUN` (literal span), `PUBLIC_LINE_REF` (view into a
  completed prior Region), `BYTE_ARRAY`, `PP_MARKER` (exact preprocessor marker).
  Dropped: LOCAL_REF, SOURCE_COPY/SOURCE_PATCH, project-source packages.
- **P25** F-owned cache lookup + F-generated NEED/FILL — the correct authority boundary.

## Identity (the ruling's core change)
Collapse P25's per-object `dense_id→u64 key` association to a **generation-local typed
ordinal**. Latch one opaque 128-bit `SourceGeneration` at relationship start.
```
ObjectKey = (SourceGeneration, object_kind, generation_local_u32_ordinal)
```
Transmit only the typed ordinal in frames (generation already latched). Same ordinal for:
wire reference · C immutable-store lookup · F cache lookup · NEED response. Ordinals
monotonic, never reused within a generation. Generation changes only on restart/compaction.
No remapping protocol until compaction / real sparse-ID measurement proves it pays.

## Protocol: typed NEED/FILL over Regions AND public Lines (closes the P25 gap)
P25 preloaded raw Regions only and rebuilt the public-Line table from TU 1. Product must
put public Lines in the protocol so it survives: worker restart w/ Regions but no Line
table · public-Line eviction · a worker joining after C promoted Lines · arbitrary
half-cold Line state.
```
NEED  { missing Region ordinals[], missing public-Line ordinals[] }
FILL  { Region programs[], public-Line definitions[] }
```
F decodes the C manifest, binds only ordinals present in its store, emits NEED; C sends
only those FILLs; F installs into its own raw-Region arena + dense-view vectors, expands
Roots/Blocks, reconstructs the complete `.ii`, and byte-checks.

## Milestones (ruled M1→M5 order)
- **M1** — direct typed generation-local ordinals; F-decoded manifest; F-generated NEED.
  Byte-exact single-conversation cold. **Publish branch + first direct-ordinal replay when
  coherent** (target ~P24 wire: all-16 108,378,454 / DuckDB 9,593,739, MISSING preserved).
- **M2** — explicit public-Line cache identity + both topological definition forms in NEED/FILL.
- **M3** — the reduced RAW_RUN / PUBLIC_LINE_REF / BYTE_ARRAY / PP_MARKER grammar, exact.
- **M4** — independent per-TU zstd-1 and zstd-3 frames (no shared-lane assumption).
- **M5** — gates: cold, CACHE50, chronological **C50/H200** + 50%-snapshot resume,
  reorder/change/revert, multi-F (1/4/8/16/32) + bounded eviction, actual socket timing ≥1 GB/s.

## Gate set to publish each checkpoint
serialized bytes by block/frame · exact replay of selected + losing candidates · z1 & z3 ·
classify/render/encode/decode throughput (≥1 GB/s target) · full cold + executed half-cold +
C50/H200 · standard/reverse/shuffle/change/revert · retained logs + peak memory + commit IDs.

## Do NOT
Import P22 ROOT_SLICE · source-copy/package ops · P18/P19 phrases · any extra model family ·
cross-TU neural / F-side predictor. Cold-400 is NOT claimed; daemon/protocol landing is NOT
in scope.

## Layout (branch `implementer/issue16-capability`)
```
capability/
  cap_codec.{h,cpp}    minimal-subset encode/decode (extracted from codec50 semantics)
  cap_identity.h       SourceGeneration + typed ordinal ObjectKey
  cap_protocol.{h,cpp} NEED/FILL framing (Regions + public Lines)
  cap_transport.{h,cpp} AF_UNIX two-process C<->F (from fcache/ipc2)
  cap_main.cpp         --role c|f, --manifest, --z, gate flags
  CAPABILITY-PLAN.md   this file
```
