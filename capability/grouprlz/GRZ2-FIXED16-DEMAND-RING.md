# GRZ2 fixed16 decoder correction: demand-populated bounded ring

Date: 2026-08-18 UTC

Role: mickg10/local-oracle

Scope: decoder storage and replay only; the fixed-112 encoder policy and wire format are unchanged

## Result

The fixed-112 G2 decoder now clears the issue's 500,000,000 B/s single-thread F endpoint on
all 16 retained corpora while replaying every byte exactly.

| coverage | original decoder | corrected decoder |
|---|---:|---:|
| exact retained-wire replays | 16/16 | 16/16 |
| F endpoint passes, all fixed16 rows | 8/16 | **16/16** |
| F endpoint passes, rows with at least 100 TUs | 8/10 | **10/10** |
| lowest corrected three-repetition minimum | — | **548,904,820 B/s** (fmt) |
| complete size/C/F passes, all fixed16 rows | 5/16 | **7/16** |
| complete size/C/F passes, rows with at least 100 TUs | 5/10 | **6/10** |

The last two rows deliberately retain the unchanged encoder and size results. This decoder
correction closes the F-side defect; it does not claim the complete fixed16 development row is
finished.

## What was wrong

The bounded decoder used this initialization:

```cpp
ring.init(true, hist + max(configured_group_cap, first_group_size));
```

`Hist::init` implemented that with `std::vector::assign`. The fixed policy asks for a 1 GiB
history plus a 512 MiB group allowance, rounded to a 2 GiB power-of-two ring. `assign` therefore
zero-filled and populated the entire 2 GiB allocation in every fresh decoder process, including
programs whose complete raw input was only 98–326 MB. The timing included that work.

This explained both symptoms in the first fixed16 ledger:

- one-group programs reported roughly 2.02–2.04 GiB peak RSS;
- their measured F rate was dominated by creating the unused part of the ring, reaching only
  81–258 MB/s on several rows.

## Rejected first correction

The first attempted correction initialized the ring from the first frame's actual live bytes and
grew it as history accumulated. It proved the diagnosis but was not acceptable:

- fmt improved 4.00x, spdlog 6.94x, json 2.60x, re2 7.01x, and leveldb 4.36x;
- large multi-group inputs had to allocate a larger vector and copy live history repeatedly;
- LLVM fell to 0.703 GB/s (0.646x), RocksDB to 0.584 GB/s (0.594x), and Abseil to
  0.584 GB/s (0.557x);
- the old and new vectors overlapped during growth, raising peak RSS to about 3.03 GiB.

All 16 rejected-run replays were exact. The failure was performance and transient memory, not
decoded content. The complete negative run is retained at:

```text
/home/ttuser/grouprlz/retained/grz2g-fixed16-lazy-ring-20260818T0031Z
```

Its ledger SHA-256 is
`9c3528e68ff9dd0f79fb031a984a4ec1e6959e4962483e52327beaf5836cf534`.

## Accepted correction

The accepted implementation preserves the original logical capacity and power-of-two mask, but
backs bounded G2 history with an anonymous read/write mapping rather than a zero-filled vector.

```text
logical ring capacity: unchanged (2 GiB under fixed-112)
address/mask stability: unchanged
ordinary group transition: no allocation and no history copy
physical pages: populated when decoded output first touches them
oversized-TU fallback: allocate a larger mapping and copy only declared live history
wire format: unchanged
encoder parse and entropy choices: unchanged
```

Keeping the stable logical ring is important. Absolute output positions continue to map through
the same mask for the whole ordinary decode, so wrap behavior does not acquire an extra state
transition. Small inputs stop after touching only their actual output pages; long inputs approach
the same resident set as before without paying a separate full-ring zero-fill.

## Performance and resident memory

Rates below are the minimum of three fresh, pinned, single-thread decoder processes. GB/s is
decimal. RSS is the maximum reported by those processes.

| corpus | old F GB/s | new F GB/s | multiple | old RSS GiB | new RSS GiB |
|---|---:|---:|---:|---:|---:|
| LLVM | 1.088 | 1.113 | 1.023x | 2.05 | 2.046 |
| RocksDB | 0.983 | 0.976 | 0.993x | 2.07 | 2.059 |
| DuckDB | 0.689 | 0.714 | 1.037x | 2.06 | 1.914 |
| Abseil | 1.049 | 1.053 | 1.004x | 2.05 | 2.035 |
| OpenCV | 1.346 | 1.334 | 0.991x | 2.04 | 2.043 |
| Godot | 0.630 | 0.632 | 1.002x | 2.20 | 2.115 |
| fmt | 0.105 | 0.549 | 5.232x | 2.04 | 0.150 |
| spdlog | 0.081 | 0.631 | 7.753x | 2.03 | 0.107 |
| catch2 | 0.627 | 1.044 | 1.666x | 2.02 | 0.901 |
| nlohmann-json | 0.221 | 0.756 | 3.426x | 2.04 | 0.297 |
| range-v3 | 0.445 | 1.006 | 2.263x | 2.03 | 0.604 |
| Eigen | 1.433 | 1.408 | 0.982x | 2.03 | 2.030 |
| re2 | 0.091 | 0.731 | 8.015x | 2.02 | 0.117 |
| leveldb | 0.116 | 0.684 | 5.878x | 2.03 | 0.152 |
| simdjson | 0.333 | 0.835 | 2.507x | 2.04 | 0.451 |
| cereal | 0.258 | 0.944 | 3.658x | 2.03 | 0.320 |

