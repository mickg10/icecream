# M1 checkpoint — direct generation-local ordinal identity

Milestone **M1** of `CAPABILITY-PLAN.md`: collapse P25's per-object `dense_id → u64 key`
to a **generation-local typed ordinal**; latch one opaque 128-bit `SourceGeneration` at
relationship start; F decodes the manifest and generates `NEED`; byte-exact single cold
conversation. Authorized by BigOracle's P25 ruling; extraction of the verified codec50
semantics (S0/S1 + P21 + reduced P24 + F-generated NEED), not a redesign.

## Result (semantic base `codec50-m1.cpp`, `--mixed-regions --direct-ordinals`, z3)

DuckDB (corpus3, 689 TUs, raw 1,893.7 MiB), single cold chronological pass, **byte-exact = OK**:

| identity model | wire total (B) | missing (B) |
|---|---:|---:|
| P24 plain (`--mixed-regions`) | 10,762,613 | 481,986 |
| P25 per-object key-map (`--key-map`) | 11,480,684 | 1,200,057 |
| **M1 direct ordinal (`--direct-ordinals`)** | **10,759,608** | 461,144 |

The generation-local ordinal is **721,076 B cheaper than P25's per-object key-map**
(11,480,684 → 10,759,608) while staying byte-exact — the identity collapse removes the
per-object key-association wire (missing 1,200,057 → 461,144) and even edges out plain P24.
Diagnostics: `generation_latched=1, region_namespaces=1, block_lifetime=generation`.

## Components on this branch
- `cap_identity.h` — `SourceGeneration` (128-bit `array<uint8_t,16>`), `ObjectKind`,
  `ObjectKey = (kind, u32 ordinal)`, `OrdinalAllocator` (monotonic per kind, generation-local).
- `cap_transport.h` — AF_UNIX length-typed frame protocol
  (`[u8 type][u32 le len][payload]`; Hello/Root/Need/Fill/Done/Ack); one generation latched in Hello.
- `cap_transport_test.cpp` — forked C(parent)/F(child) over a socketpair; drives
  Hello[gen] → Root → F-derived Need → Fill(50 KB) → Ack; verifies the 128-bit latch and byte-exactness. **PASS.**
- `codec50-m1.cpp` — the semantic base (in-process C/F model) that emits the numbers above.

## Next on this branch
`cap_main.cpp --role c|f`: thread codec50-m1's real ROOT → F-generated NEED → FILL through
`cap_transport` as a **genuine two-process split** (separate C authority state from F's
independently-rebuilt store), reproducing 10,759,608 B byte-exact across the socket. Then
M2 (explicit public-Line cache identity), M3 (reduced RAW_RUN/PUBLIC_LINE_REF/BYTE_ARRAY/PP_MARKER
grammar), M4 (independent per-TU z1/z3 frames), M5 (cold/CACHE50/H200-C50/reorder-change-revert/
1–32-F/eviction/socket-timing gates).
