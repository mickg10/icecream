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

## Next on this branch
`cap_main.cpp --role c|f`: thread codec50-m1's real ROOT → F-generated NEED → FILL through
`cap_transport` as a **genuine two-process split**. The cut is the per-TU `missReg` handoff:
C→F `Root`(tokens+block manifest) → F→C `Need`(F-derived missing region ordinals, in ROOT-expansion
order) → C→F `Fill`(paths + mixed z3 streams). F rebuilds its store only from decoded wire and
byte-checks; it never reads the C authority. Must reproduce **9,590,734** byte-exact across the
socket (per-TU stateful z3 lane preserved; independent z1/z3 frames are M4's job). Then M2
(explicit public-Line cache identity), M3 (reduced grammar), M4 (independent frames), M5 (gates).
