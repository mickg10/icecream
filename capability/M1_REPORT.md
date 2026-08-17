# M1 checkpoint — direct generation-local ordinal identity

Milestone **M1** of `CAPABILITY-PLAN.md`: collapse P25's per-object `dense_id → u64 key`
to a **generation-local typed ordinal**; latch one opaque 128-bit `SourceGeneration` at
relationship start; F decodes the manifest and generates `NEED`; byte-exact single cold
conversation. Extraction of the verified codec50 semantics (S0/S1 + P21 + reduced P24 +
F-generated NEED), not a redesign.

## Canonical invocation (reproduces the documented target)
```
build: g++ -O3 -std=c++17 -DICE_LINE_CAP_LOG2=23 codec50-m1.cpp -o codec50-m1 -lzstd
run:   ./codec50-m1 --manifest <corpus>/manifest.txt --z 3 --mixed-regions --byte-array-lines --direct-ordinals
```
`--byte-array-lines` and `-DICE_LINE_CAP_LOG2=23` are load-bearing — omitting either changes
the line ordinals and the wire total. The all-16 figure is the **sum** of the 16 per-corpus
`totalwire` values.

## Result (DuckDB / corpus3, 689 TUs, raw 1,893.7 MiB, one cold pass, byte-exact = OK)

| identity model (`--mixed-regions --byte-array-lines …`) | wire total (B) | missing (B) |
|---|---:|---:|
| P25 per-object key-map (`--key-map`) | 10,311,810 | 1,200,057 |
| **M1 direct ordinal (`--direct-ordinals`)** | **9,590,734** | 461,144 |

`9,590,734` **matches the documented cold M1 DuckDB target to the byte** (all-16 cold target =
108,363,498, the 16-corpus sum). The generation-local ordinal is **721,076 B cheaper than
P25's per-object key-map** (10,311,810 → 9,590,734), driven by collapsing the key-association
wire (missing 1,200,057 → 461,144), while staying byte-exact.
Category ledger (direct ordinal): `root=504,710 line_def=6,438,812 region_def=2,016,440
block_def=65,853 path_def=98,779 missing=461,144 framing=4,996`.
Diagnostics: `generation_latched=1, region_namespaces=1, block_lifetime=generation`.

## Components on this branch
- `cap_identity.h` — `SourceGeneration` (128-bit `array<uint8_t,16>`), `ObjectKind`,
  `ObjectKey = (kind, u32 ordinal)`, `OrdinalAllocator` (monotonic per kind, generation-local).
- `cap_transport.h` — AF_UNIX length-typed frame protocol
  (`[u8 type][u32 le len][payload]`; Hello/Root/Need/Fill/Done/Ack); one generation latched in Hello.
- `cap_transport_test.cpp` — forked C(parent)/F(child) socketpair round-trip
  (Hello[gen] → Root → F-derived Need → Fill(50 KB) → Ack; verifies latch + byte-exactness). **PASS.**
- `codec50-m1.cpp` — the semantic base (in-process C/F model) that emits the numbers above.

## M1 socket integration — COMPLETE
`cap_main.cpp` (fork + AF_UNIX `socketpair`; parent = C authority, child = F store) threads the real
ROOT → F-derived NEED → FILL dialogue through `cap_transport`'s packed 4-byte frames. Per-TU lockstep:
`C→Root[rootb|blockRaw]` · `F→Need[missingRaw]` · `C→Fill[fill_paths|mixed0..3]` · `C→Done` ·
`F→Ack[byteexact]`. F rebuilds its store from wire (+ re-reads the same system headers from disk),
never touching C's `dict`/`tokstream` after fork, and byte-checks every `.ii`. It reproduces the
codec50-m1 7-category ledger **to the byte on two corpora**:

| corpus | TOTAL wire (B) | byte-exact |
|---|---:|---|
| DuckDB (corpus3, 689 TUs) | **9,590,734** | OK — all 689 TUs |
| RocksDB (corpus2, 622 TUs) | **9,825,414** | OK — all 622 TUs |

**Framing accounting (corrected per local-oracle's M1 review).** The codec's virtual `FRAME=4`
length charge equals the real 4-byte packed header exactly — but that zero-delta is *only* the
length prefix; the virtual **category ledger is NOT the socket total**. The real socket total
(DuckDB = 10,311,523 B) exceeds the 9,590,734 B category ledger for two distinct reasons: (a)
relationship/job frames the category ledger does not count (Hello 20 B, per-job Done/Ack 4 B), and
(b) ROOT, NEED, Block-material, and path-def payloads are currently sent **raw** on the socket
whereas the virtual ledger charges their z3-compressed size. The category ledger (9,590,734) is the
codec accounting model and the byte-exact regression gate; the socket total is the literal transfer,
reported separately. Independently rebuilt + re-run: bit-identical.
Files: `cap_codec.{h,cpp}`, `cap_protocol.h`, `cap_main.cpp`.
Build: `g++ -O3 -std=c++17 -DICE_LINE_CAP_LOG2=23 cap_main.cpp cap_codec.cpp -o cap_main -lzstd`.

## Next
**M2** — public Lines enter typed NEED/FILL (restart / late-join / eviction; materialize immutable
Line bytes so Region eviction cannot dangle a view; unequal-rebind + transactional-rollback tests).
Then M3 (reduced RAW_RUN/PUBLIC_LINE_REF/BYTE_ARRAY/PP_MARKER grammar + optional EMBEDDED_OBJECT),
M4 (independent per-TU z1/z3 frames), M5 (socket/cache/evolution/performance gates).
