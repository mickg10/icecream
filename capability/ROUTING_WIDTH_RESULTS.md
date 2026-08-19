# Exact C-to-F routing-width and mixed-warmth results

This report supplies the measured width term for the Issue #16 topology and cost model. It is a
codec/cache capability result, not a compiler-makespan result: the endpoints reconstruct every
`.ii` byte exactly through real pipes, but they do not run the compiler.

Commit under test: `3a9e75fd` (with direction/category ledgers from `6c4fd89a` and `211bd585`).

## Binding score and closure

For TU `i` assigned to F cache domain `f`, the binding byte score is

```text
CtoF(i,f) = Root(i,f) + Fill(i,f) + CControl(i,f)
BuildCtoF = sum over every TU and every physical destination socket of CtoF(i,f)
```

Return traffic is retained, but it is not substituted for the score:

```text
FtoC(i,f) = Need(i,f) + FControl(i,f)
Wire(i,f) = CtoF(i,f) + FtoC(i,f)
```

The runner rejects a row unless exact reconstruction, frame closure, direction closure, category
closure, raw-byte closure, physical-wire closure, and TU-count closure all pass. Root, Fill, and
Need must also equal their independent physical frame ledgers. There is no undirected carry.

## Experiment

- Eight corpora: LLVM, RocksDB, DuckDB, Godot, Catch2, range-v3, Eigen, and cereal.
- Fixed cache widths `M = 1, 2, 4, 8, 16, 30`.
- Round-robin assignment, one shared persistent cache per F daemon, real reconstruction pipes.
- zstd-1 for bounded Root/Fill payloads.
- 48 cold runs plus 48 mixed-warmth runs.
- Mixed warmth runs pass one over F0 only, retain F0, introduce `M-1` empty F caches, then route
  pass two round-robin over all M destinations. The pass-two score is split by actual destination
  into warm-F and cold-F C-to-F bytes.
- Built with `-O3 -DNDEBUG -march=native`, exact source/binary/manifest identities retained.

All 96 runs passed every exactness and accounting gate.

## Cold width: exact physical C-to-F bytes

Decimal MB; the parenthesized value is the multiplier over the same corpus at `M=1`.

| corpus | M=1 | M=2 | M=4 | M=8 | M=16 | M=30 |
|---|---:|---:|---:|---:|---:|---:|
| LLVM | 14.440 (1.00x) | 17.724 (1.23x) | 22.898 (1.59x) | 31.604 (2.19x) | 46.207 (3.20x) | 67.341 (4.66x) |
| RocksDB | 14.337 (1.00x) | 16.379 (1.14x) | 19.694 (1.37x) | 25.270 (1.76x) | 35.411 (2.47x) | 51.997 (3.63x) |
| DuckDB | 12.615 (1.00x) | 15.486 (1.23x) | 20.231 (1.60x) | 27.068 (2.15x) | 38.700 (3.07x) | 54.133 (4.29x) |
| Godot | 62.579 (1.00x) | 70.026 (1.12x) | 81.443 (1.30x) | 98.507 (1.57x) | 123.828 (1.98x) | 156.563 (2.50x) |
| Catch2 | 2.304 (1.00x) | 3.434 (1.49x) | 5.380 (2.34x) | 7.865 (3.41x) | 12.091 (5.25x) | 18.791 (8.16x) |
| range-v3 | 1.794 (1.00x) | 2.705 (1.51x) | 4.366 (2.43x) | 6.982 (3.89x) | 11.541 (6.43x) | 18.278 (10.19x) |
| Eigen | 3.246 (1.00x) | 5.526 (1.70x) | 9.819 (3.02x) | 17.030 (5.25x) | 27.836 (8.57x) | 41.510 (12.79x) |
| cereal | 1.109 (1.00x) | 1.856 (1.67x) | 3.163 (2.85x) | 5.738 (5.18x) | 10.923 (9.85x) | 19.970 (18.01x) |

The aggregate is only a convenient scale sum; corpora are separate builds.

| width | aggregate C-to-F | vs M=1 aggregate | Root | Fill | control | Fill share | 1 Gbit/s serialization | 10 Gbit/s serialization |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 112.424 MB | 1.00x | 21.440 MB | 90.939 MB | 0.046 MB | 80.9% | 0.899 s | 0.090 s |
| 2 | 133.136 MB | 1.18x | 24.839 MB | 108.251 MB | 0.046 MB | 81.3% | 1.065 s | 0.107 s |
| 4 | 166.993 MB | 1.49x | 29.151 MB | 137.796 MB | 0.046 MB | 82.5% | 1.336 s | 0.134 s |
| 8 | 220.063 MB | 1.96x | 33.859 MB | 186.157 MB | 0.047 MB | 84.6% | 1.761 s | 0.176 s |
| 16 | 306.537 MB | 2.73x | 37.737 MB | 268.750 MB | 0.049 MB | 87.7% | 2.452 s | 0.245 s |
| 30 | 428.583 MB | 3.81x | 40.483 MB | 388.047 MB | 0.052 MB | 90.5% | 3.429 s | 0.343 s |

At `M=30`, individual corpus multipliers range from 2.50x to 18.01x. Fill is 80.5-96.9% of
the C-to-F score by corpus. Control is negligible. The main width cost is repeated definitions,
with a smaller Root increase because each destination must receive its own first-use Block
definitions and reconstruction context. Identity remains the same direct `(SourceGeneration,
ObjectKind, generation-local u32 ordinal)` on every destination; there is no relationship-local
remap.

