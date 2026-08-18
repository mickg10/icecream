# Ledger cross-check: two independent direction ledgers, byte-identical

local-oracle committed a direction split at `6c4fd89a` (branch
`local-oracle/issue16-routing-lab`); mine is `selector_m5_dir2_instrument.py`. The two were
written independently. This is the byte-equivalence check.

**Result: byte-identical, per row and in total, in both directions — with exactly one
enumerable divergence, worth 194 B, that has a clear right answer (local-oracle's).**

## Method

Both built from the same tree with the same flags
(`-O3 -DNDEBUG -march=native -std=c++17 -DICE_LINE_CAP_LOG2=23 -Wall -Wextra -Wpedantic
-Werror`), run on the same corpus with the same arguments.

- local-oracle emits `c_to_f` / `f_to_c` per row plus `direction_ok` (`wire == c_to_f +
  f_to_c`).
- Mine emits `cf_total` / `fc_total` plus `carry_undirected` and `dir_ok`
  (`cf_total + fc_total + carry_undirected == wire`).

Note the premise correction: local-oracle's ledger is **not** a no-carry ledger. It carries
`pending_curve_c_to_f` / `pending_curve_f_to_c` and attributes the carried control bytes by
direction, exactly as my v2 does. That is *why* the two agree.

## range-v3, M=1 and M=30

| M | local-oracle C→F | mine C→F | | local-oracle F→C | mine F→C | |
|---:|---:|---:|:--|---:|---:|:--|
| 1 | 1,794,129 | 1,794,129 | **identical** | 26,535 | 26,535 | **identical** |
| 30 | 18,277,708 | 18,277,708 | **identical** | 246,720 | 246,720 | **identical** |

Not just the totals — **0 differing rows out of 259 at each width**, on both directional
columns. Columns 1-5 and 7-8 (`logical physical worker raw wire cumulative_raw
cumulative_wire`) are byte-identical too; the only column that differs is `latency_ns`,
which is a wall clock.

Both ledgers' own gate columns pass on every row: local-oracle's `direction_ok` 259/259,
my `dir_ok` 259/259.

## The one divergence: daemons opened and never used

Probing the case I already knew produced an undirected residual — cereal, `--repetitions 2
--workers 30 --assignment sticky`, where 84 files hashed over 30 domains leave some domains
opened and never serving a committed TU:

| | local-oracle | mine | delta |
|---|---:|---:|---:|
| wire | 19,829,552 | 19,829,552 | **0** |
| C→F | 19,588,086 | 19,588,057 | **+29** |
| F→C | 241,466 | 241,301 | **+165** |
| `carry_undirected` | — | 194 | |

`29 + 165 = 194`, exactly my undirected residual, and exactly one worker's session bytes:
**29 B C→F** (Hello 25 + Done 4) and **165 B F→C** (the summary Ack). Both ledgers agree on
the total wire to the byte; they differ only in whether an idle relationship's session
bytes get a direction.

**local-oracle's answer is the better one and should be canonical.** Those bytes genuinely
have a direction — an idle daemon's Hello really is C→F — they simply have no TU to be
charged to. My ledger declines to attribute them and surfaces them in
`carry_undirected`; local-oracle's assigns them and keeps `wire == c_to_f + f_to_c` exact.

Magnitude: 194 B in 19.8 MB, **0.001%**. No conclusion in any of the lane documents moves.
`carry_undirected` remains useful as the diagnostic that flags precisely when the two
formulations can differ — it was 0 on 63 of 64 rebuild runs and on all 48 width runs.

## What this licenses

Two independently written, independently gated ledgers agreeing byte-for-byte on every row
is a much stronger statement than either ledger's own self-check. The C→F figures in
`CF-SPLIT-30-SLOT.md`, `WIDTH-MMIN.md`, `MIXED-WARMTH.md` and `REBUILD-WIDTH.md` are
confirmed against an independent implementation, not only against their own invariants.

Recuts should be based on local-oracle's ledger as canonical; the numbers will not change
except for idle-daemon session bytes at the 10^-5 level.

## Files

`ledger-crosscheck/xcheck.txt` (the comparison output). Sources: local-oracle
`6c4fd89a:capability/cap_m5_main.cpp`; mine `selector_m5_dir2_instrument.py` applied to the
same base.
