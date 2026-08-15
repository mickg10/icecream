# Issue #16: exact online phrase bootstrap across 16 corpora

## Outcome

The exact Region-program capability layer now has a complete 16-corpus result instead of a
single-project result.

Across 28,554,671,510 raw `.ii` bytes, the best measured exact online row for each corpus transfers
59,798,221 bytes, or **477.52x**.  This aggregate is deliberately conservative about the bootstrap
package: every corpus that selects it pays its complete 79,138-byte frame again.  All selected rows
decode their complete Region sequence exactly with an independent decoder.

The distribution matters more than the aggregate:

| statistic | selected exact Region-program ratio |
|---|---:|
| minimum | 121.30x |
| lower quartile | 266.05x |
| median | 492.63x |
| maximum | 2,205.21x |
| corpora at or above 400x | 8 / 16 |
| structural H200 at or before 50% | 10 / 16 |
| H200 unavailable because the corpus has fewer than 64 TUs | 2 / 16 |

This is a strong superblock capability result, but it is **not** a complete cold-400 result.  The
wire here reconstructs exact Region identities, not complete `.ii` bytes: Line definitions,
literal/value residuals, missing-object traffic, and the complete production framing still have to
be integrated and charged.  The Python harness also does not meet the 1 GB/s product gate.

## Experimental boundary

- Phrase units are exact 2/4/8/16/32-Region sequences within marker-aligned runs, plus exact whole
  runs as candidates.
- The online learner starts empty, scores TU `t` from state committed through `t-1`, and observes
  `t` only after exact encoding and decoding.
- Candidate document frequency uses a deterministic bounded Space-Saving table.  The primary row
  promotes at the second observed TU, ranks by estimated byte return, publishes immutable phrases,
  and has an 8 KiB per-TU publication limit.
- Both endpoints independently assign dense Region IDs by first common observation.  Online phrase
  definitions are delta-coded sequences of these IDs, compressed as actual zstd-3 frames, decoded,
  and reconstructed into exact phrase keys at F.
- Payloads are actual independently compressed zstd-3 frames.  A one-byte selector and four-byte
  frame lengths are charged.
- The fixed bootstrap package contains 4,476 exact phrases, 248,831 uncompressed definition bytes,
  and 79,138 charged zstd-3 bytes.  Its SHA-256 is
  `8fb47610e74309ea15bd1c0c2080ee07b5931518bb8e638b49ddb2785f322430`.
- That package was frozen from RocksDB and OpenCV expanded traces.  It is therefore a valid held-out
  capability control for the other 14 corpora.  RocksDB and OpenCV use only the empty-start row in
  the headline table.
- The package is an expanded-output capability bound, not the final portable raw-source package.

## Complete corpus table

`pre+online` means the frozen package plus the same chronological online learner.  `empty+online`
means no package and no target-local phrases at TU 0.  The selected budget is the best of the
measured 128/512 KiB points, except DuckDB where the already-running coherent 1 MiB control finished
before the experiment moved to all corpora.  This per-corpus budget selection is an oracle over a
small fixed sweep; a runtime result needs the marginal-return stopping rule described below.

