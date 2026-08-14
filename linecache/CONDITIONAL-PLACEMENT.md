# Conditional skeleton-slot diagnostic

This local-oracle diagnostic starts from implementer commit `860d1c7`, preserving its exact distinct
Line extraction and skeleton/token streams. It asks a narrower question before treating the globally
interned token-placement stream as an entropy floor: how much token predictability remains after
conditioning on the exact skeleton and slot-column position?

This is not an accepted Protocol-50 contender. The empirical frequency row knows the complete target
set and does not charge a model. The previous-value row is a real encoded token-placement stream with
an independent exact decoder, but it has not yet been integrated into the full Line reconstruction or
the two-process codec. These numbers decide whether a full implementation is worth doing.

Build and reproduce:

```text
g++ -O3 -march=native -std=c++17 linecache/definition_codec.cpp \
    linecache/test_defcodec.cpp -o test_defcodec -lzstd
./test_defcodec --manifest MANIFEST --skeleton
```

## Results

| Metric | DuckDB | LLVM |
|---|---:|---:|
| Slot occurrences | 7,202,235 | 4,151,710 |
| Exact `(skeleton,column)` contexts | 3,007,067 | 912,108 |
| Repeated contexts | 263,179 | 244,335 |
| Slots in repeated contexts | 61.9% | 83.9% |
| Empirical entropy within repeated contexts | 3.441 bits/slot | 3.316 bits/slot |
| Hindsight top-1 coverage within repeated contexts | 48.5% | 49.6% |
| Hindsight top-8 coverage within repeated contexts | 67.8% | 71.2% |
| Previous-value hit rate | 47.4% | 50.2% |
| Original global token-ID placement, zstd-3 | 5.030 MiB | 4.322 MiB |
| Exact previous-value placement, zstd-3 | 4.789 MiB | 4.084 MiB |
| Placement reduction | 4.8% | 5.5% |
| Previous-value token round trip | PASS | PASS |

## Interpretation

The original skeleton experiment disproves one representation: a global first-seen token ordinal
stream plus zstd does not materially beat front coding. It does not establish that contextual token
choice is irreducible. Exact repeated contexts retain measurable predictability, and a backed-off
model may cover contexts that occur once in the target but were seen in unrelated training data.

However, the first legal deterministic control is modest: repeating the prior value saves only about
0.24 MiB on either corpus. A learned row earns full-codec implementation only if disjoint-project
pretraining or online project adaptation closes substantially more of the gap after model and escape
bytes are counted.

The next experiment should freeze a small model trained on whole held-out repositories and compare:

```text
no predictor
GENERIC_SOURCE
GENERIC_II
TOOLCHAIN_BASE
generic + strict post-TU online adaptation
```

For a C-only ranker, F still receives an explicit prior base/template and exact residual. For a shared
token table, its package/version and bytes must be reported separately. Every surviving row must then
move into the real two-process codec and retain the complete 1.0 GB/s gate.
