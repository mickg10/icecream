# M2 checkpoint — public Lines in typed NEED/FILL (survives restart / late-join / eviction)

M2 gives public Lines explicit generation-local ordinal identity in the NEED/FILL protocol so the
relationship survives worker **restart**, **late join**, and public-Line **eviction**, with immutable
Line materialization (an evicted source Region can never dangle a view) and transactional Fill semantics.

## Cold regression gate — PRESERVED byte-exact (both corpora)
The full `wire by category` ledger is byte-for-byte identical to M1 after all M2 changes:

| corpus | root | line_def | region_def | block_def | path_def | missing | framing | TOTAL |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| DuckDB c3 | 504710 | 6438812 | 2016440 | 65853 | 98779 | 461144 | 4996 | **9,590,734** |
| RocksDB c2 | 1389952 | 3522620 | 3443059 | 63770 | 58920 | 1342121 | 4972 | **9,825,414** |

In pure cold the recovery machinery is a strict no-op: op7/op8/op9 = 0, NEED drop-list = 0, no Rejoin
frame. Independently rebuilt + re-run (bit-identical).

## Five scenarios — all byte-exact, zero errors
1. **Restart** — F drops its whole store mid-conversation and recovers Regions/Blocks/paths/public Lines
   via NEED/FILL. Verified at restart TUs {1,50,400,688} on DuckDB and {1,311,621} on RocksDB.
2. **Late-join** — a fresh F joins after C promoted public Lines (C warm-passes to build authority, no I/O);
   serves the tail byte-exact.
3. **Eviction** — F drops LRU public Lines under a bounded budget, reports them in the next NEED, C
   re-provides on next use (op8 view — source Region retained); byte-exact over 10^4s of evict/re-NEED cycles.
4. **Unequal-rebind** — rebinding a held ordinal to different bytes is rejected (rollback); same bytes
   idempotently accepted.
5. **Transactional rollback** — a Fill that fails validation commits nothing (region data + path install +
   public store reverted to the pre-Fill checkpoint); the ordinary path recovers on the next Fill.

## Mechanism
- Public-Line ordinal = C's `nextMixedPublic` = `ObjectKind::PublicLine` (reuses `cap_identity.h`).
- F materializes **immutable** public-Line bytes at publish time (`FpublicBytes[ordinal]`); op2 resolves
  from those, not the source Region → eviction cannot dangle a view.
- Recovery rides new control-lane opcodes off the cold path: op7 reprovide-bytes, op8 reprovide-view,
  op9 publish-bytes (op1's first-publish byte form, chosen only when F lacks the source Region post-restart;
  op9 = 0 in cold, verified). C's `fknownPublic` model is kept exact by F's drop reports + the restart
  reset, with an assert that C never materializes an op2 for an ordinal it believes F lacks.
- NEED reuses M1's reserved trailing varint for the drop list (cold = 0 = identical). Restart uses the new
  `Frame::Rejoin = 6` (one of the two free header type-values; never sent in cold). The F block store is now
  sparse (id-indexed) to accept an arbitrary restart subset — F-internal, cold preserved.
- Fill decode is transactional: it records pre-Fill checkpoints (region data, path install, public store,
  block store, known-flags) and reverts the whole batch on any failure (the old `exit(2)` paths became
  recoverable failures).

## Files
`cap_transport.h` (+`Rejoin=6`), `cap_header_test.cpp` (type 6 valid, 7 reserved), `cap_codec.{h,cpp}`,
`cap_protocol.h`, `cap_main.cpp` (`--restart T | --latejoin T | --evict N`), `cap_m2_test.cpp`.
Build: `g++ -O3 -std=c++17 -DICE_LINE_CAP_LOG2=23 cap_main.cpp cap_codec.cpp -o cap_main -lzstd`.
One scoped future refinement noted in code: recovery-mode literal blocks persist for the remainder after a
restart (chosen: persistent-simple); scoped COPY via an `freg_base` map is a later optimization.

## Next
**M3** — the reduced grammar (`RAW_RUN` / `PUBLIC_LINE_REF` / `BYTE_ARRAY` / `PP_MARKER` + optional
`EMBEDDED_OBJECT`), dropping the source-copy/package/local-ref paths. Note: removing the source-copy path
re-bases the wire total (it is a deliberate grammar change, not a transport change), so M3 establishes a new
reference number rather than preserving 9,590,734. Then M4 (independent per-TU z1/z3 frames), M5 (gates).
