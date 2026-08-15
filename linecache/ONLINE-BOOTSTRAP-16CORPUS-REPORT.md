# Issue #16: balanced 16-corpus exact online phrase bootstrap

## Outcome

The exact Region-program capability experiment now uses one common online policy and one common
512 KiB candidate-vocabulary cap across all 16 corpora. The wire policy is **first profitable use**:
a phrase can be learned only after prior TUs, but its definition is not sent until a later TU can
repay that definition in the same compressed candidate frame.

All selected rows reconstruct every Region exactly with an independent decoder. Across
28,554,671,510 raw `.ii` bytes, the selected rows transfer 59,706,705 charged structural bytes.

The primary acceptance view gives every corpus one vote:

| statistic | exact Region-program result |
|---|---:|
| equal-corpus ratio | **361.94x** |
| minimum | 122.17x |
| lower quartile | 271.85x |
| median | 513.67x |
| maximum | 2,507.23x |
| corpora at or above 400x | 9 / 16 |
| structural H200 at or before 50% | 10 / 16 |
| H200 unavailable because corpus has fewer than 64 TUs | 2 / 16 |

The equal-corpus ratio is the harmonic mean of the 16 per-corpus ratios. Equivalently, it gives
each corpus one equal-sized unit of raw input before summing wire fractions. The byte-weighted
aggregate is **478.25x**, but it is secondary: large expanded traces must not hide the lower tail.

This remains a capability-layer result, not a complete cold-400 result. It reconstructs exact
Region identities. Line definitions, typed values, residual literals, closure traffic, missing
object exchanges, and production framing are not yet present. The Python harness is also an
executable definition, not the >=1 GB/s implementation.

## The policy correction

The earlier policy sent every promoted definition immediately after the TU that caused promotion.
That spent bytes based on an estimated future return. The new policy separates learning from wire
publication:

1. Encode and independently decode TU `t` from state committed through `t-1`.
2. Observe `t` only after it has decoded exactly.
3. Promote eligible exact phrases into bounded C-side candidate vocabulary.
4. On a later TU, build both candidates from the same prior dynamic state:
   - frozen/empty baseline;
   - online parse plus every still-unpublished phrase definition used by that parse.
5. Compress both candidates with the real frame codec and include selectors, lengths, and
   definition frames in the comparison.
6. Select the online candidate only when its complete current-TU wire is smaller. If selected,
   install exactly those definitions at F; otherwise send nothing and try again on a future use.
7. Commit only the selected encoder state, decode at F, compare the complete Region stream, and
   only then learn the TU.

This rule has no corpus name, future horizon, or fixed definition allowance on the wire. The
512 KiB value now limits the C-side promoted vocabulary retained by this capability learner; it is
not a promise to transmit 512 KiB. Across the selected rows, 222,750 phrases were promoted but only
114,847 were published: **51.56%**. Every publication was repaid by the TU that first carried it.

## Balanced policy comparison

The fair comparison fixes every corpus at 512 KiB and changes only publication timing:

| metric | publish at promotion | first profitable use | change |
|---|---:|---:|---:|
| byte-weighted aggregate | 471.00x | **478.25x** | +1.54% |
| equal-corpus ratio | 349.99x | **361.94x** | +3.42% |
| minimum | 119.21x | **122.17x** | +2.48% |
| lower quartile | 263.68x | **271.85x** | +3.10% |
| median | 491.58x | **513.67x** | +4.49% |
| corpora at or above 400x | 8 / 16 | **9 / 16** | +1 corpus |

First profitable use reduces charged bytes on **all 16 corpora**. Per-corpus ratio gains range from
0.30% on Godot to 14.34% on cereal. This makes it a general policy improvement rather than a
single-project tuning result.

The prior `d509e7d` headline selected 128 or 512 KiB separately per corpus and retained an already
running 1 MiB DuckDB point. That 477.52x result remains a historical control, but it is no longer
the primary table because its per-corpus cap selection used hindsight. At the uniform 512 KiB cap,
DuckDB is 288.91x; the 1 MiB result is retained only as a labeled capacity control.

