# issue #16 — implementer harness (interner bake-off + PRODUCTION_FUSED)

Author: mickg10/implementer. Built on **local-oracle's accepted trace interner**
(`trace-interner-bench.cpp`, branch `local-oracle/issue16-trace-interner`, commit `f1892cd`).
Every `*_tail.cpp` here is combined with the **first 655 lines** of that file (the interner) to
produce a self-contained program; the pre-combined self-contained sources are also included
(`production-fused.cpp`, `fused-socket.cpp`, `fcache.cpp`).

Build: `g++ -O3 -march=native -std=c++17 [-pthread] [-lzstd] -o X X.cpp`
Run:   `taskset -c N ./X --manifest <corpus>/manifest.txt` (manifest = one preprocessed `.ii` path/line).
Correctness gate: `distinct` matches (771055 LLVM / 587613 RocksDB / 637610 DuckDB) **and**
`reconstruct(all emitted IDs, in order) == original bytes` (digest). `distinct==` alone is NOT sufficient.

## Machines
- nas642: Intel Xeon Gold 6136 (Skylake-SP, AVX-512; VM, no HW PMU) — 3 GB/s single-thread target.
- quietbox2: AMD EPYC 8124P (Zen4, 32T, AVX-512+VAES) — 10 GB/s single-thread target.

## Interner bake-off (single thread, HOT, pinned) — the losers, for the record
| file | what | nas642 | quietbox2 |
|---|---|---|---|
| `trie-bench2.cpp` | flat 64B radix trie, adaptive fat nodes | 0.15 | — |
| `linehash-bench.cpp` | flat open-addressing line hash | 0.44 | — |
| `swisshash2-bench.cpp -DHASHV=4` | Swiss/F14 + huge pages + inline slot + rapidhash + prefetch pipeline | 0.77 | 1.04 |
| `lean.cpp` | 64-wide AVX-512 tag, lean serial | 0.67 | 1.18 |
| `amac-bench.cpp` | AMAC rolling pipeline (== phase-batched ⇒ not latency-bound) | 0.62 | — |
| `hash-micro.cpp` | per-line hash cost: rapidhash 10.8/8.8ns, FNV 15, mulfold 18, aes1 16 | | |
| `corpus-stats.cpp` | unique-lines / size-class / hot-set concentration | | |

Line-granular dedup caps at the rapidhash throughput (~2.2/2.8 GB/s). The winner is
local-oracle's **trace/span replay** (marker-aligned, exact-verified): **5.9 GB/s nas642 /
14.2 GB/s quietbox2 HOT** — clears both targets (independently reproduced by me).

## PRODUCTION_FUSED (the fused message + F cache-server)
- `production-fused.cpp` — placement-1 (in-process) `KEYSET→MISSING→BODY` + zstd(1/3/6); byte-exact.
  Body 22.3× (L3); dictionary owner is NOT the bottleneck (interner 14 GB/s) — message-varint + zstd are.
- `fused-socket.cpp` — placement-2 (socketpair), byte-exact; the KEYSET is ~half the wire ⇒ true wire ratio ~12×.
- `fcache.cpp` — the real F topology: **single-owner F cache SERVER + ephemeral fork-per-job that
  talks to it over IPC (no COW)**. Pure decode 2.2–2.5 GB/s/thread; single owner scales to ~4.7
  (nas642) / ~5.5 (quietbox2) GB/s aggregate without saturating (read-only warm store, no contention).
  Roles: `--role cacheserver --sock P` and `--role driver --sock P --jobs N`.

Full measurements and rationale: issue #16 comments by mickg10/implementer.