| corpus | TUs | selected budget | selected row | final ratio | cumulative C50 | structural H200 |
|---|---:|---:|---|---:|---:|---:|
| LLVM | 1,238 | 512 KiB | pre+online | 796.74x | 651.28x | 0.0625 |
| RocksDB | 622 | 512 KiB | empty+online | 186.64x | 131.00x | 0.3958 |
| DuckDB | 689 | 1 MiB | pre+online | 321.09x | 581.75x | 0.1022 |
| Abseil | 700 | 512 KiB | pre+online | 283.03x | 173.59x | 0.2047 |
| OpenCV | 1,506 | 512 KiB | empty+online | 634.04x | 471.16x | 0.0339 |
| Godot | 2,207 | 512 KiB | pre+online | 748.79x | 988.85x | 0.0230 |
| fmt | 50 | 128 KiB | pre+online | 121.30x | 81.75x | n/a (<64 TUs) |
| spdlog | 34 | 128 KiB | pre+online | 249.08x | 151.28x | n/a (<64 TUs) |
| Catch2 | 857 | 512 KiB | pre+online | 800.04x | 774.58x | 0.0663 |
| nlohmann/json | 99 | 128 KiB | pre+online | 388.16x | 262.68x | 0.6439 |
| range-v3 | 259 | 512 KiB | pre+online | 779.41x | 604.43x | 0.2455 |
| Eigen | 650 | 512 KiB | pre+online | 2,205.21x | 1,596.26x | 0.0980 |
| RE2 | 72 | 128 KiB | pre+online | 323.12x | 203.37x | 0.8584 |
| LevelDB | 72 | 128 KiB | pre+online | 192.78x | 209.66x | 0.8875 |
| simdjson | 153 | 512 KiB | pre+online | 597.10x | 373.74x | 0.3871 |
| cereal | 84 | 128 KiB | empty+online | 1,117.05x | 672.65x | 0.7563 |

`H200` follows BigOracle's issue ruling: the earliest raw-corpus fraction whose trailing 5%-of-raw
window, expanded to at least 64 TUs, reaches 200x and remains there for the following 10% of raw
bytes.  The table's H200 is structural only.  A pass does not replace the separate final-400 gate:
for example, RocksDB and DuckDB cross a good early window but later difficult material lowers their
final ratio.

## What the broader data changes

### A mandatory bootstrap package is wrong

The package improves 13 of its 14 valid held-out corpora, often substantially, but cereal is smaller
from an empty start: 1,117.05x versus 946.07x at the selected 128 KiB online budget.  Package bytes
must therefore be a charged candidate.  A C/F generation should install a package only after its
expected savings repay the package frame; a generic package must never be assumed free.

### One fixed online budget is also wrong

For the six short or near-threshold corpora, reducing the online budget from 512 KiB to 128 KiB
improved every selected row:

| corpus | 512 KiB | 128 KiB | 64 KiB | best measured |
|---|---:|---:|---:|---:|
| fmt | 119.21x | **121.30x** | 119.14x | 128 KiB |
| spdlog | 244.34x | **249.08x** | 241.85x | 128 KiB |
| nlohmann/json | 386.06x | **388.16x** | 338.33x | 128 KiB |
| RE2 | 310.05x | **323.12x** | 318.30x | 128 KiB |
| LevelDB | 190.30x | **192.78x** | 186.59x | 128 KiB |
| cereal, empty start | 1,052.26x | **1,117.05x** | 1,075.97x | 128 KiB |

The product rule should not use a project name or a fixed byte cap.  Each proposed phrase should
carry its actual compressed definition cost and a conservative future saved-byte estimate.  Stop
publishing when the lower-bound remaining return no longer repays the definition plus selector and
closure cost.  The fixed budgets remain benchmark controls around that online stopping rule.

### The low tail has several causes

| corpus | repeated Region occurrences | largest TU share of wire | top-five TU share | payload share | model share |
|---|---:|---:|---:|---:|---:|
| fmt | 85.83% | 19.42% | 48.69% | 88.48% | 6.92% |
| LevelDB | 92.31% | 14.59% | 38.77% | 80.59% | 10.47% |
| RocksDB | 87.19% | 3.25% | 13.09% | 99.50% | 0% |
| Abseil | 94.02% | 2.61% | 10.54% | 98.34% | 0.87% |
| DuckDB | 92.71% | 19.81% | 37.86% | 97.90% | 1.14% |

- fmt and LevelDB are short-run amortization and concentrated-TU problems.
- RocksDB and Abseil have distributed payload residuals; a smaller package cannot provide the
  missing factor.
- DuckDB has a separate generated-data outlier.  `utf8proc.cpp.ii` alone contributes about 1.37 MB
  of the structural wire.  A raw zstd-3 TU fallback would save an estimated 1,041,616 bytes there,
  raising the 512 KiB estimate from 286.18x to about 336.73x.  This remains an estimate until a mixed
  receiver path independently reconstructs the raw TU and deliberately leaves its objects out of
  the shared cache state.