## Complete uniform-cap table

`package+online` means the frozen package plus chronological target-local learning.
`empty+online` means no decoder-visible package at TU 0. RocksDB and OpenCV are package-training
sources and therefore use only empty-start rows in this table. Cereal selects empty start because
it is smaller after paying the complete package frame.

| corpus | selected start | final ratio | cumulative C50 | structural H200 | published / promoted |
|---|---|---:|---:|---:|---:|
| LLVM | package+online | 819.50x | 668.55x | 0.0625 | 11,033 / 17,409 |
| RocksDB | empty+online | 187.89x | 131.65x | 0.3949 | 20,043 / 25,328 |
| DuckDB | package+online | 288.91x | 462.73x | 0.1022 | 9,488 / 14,583 |
| Abseil | package+online | 284.97x | 174.68x | 0.2047 | 9,736 / 15,389 |
| OpenCV | empty+online | 639.18x | 475.23x | 0.0167 | 6,558 / 13,897 |
| Godot | package+online | 751.05x | 995.85x | 0.0230 | 6,959 / 11,369 |
| fmt | package+online | 122.17x | 82.60x | n/a (<64 TUs) | 5,219 / 11,163 |
| spdlog | package+online | 258.73x | 157.27x | n/a (<64 TUs) | 1,692 / 6,904 |
| Catch2 | package+online | 818.99x | 806.73x | 0.0663 | 5,336 / 11,352 |
| nlohmann/json | package+online | 406.54x | 271.56x | 0.6439 | 6,446 / 14,288 |
| range-v3 | package+online | 811.83x | 657.24x | 0.2455 | 6,954 / 13,084 |
| Eigen | package+online | 2,507.23x | 1,768.19x | 0.0980 | 5,404 / 17,235 |
| RE2 | package+online | 336.44x | 212.47x | 0.8584 | 3,892 / 10,025 |
| LevelDB | package+online | 199.16x | 213.17x | 0.8875 | 5,928 / 13,207 |
| simdjson | package+online | 620.81x | 393.75x | 0.3871 | 7,367 / 13,710 |
| cereal | empty+online | 1,203.18x | 706.98x | 0.7563 | 2,792 / 13,807 |

`H200` follows the issue ruling: the earliest raw-corpus fraction whose trailing 5%-of-raw window,
expanded to at least 64 TUs, reaches 200x and remains there for the following 10% of raw bytes.
These values charge the package at TU 0 but still cover only the structural layer.

## Cold empty start versus package start

The package contains 4,476 exact phrases, 248,831 uncompressed definition bytes, and a 79,138-byte
zstd-3 frame. Its SHA-256 is
`8fb47610e74309ea15bd1c0c2080ee07b5931518bb8e638b49ddb2785f322430`.

It was frozen from RocksDB and OpenCV expanded traces. The following comparison therefore uses the
other 14 corpora as held-out capability tests. It does not claim this expanded-output package is the
final portable raw-source artifact.

| corpus | TUs | empty final | package final | package change | permanently smaller from TU | winner |
|---|---:|---:|---:|---:|---:|---|
| LLVM | 1,238 | 780.97x | 819.50x | +4.93% | 6 | package |
| DuckDB | 689 | 271.16x | 288.91x | +6.55% | 4 | package |
| Abseil | 700 | 279.87x | 284.97x | +1.82% | 4 | package |
| Godot | 2,207 | 687.08x | 751.05x | +9.31% | 5 | package |
| fmt | 50 | 112.76x | 122.17x | +8.35% | 4 | package |
| spdlog | 34 | 212.50x | 258.73x | +21.76% | 4 | package |
| Catch2 | 857 | 804.49x | 818.99x | +1.80% | 277 | package |
| nlohmann/json | 99 | 358.47x | 406.54x | +13.41% | 4 | package |
| range-v3 | 259 | 709.99x | 811.83x | +14.34% | 4 | package |
| Eigen | 650 | 1,417.00x | 2,507.23x | +76.94% | 93 | package |
| RE2 | 72 | 281.34x | 336.44x | +19.58% | 6 | package |
| LevelDB | 72 | 174.75x | 199.16x | +13.96% | 4 | package |
| simdjson | 153 | 514.31x | 620.81x | +20.71% | 5 | package |
| cereal | 84 | **1,203.18x** | 1,004.57x | -16.51% | never | empty |