## Thirty requested slots: capacity changes the binding width

`M` is the number of independent F cache domains, not the number of compiler slots. For uniform
F capacity `c`, the minimum feasible width is `ceil(30/c)`. Applying the measured rows gives:

| slots exposed by each F | minimum F caches | aggregate C-to-F | 1 Gbit/s serialization | 10 Gbit/s serialization |
|---:|---:|---:|---:|---:|
| 30 | 1 | 112.424 MB | 0.899 s | 0.090 s |
| 8 | 4 | 166.993 MB | 1.336 s | 0.134 s |
| 4 | 8 | 220.063 MB | 1.761 s | 0.176 s |
| 1 | 30 | 428.583 MB | 3.429 s | 0.343 s |

These are serialization floors on one C egress, not build elapsed time. Wider placement is useful
only if its reduction in real compiler makespan repays the added Root/Fill transfer and cache work.
An actual farm row must use the published heterogeneous per-F capacity vector, not assume one slot
per daemon.

## Mixed warmth: one warm F cannot warm newly recruited Fs

Pass-two exact C-to-F bytes after pass one warmed only F0:

| corpus | M=1 | M=2 | M=4 | M=8 | M=16 | M=30 |
|---|---:|---:|---:|---:|---:|---:|
| LLVM | 8.553 MB | 14.973 MB | 21.912 MB | 31.226 MB | 46.019 MB | 67.207 MB |
| RocksDB | 8.595 MB | 13.605 MB | 18.562 MB | 24.865 MB | 35.339 MB | 52.483 MB |
| DuckDB | 3.198 MB | 10.374 MB | 17.026 MB | 25.453 MB | 37.711 MB | 53.637 MB |
| Godot | 10.816 MB | 31.142 MB | 53.868 MB | 96.183 MB | 123.319 MB | 159.244 MB |
| Catch2 | 0.501 MB | 1.893 MB | 4.099 MB | 7.089 MB | 11.556 MB | 18.187 MB |
| range-v3 | 1.384 MB | 2.322 MB | 3.920 MB | 6.475 MB | 10.966 MB | 17.903 MB |
| Eigen | 8.633 MB | 10.304 MB | 13.217 MB | 18.430 MB | 27.995 MB | 41.693 MB |
| cereal | 0.038 MB | 1.087 MB | 2.578 MB | 5.251 MB | 10.452 MB | 19.431 MB |

At `M=30`, pass two costs 429.786 MB in aggregate. Only 1.378 MB (0.32%) goes to TUs routed back
to warm F0; 428.409 MB goes to TUs routed to the 29 cold destinations. That total is within 0.3%
of the fully cold `M=30` aggregate. Warmth is local to an F cache domain; assigning a TU to a new F
does not inherit another F's resident definitions.

The consequence is concrete: a scheduler should retain a small capacity-sufficient home set and
return compatible work to the F whose cache already has the material. Recruiting an empty F is a
new-cache opening event whose predicted definition cost belongs in the scheduling decision.

## What this measurement proves, and what it does not

It proves:

1. physical C-to-F direction and Root/Fill/control attribution close exactly on every TU;
2. cache-domain count is a first-order transfer variable;
3. repeated Fill dominates wide cold placement;
4. one warm destination does not make other destinations warm;
5. fixed capacity can convert the 30-slot request into a much narrower binding cache width.

It does not yet prove:

1. the best capacity-aware assignment on a real scheduler-ready trace;
2. compiler makespan at each width;
3. the per-width information lower bound after charging one Root and one compressed definition
   union per selected F;
4. how much Fill can be reduced by confirmed-resident external references, different immutable
   object boundaries, or batched missing-object bodies;
5. restart, eviction, spill, and changed-header rows.

Those are the next acceptance rows. They must retain the same physical-direction and per-TU closure.

## Retained evidence

Remote canonical artifact root:

```text
/home/ttuser/issue16-routing-localoracle-3a9e75fd/z1-8corpus
```

It contains all 96 logs, all 96 per-TU curves, TSV matrices, JSON provenance, build command, binary,
and a complete `SHA256SUMS`. `sha256sum -c SHA256SUMS` passes.

Compact local mirror:

```text
/tanksmall/scratch/ictmp/issue16-routing-width-z1-3a9e75fd
```

Key hashes:

```text
12c75b7b07770e6995d99f5d4890b854e2dc35fef5d458704c73a23db1e02341  cold-width.tsv
d05c441d87c965acd2354ba2ec9c1790fe4e29aba48bd142e3693254ec4d12ec  mixed-warmth.tsv
a51bf101d40d430c6eaa27a884a4fab10f0b04be4edc844b6fcdbbb813b6e792  capacity-width.tsv
5d28c105438a4a8b0a3036303c1614a3638e13a0dd85526df59328c3e1e4e651  matrix.json
6cb231414f90a792c9c0f882040d8a9d7785dce17ad53b9c8f8de07884085a34  SHA256SUMS
```

## Decision from this row

Keep the architecture as one canonical C object state plus one independent resident-state view per
selected F cache. Do not create one semantic C store per F, and do not model independent F caches as
one shared resident set. First minimize the capacity-sufficient F set; then reduce the definition
union that must be installed on each remaining F. A more elaborate transfer mechanism should be
accepted only if it wins exact scheduled replay by enough to matter after this routing correction.