### Exact whole-run union was a useful negative control

A three-way actual-frame comparison among frozen phrases, online phrases, and complete context runs
selected the whole-run candidate 0/689 times at both 512 KiB and 1 MiB on DuckDB.  It produced
identical totals and was removed from the harness.  Carrying that extra branch is not justified.

## Ordering and change controls completed so far

At the 512 KiB DuckDB point:

| input | pre+online ratio | exact |
|---|---:|---|
| manifest order | 286.18x | yes |
| deterministic shuffle | 315.31x | yes |
| replacement of the most widely shared Region | 286.62x | yes |

The tested content change is effectively neutral, and the one shuffle improves rather than harms
the result.  This is evidence, not the complete stability gate.  Still required are multiple fixed
seeds, reverse order, scheduler-like order, delayed feedback batches, change/recovery/revert loops,
and median/range reporting.

## Performance and state

The Python code is an executable definition of the codec semantics, not a product implementation.
It repeatedly clones phrase indexes and scans candidate sets.  Complete runs are far below the
required 1 GB/s encode rate; Godot's four-row capability run took about 1,311 seconds.  Logical
learner state reaches tens of MiB on large corpora, while Python process RSS is much larger.

The C++ implementation should retain only the semantic requirements:

1. immutable exact phrase definitions;
2. encode-before-learn snapshots;
3. a bounded frequency/return estimator;
4. a fast phrase matcher over stable dense IDs;
5. actual-byte selection between phrase and fallback frames;
6. independent exact expansion at F.

It should not copy the Python object layout or package-cloning strategy.

## Next work chosen from the 16-corpus evidence

1. Integrate the representation into the real C/F codec and charge Line definitions, Block closure,
   Root programs, values/literals, requests, replies, selectors, and framing.  Until that exists,
   aggregate structural 477.52x is not total 477.52x.
2. Replace fixed publication budgets with the conservative marginal-return stopping rule, then
   rerun the 16-corpus table without per-corpus hindsight selection.
3. Build valid leave-one-out packages for RocksDB and OpenCV and a portable raw-source package.  The
   present expanded-output package is a capability bootstrap only.
4. Attack distributed payload residuals using variable-length/nested Blocks and whole-TU minimum
   actual-wire parsing; judge changes on RocksDB, Abseil, and the lower quartile, not DuckDB alone.
5. Add an exact raw-TU fallback that intentionally performs no cache publication for that TU.  This
   targets generated tables without forcing their mostly unique Regions through the object stream.
6. Run the full order/change matrix and compute full-wire H200, not only the structural version.
7. Port only contenders that preserve at least 1 GB/s end-to-end throughput.

## Reproduction and retained evidence

The runner is `linecache/online_bootstrap_curves.py`.  It can train or load an exact package, execute
empty/frozen/online rows, emit full per-TU JSON/TSV curves, shuffle or perturb the target, and now
computes C50 and H200.  `--row-set empty` avoids wasting time on an invalid pretrained row during a
leave-one-out run.

`linecache/summarize_online_bootstrap.py` turns named report JSON files into the retained compact
16-corpus TSV and JSON summary.  Large deterministic event traces remain ignored build artifacts.

Retained summary artifacts:

- `linecache/ml-artifacts/online-bootstrap-16corpus.tsv`
- `linecache/ml-artifacts/online-bootstrap-16corpus-summary.json`
- `linecache/ml-artifacts/online-bootstrap-common-rocks-opencv.zst`
- TSV SHA-256: `94adb4b0aa2fe9e973e2f1ff29d90fb25cd4b6dd6180fd576f2ffaf419c72463`
- summary JSON SHA-256: `83e66ba62d4a86efa81be6bd59d78f0f66e4c377527754c60f8daf4a32bdfc90`

Verification performed:

- 16 deterministic event exports complete;
- every selected row reports independent exact Region replay;
- package canonical deserialize/reserialize checks pass on every pretrained run;
- Python compilation passes;
- generated TSV/JSON totals match source report totals;
- `git diff --check` passes.