The package wins 13/14 held-out corpora. Empty start remains a required candidate because cereal
never repays the package. Most package wins repay very quickly, but Catch2 and Eigen demonstrate
that package payoff can be delayed even when the final gain is real.

Equal-corpus cold learning checkpoints make the startup debt explicit:

| TU | eligible corpora | empty start | package start | package change | package currently smaller |
|---:|---:|---:|---:|---:|---:|
| 1 | 14 | 26.88x | 0.31x | -98.84% | 0 / 14 |
| 5 | 14 | 52.41x | 53.15x | +1.42% | 9 / 14 |
| 10 | 14 | 77.43x | 85.89x | +10.93% | 11 / 14 |
| 25 | 14 | 143.83x | 163.09x | +13.38% | 11 / 14 |
| 50 | 13 | 216.40x | 243.15x | +12.36% | 10 / 13 |
| 100 | 8 | 311.57x | 340.04x | +9.14% | 7 / 8 |
| 200 | 7 | 384.27x | 412.35x | +7.31% | 6 / 7 |
| final | 14 | 335.17x | **373.50x** | +11.44% | 13 / 14 |

Only corpora that still have a TU at a checkpoint participate in that checkpoint. The final row
again includes all 14. The result supports a charged optional bootstrap candidate, not mandatory
installation.

## What the broad result says to optimize next

Seven corpora remain below structural 400x: fmt, RocksDB, LevelDB, spdlog, Abseil, DuckDB, and RE2.
They do not form one failure mode:

- fmt, spdlog, LevelDB, and RE2 have short amortization horizons. Avoiding premature definition
  publication helps them materially, but fixed power-of-two phrases still leave small-run debt.
- RocksDB and Abseil have distributed payload residuals. Their largest TUs do not dominate the
  total, so a single raw-TU exception or a smaller package does not provide the missing factor.
- DuckDB has some concentrated generated material, but it is only one member of the lower tail and
  no longer determines mechanism choice.

The next structural contender should therefore be a general representation improvement:
variable-length or nested Blocks, followed by actual-byte minimum parsing over those Blocks. It
should first be judged by 16-corpus pass count, minimum, lower quartile, and equal-corpus ratio. A
change that helps only one corpus is diagnostic evidence, not an accepted direction.

The evaluation hierarchy is now:

1. exact independent replay on every corpus;
2. number of per-corpus cold-400 and H200 passes;
3. minimum, lower quartile, and equal-corpus ratio;
4. per-corpus regressions and order/change ranges;
5. total transferred bytes and >=1 GB/s encode/decode throughput.

## Experimental boundary

- Phrase units are exact whole context runs plus overlapping 2/4/8/16/32-Region sequences within
  marker-aligned runs.
- Both endpoints independently assign dense Region IDs by first common observation.
- Online phrase definitions are delta-coded sequences of those IDs, compressed as actual zstd-3
  frames, decoded, and reconstructed into exact phrase keys at F.
- Payloads are actual independent zstd-3 frames. One selector byte and four-byte frame lengths are
  charged.
- Candidate document frequency uses a deterministic bounded Space-Saving table.
- Phrase IDs and decoder-visible definitions are immutable once published.
- The fixed package is charged independently for each corpus that selects it.

The present summary must not be combined numerically with Line/value results from another harness.
The production gate requires one integrated ledger containing package, Lines, Blocks, Root/TU
programs, typed values, residual literals, online definitions, selectors/framing, and any
missing-object exchanges.

## Ordering and change coverage

The earlier 512 KiB single-corpus controls remain exact:

| input control | ratio under promotion-time policy |
|---|---:|
| manifest order | 286.18x |
| one deterministic shuffle | 315.31x |
| replacement of the most shared Region | 286.62x |

Those controls show that one tested reorder and one content replacement do not break replay, but
they are not broad stability evidence. The first-use policy still needs multiple fixed shuffles,
reverse and scheduler-like order, delayed learning batches, and change/recovery/revert sequences.
Screen these on short, distributed-residual, and large corpora, then run the accepted candidate on
all 16.

## Performance and state

The Python runner repeatedly clones phrase indexes and scans candidate sets. Its measured effective
rates are far below the product gate and should not be used as an implementation estimate. The C++
implementation needs only the semantics:

1. immutable exact phrase definitions;
2. encode-before-learn snapshots;
3. bounded C-side candidate learning;
4. a fast matcher over stable dense IDs;
5. first-profitable-use publication using actual encoded bytes;
6. independent exact expansion at F.

The C++ gate remains >=1 GB/s end to end for a full run, including matching, definition assembly,
compression, decoding, and exact reconstruction. Memory reporting must separate C-only candidate
state from decoder-visible state and transferred bytes.

## Next work

1. Integrate this representation into the real C/F codec and produce one exact full-wire ledger.
2. Implement variable-length/nested Blocks as an isolated contender and judge it on the balanced
   acceptance hierarchy above.
3. Build a portable raw-source package and valid leave-one-out package controls for RocksDB and
   OpenCV; retain empty start as an actual candidate.
4. Run the broad order/change matrix and compute H200 from complete wire, not structural wire.
5. Port only contenders that preserve the >=1 GB/s product gate.

## Reproduction and retained evidence

The runner is `linecache/online_bootstrap_curves.py`. The primary switch is now
`--publication first-use`; `--publication promotion` retains the old control. It can load or train a
package, execute empty/package rows, emit per-TU JSON/TSV curves, shuffle or perturb the target, and
compute C50/H200.

`linecache/summarize_online_bootstrap.py` accepts split or complete row reports for each corpus,
selects one valid row per corpus, emits the complete ledger, and reports both byte-weighted and
equal-corpus ratios.

`linecache/compare_bootstrap_curves.py` emits the 14-corpus empty/package comparison, permanent
repayment TUs, and common learning checkpoints.

Retained artifacts:

- `linecache/ml-artifacts/online-bootstrap-16corpus.tsv`
- `linecache/ml-artifacts/online-bootstrap-16corpus-summary.json`
- `linecache/ml-artifacts/online-bootstrap-pretraining-14corpus.tsv`
- `linecache/ml-artifacts/online-bootstrap-pretraining-14corpus-summary.json`
- `linecache/ml-artifacts/online-bootstrap-common-rocks-opencv.zst`

Artifact SHA-256 values:

```text
dee698609aadc9406836b3bb7ecd899e91bb112450ab731c177e07f541118faf  online-bootstrap-16corpus.tsv
4912b201e1bb970cf064f8cd7fa7d566772ea75c46ab17c98f6a04b44748a2a6  online-bootstrap-16corpus-summary.json
01ca967df84f0b8b5acdc8bca295cdfcc8f7098068103a84b49e0ca4b7576a2a  online-bootstrap-pretraining-14corpus.tsv
a916cfceeaa807a1d788f3c850fa6c62a3a86cf0972f70eabe45dfbd17c8be40  online-bootstrap-pretraining-14corpus-summary.json
8fb47610e74309ea15bd1c0c2080ee07b5931518bb8e638b49ddb2785f322430  online-bootstrap-common-rocks-opencv.zst
```

Verification performed:

- 16 deterministic event exports complete;
- every selected row exact;
- all 16 first-use rows smaller than their same-cap promotion controls;
- package canonical deserialize/reserialize checks pass;
- 14-corpus empty/package row boundaries match exactly;
- focused first-use and H200 unit tests pass;
- Python compilation and lint pass;
- generated TSV/JSON totals reconcile with source reports;
- `git diff --check` passes.