On the large rows, the new minimum is 0.982x–1.037x the old minimum, within normal run variation
and well inside the 10% performance-loss limit. On small rows, avoiding the unused population
work yields 2.3x–8.0x gains and saves as much as 1.923 GiB peak RSS.

The ledger's `new_ring_GiB` remains 2.000 by design: that is logical address capacity, not RSS.

## Correctness and behavior gates

The expanded independent gate passes 12/12:

1. complete exact replay;
2. suffix-independent emitted prefixes;
3. strict full decode versus deliberate prefix decode;
4. rejection of non-frame cuts;
5. byte-zero and same-group match visibility;
6. ADD-cap rollback and deterministic retry;
7. repeated wrap over 1.270 GiB with a 1 MiB history;
8. a late oversized TU that grows an already-populated ring from 2 MiB to 8 MiB, reuses byte-zero
   history, then wraps, with exact replay and deterministic retry.

Invariant 8 produced three groups, `src0=512`, `oversized=2`, and `retry_fail=0`. Input and replay
share SHA-256 `34dc8cd44f7dd6075ca5da6e99b0377f570e4fdb48408d6415c89b1749a46f7c`.

The complete fixed16 replay additionally verifies:

- all 16 retained wire sizes and SHA-256 values against the original ledger;
- three fresh decoder measurements per wire;
- one complete output-file comparison and source/replay SHA-256 pair per wire;
- 16/16 exact and 16/16 F endpoint passes.

## Wire identity

The encoder path is unchanged. As a direct check, the frozen `1777470` binary and corrected binary
encoded the same gate input to byte-identical containers:

```text
cae30f801c6b71d9e60bbba6617145fe9bef1cb23fb3a643213c647e4ab392bf
```

The fixed16 replay reused the original 16 containers rather than generating replacements. Every
container hash matches the original ledger.

## Provenance

```text
original measured binary SHA-256:
  05c71c65d25afad331ac95d084639ea86489c3b31b440b94b95e3dc935f31621

corrected source SHA-256:
  1f6beb2078b172031f0b24409de44a9608c12c6c2ef79f2959c6e8aa83cb1ed9
corrected measured binary SHA-256:
  aed6de1790f9ff115f09e422b44c067fa632f8bc5412a7ee50bd2f8397b2f888
expanded gate SHA-256:
  a7ca647b5b5d05c9fae2dc71920747f13725db0ee166ca7d1e0d7dfe5b1e0be9

original fixed16 ledger SHA-256:
  b6fa4b32d38494c39b88ccaec76d579a4e6bce6675d57e3367084f1a5e211ea4
corrected decoder ledger SHA-256:
  d1472b0f2e244ed22f232e7c938e779ec475757aa76aef085f9a3407641b6dc9
```

Build:

```bash
g++ -O3 -march=native -std=c++23 -Wall -Wextra -Wpedantic -Werror \
    -I /home/ttuser/libbsc -c grz2g.cpp -o grz2g.o
g++ grz2g.o /home/ttuser/grouprlz/libbsc.a -lzstd -lpthread -o grz2g
```

Retained quietbox2 directories:

```text
original fixed16 run:
  /home/ttuser/grouprlz/retained/grz2g-fixed16-1777470-20260818T0000Z
rejected lazy-grow run:
  /home/ttuser/grouprlz/retained/grz2g-fixed16-lazy-ring-20260818T0031Z
accepted demand-ring replay:
  /home/ttuser/grouprlz/retained/grz2g-fixed16-demand-ring-20260818T0045Z
expanded 12/12 gate:
  /home/ttuser/grouprlz/retained/grz2g-demand-ring-gate-20260818T0055Z
```

Repository artifacts:

- `g2-fixed112-fixed16.tsv`: original full size/C/F fixed16 ledger;
- `g2-fixed16-demand-ring.tsv`: corrected decoder comparison ledger;
- `g2_replay_fixed16.sh`: retained-wire replay runner;
- `gate_grz2.py`: independent gate including late oversized growth;
- `grz2g.cpp`: corrected bounded-history backing.

## Remaining fixed16 work

This correction removes F decode as a fixed16 blocker. Four of the ten at-least-100-TU rows still
prevent the complete size/C/F matrix from passing:

| corpus | remaining result |
|---|---|
| DuckDB | C encode 0.827 GB/s |
| Godot | C encode 0.367 GB/s |
| simdjson | C encode 0.957 GB/s |
| Eigen | wire size 1.644x whole-program z19 |

The next research step is therefore not more decoder tuning. It is the already-identified
encoder/selection work: establish a non-hindsight selection rule on broader held-out corpora and
measure whether the alternative P29 path can cover the Eigen/Godot shape without sacrificing the
fixed-112 chronology and memory bounds.
