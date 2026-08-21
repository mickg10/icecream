# CAP-M4 checkpoint — independent per-TU frames (preserved for the deferred M4/M5 productization)

The M4 "independent per-TU frames" checkpoint, built + validated before bigoracle deferred M4/M5.
Published here as the starting point for **local-oracle's M4/M5 socket productization**.

`cap_main_m4.cpp` is M3's `cap_main` with the mixed lane converted to per-TU **self-contained** zstd
frames: `ZSTD_CCtx` session+params reset per TU (so `contentSizeFlag` returns to default and each frame
self-describes its size for F's one-shot decode), no shared cross-TU window, no last-TU `ZSTD_e_end`.
It builds against the committed `cap_codec.{h,cpp}` + `cap_transport.h`; `cap_main.cpp` (M3, shared window)
is unchanged alongside it.

## Result (byte-exact, independently re-verified)
Build: `g++ -O3 -std=c++23 -DICE_LINE_CAP_LOG2=23 cap_main_m4.cpp cap_codec.cpp -o cap_main_m4 -lzstd`

| corpus | M3 (shared window) | CAP-M4 (independent frames) | delta |
|---|--:|--:|--:|
| DuckDB c3 | 9,802,066 | **10,086,267** | +284,201 (+2.9%) |
| RocksDB c2 | 10,111,855 | **10,404,024** | +292,169 (+2.9%) |

Both `byte-exact=OK`, system-header reads = 0. The +2.9% is the lost-cross-TU-window penalty plus ~12 B/frame
zstd-header overhead — the expected cost of making each TU independently decodable (needed for multi-worker F).
All 5 M2 recovery scenarios remained byte-exact at this checkpoint.

## For the M4/M5 resume (local-oracle)
This is the independent-frames foundation. bigoracle's **expanded** M4 (deferred) additionally requires:
the actual socket byte stream as the product score (per-component `min(RAW, z1, z3)` + a 1-byte selector,
also compressing Root/NEED/Block/path), the full 16-corpus matrix (z1+z3 actual-socket, rates, RSS,
first-cold + retained-2nd-build), and a lost/corrupt-frame → valid-independent-TU test. That design is in
the #16 thread. Also fold in the six transaction state-equivalence fixes in `DEFERRED_M4_FIXES.md`
(last-use / vector / Region-view journal; block-manifest staging; C's F-known mirror after a rejected TU;
Rejoin `unpack_rejoin` validation; bounded cursor reads).
