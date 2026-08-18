# Ledger cross-check: two independent ledgers converge, per row and per category

local-oracle's direction ledger and mine were written independently. This is the
byte-equivalence check, now against **`211bd585`** (branch
`local-oracle/issue16-routing-lab`), which supersedes `6c4fd89a` by adding native per-TU
category fields — `c_root c_fill c_control f_need f_control`, closing
`c_to_f = c_root + c_fill + c_control` and `f_to_c = f_need + f_control` per TU.

**`211bd585` is adopted as canonical.** My own carry split is retired in favour of it; the
numbers do not move, which is the point of this document.

## Result: identical, at every level checked

range-v3, `--codec z1 --real-pipes`, both builds from the same base with the same flags:

| quantity | M=1 local-oracle | M=1 mine | M=30 local-oracle | M=30 mine |
|---|---:|---:|---:|---:|
| C→F | 1,794,129 | 1,794,129 | 18,277,708 | 18,277,708 |
| F→C | 26,535 | 26,535 | 246,720 | 246,720 |
| `c_root` | 682,111 | 682,111 | 1,296,932 | 1,296,932 |
| `c_fill` | 1,110,303 | 1,110,303 | 16,978,191 | 16,978,191 |
| `c_control` | 1,715 | 1,715 | 2,585 | 2,585 |
| `f_need` | 21,446 | 21,446 | 236,846 | 236,846 |
| `f_control` | 5,089 | 5,089 | 9,874 | 9,874 |

Every cell identical — **and 0 differing rows out of 259 at each width on the five-category
comparison**, not just on the totals. Both gates pass on both sides: local-oracle's
`direction_ok` and `category_ok` 259/259, my `dir_ok` 259/259.

So the convergence now covers the **decomposition**, not only the direction totals. The
Root/Fill/control split reported throughout this lane is confirmed against an independent
implementation.

## Mixed-warmth recut, re-derived on the canonical base

The C→F recut of the mixed-warmth arm was originally derived from my ledger. Re-run
directly on `211bd585`, reading its native per-TU `c_to_f` and classifying warm- vs
cold-served by worker id:

**40 of 40 cells identical** — 8 corpora x {1,2,4,8,30} widths, on both the rebuild's total
C→F and the warm-served C→F. A few rows for illustration:

| corpus | k | mine `p2_cf` | local-oracle | mine `p2_warm_cf` | local-oracle |
|---|---:|---:|---:|---:|---:|
| cereal | 30 | 19,431,287 | 19,431,287 | 115 | 115 |
| range-v3 | 30 | 17,903,379 | 17,903,379 | 48,175 | 48,175 |
| llvm | 30 | 67,207,137 | 67,207,137 | 267,273 | 267,273 |
| godot | 30 | 159,244,315 | 159,244,315 | 341,551 | 341,551 |
| eigen | 1 | 8,632,891 | 8,632,891 | 8,632,891 | 8,632,891 |

The mixed-warmth headline therefore stands unchanged on the canonical base: **route-to-warm
is 13.8x median on C→F against 13.9x duplex**, and the per-TU parallelism price is 49-77 KB.

## Historical note: the one divergence against `6c4fd89a`

Checked earlier against the previous tip, the two ledgers differed in exactly one place,
worth 194 B: on cereal `--repetitions 2 --workers 30 --assignment sticky`, where 84 files
hashed over 30 domains leave some domains opened and never serving a committed TU.
local-oracle attributed the idle relationship's session bytes (29 B C→F = Hello 25 + Done 4;
165 B F→C = the summary Ack); mine parked them in `carry_undirected`. Total wire agreed to
the byte.

local-oracle's is the better treatment — those bytes have a direction, they simply have no
TU to be charged to — which is part of why its ledger is now canonical. `carry_undirected`
was 0 on all 48 width runs and on 63 of 64 rebuild runs, so it remains a useful diagnostic
for exactly when the two formulations can differ, and is worth knowing for a scheduler that
opens domains speculatively.

## What this licenses

Two independently written, independently gated ledgers agreeing byte-for-byte on every row
and every category is a far stronger claim than either ledger's own self-check. The C→F and
Root/Fill/control figures in `CF-SPLIT-30-SLOT.md`, `WIDTH-MMIN.md`, `MIXED-WARMTH.md` and
`REBUILD-WIDTH.md` are confirmed against an independent implementation.

## Files

`ledger-crosscheck/xcheck211.txt` (the `211bd585` comparison),
`ledger-crosscheck/xcheck.txt` (the earlier `6c4fd89a` comparison),
`mixed-warmth/mixedwarm211.tsv` (the recut on the canonical base). Runner
`selector_xcheck211.sh`, recut `selector_mixedwarm_lo211.sh`. Sources: local-oracle
`211bd585:capability/cap_m5_main.cpp` and `cap_m5_stream_main.cpp`, both built clean with
`-Wall -Wextra -Wpedantic -Werror`.
