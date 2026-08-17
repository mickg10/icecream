# M3 checkpoint — reduced grammar / self-describing (F needs zero system headers)

M3 restricts the codec's control-lane grammar to four terminals and removes the source-copy path,
making the codec **self-describing**: F reconstructs every `.ii` without reading any system header.
This is a deliberate portability change (F on a different machine/toolchain no longer needs C's
`/usr/include`), and it re-bases the wire upward.

## Grammar (M3)
`RAW_RUN` · `PUBLIC_LINE` {publish · ref · recover} · `BYTE_ARRAY` · `PP_MARKER`. `EMBEDDED_OBJECT`
(op10) reserved with a no-op materializer hook + decoder reject (the P26–P29 material lane, deferred).
**Removed:** op5 SOURCE_COPY, op6 SOURCE_PATCH, and all project-source/package machinery.

## Gate 1 — exact reconstruction + ZERO system-header I/O (portability proven three ways)
F rebuilds every `.ii` byte-exact on DuckDB (689 TUs) and RocksDB (622 TUs). On a full run:
- `strace -f -e openat`: 696 total opens, **0** under `/usr/include | /usr/lib/gcc | /usr/local/include`.
- `system_header_reads()` counter == 0 for both C and F (asserted at end of run; F fails if nonzero).
- the disabled `SourceTextStore::get()` `abort()`s if ever called — an auditable, regression-enforced stub.
Independently reproduced (strace + both counters).

## Gate 2 — wire re-base
| corpus | M2 TOTAL | M3 TOTAL | delta |
|---|--:|--:|--:|
| DuckDB c3 | 9,590,734 | **9,802,066** | +211,332 (+2.2%) |
| RocksDB c2 | 9,825,414 | **10,111,855** | +286,441 (+2.9%) |

The added bytes land entirely in `line_def` (the ex-op5/op6 system-header content re-sent as z3-compressed
RAW_RUN literals), partly offset by `region_def` shrinking (op5/op6 control opcodes gone + more literal-run
coalescing); `root/block_def/path_def/missing/framing` unchanged. **100% of the ex-op5/op6 lines became
RAW_RUN, 0 became BYTE_ARRAY** (system-header lines are declarations/code, never numeric arrays). The ~2 MB
`raw_source_reused` compresses to ~211 KB — the portability cost is small because header content is repetitive.

## Gate 3 — M2 capabilities preserved
All five scenarios re-run under the reduced grammar, byte-exact with system-header reads = 0: restart,
late-join, eviction, unequal-rebind (reject), transactional-rollback. `cap_m2_test` (+op10 reject),
`cap_header_test`, `cap_transport_test` PASS.

## Gate 4 — determinism
Bit-identical TOTAL on repeat runs.

## Files
`cap_codec.{h,cpp}` (source path removed; `SourceTextStore` stubbed-abort + `system_header_reads` counter;
op10 hook), `cap_main.cpp` (M3 report + C/F header-read guard), `cap_m2_test.cpp` (+op10 reject test).
`cap_protocol.h` / `cap_transport.h` / `cap_identity.h` unchanged from M2.

## Next
**M4** — independent per-TU z1 and z3 frames (drop the shared stateful-lane assumption; each TU's Fill
frames independently, no cross-TU zstd window). Then **M5** (cold / CACHE50 / chronological H200-C50 /
reorder-change-revert / 1–32-F multi-worker / bounded eviction / socket timing ≥1 GB/s gates).
