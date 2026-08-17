# Deferred M4 transaction fixes (from local-oracle's independent M3 replay)

bigoracle issued a course-correction (owner-endorsed): **M4/M5 socket work is deferred**, the product
capability branch is **frozen at M3**, and the next work is the single-process `material_lab` research
engine. In parallel, local-oracle's independent M3 replay **accepted** the reduced grammar + the
no-system-header-read boundary (DuckDB 9,802,066 virtual / 10,522,858 socket; RocksDB 10,111,855 /
11,628,868; both exact; header reads = 0; ASan+UBSan clean) — but found the M2/M3 transactional-rollback
subgate **not yet fully state-equivalent**. Those fixes are REQUIRED before the M4 transactional gate is
accepted; they fold into M4's transaction when it resumes for Tier-C productization. **They do not affect
the research lane** — the single-process simulator does not evict/restart, so the rollback path is inert there.

## Confirmed defect — "failed Fill commits nothing" is not yet true
`cap_codec.cpp:243-245,:279` write `FpublicLastUse[ordinal]=t` on an idempotent bind / op2 reference; the
rollback (`:230-235`) restores Region data, path count, public-next, known bits, absent→present flips, but
NOT last-use. A rejected Fill therefore changes the next bounded-cache eviction choice.
Repro: install ordinal 5 @TU3 (last_use=3) → failed Fill @TU99 (op2 ref 5 + invalid following opcode) →
`decode_fill` returns false → last_use is 99, not 3. Rollback also leaves newly-grown public vectors grown
and restores a committed Region only by setting `known=false`, not its complete prior view.

## Required before the M4 transactional gate (local-oracle)
1. ~~Journal + restore every touched public ordinal's prior presence/bytes/last-use (incl. op2 + idempotent rebind); restore vector sizes.~~ **Implemented on the local-oracle correction branch.** Last-use changes are deferred until complete Fill validation; new bindings and vector extents roll back.
2. ~~Restore complete prior Region views, not only `known`.~~ **Implemented on the local-oracle correction branch.** The complete prior view is journaled and restored in reverse mutation order.
3. Stage Block-manifest installation with the rest of the TU (it currently commits before Fill decoding).
4. After a rejected TU, do not leave C's F-known mirror advanced — explicit TU acceptance before committing C's mirror, or a conservative resync that also restores the complete path table.
5. **Partially implemented:** the exact last-use/vector-extent/Region-view reproduction and subsequent valid Fill are retained in `cap_m2_test`. The live whole-TU failed-component-then-valid-TU test remains part of M4.
6. Validate the Rejoin TU (C receives the payload but never calls `unpack_rejoin`); convert M4 component parsing to bounded cursor reads requiring complete payload consumption.

## Also
- ~~5 `-Wmisleading-indentation` warnings in `cap_codec.cpp`.~~ **Cleaned on the local-oracle correction branch; both `cap_main` and `cap_m2_test` compile warning-free with `-Wall -Wextra -Wpedantic -Werror`.**
- `M1_REPORT.md` relationship-frame sizes corrected: cap_main **Hello = 25 B** (4-byte header + 16-byte generation + NREG + NBLK varints), **Done = 4 B**, **Ack = 5 B** (was mis-stated as 20 / 4 / 4).

## Status
M2 transactional-rollback subgate = **PROVISIONAL** (items 3, 4, the live half of 5, and 6 remain for M4).
M1/M2/M3 semantic + byte-exact + zero-header-read portability results **stand and are independently reproduced**.
