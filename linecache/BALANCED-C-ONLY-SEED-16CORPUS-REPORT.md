# C-only bootstrap seed: exact 16-corpus startup screen

Issue: `mickg10/icecream#16`

Evidence branch: `local-oracle/issue16-c-only-seed`

## Outcome

The target-disjoint pretrained phrase set is substantially more useful when it exists
only in C's candidate vocabulary than when it is installed wholesale at F before the
first translation unit.

Across the first `min(200, complete corpus)` TUs of all 16 corpora:

| metric | empty online | installed package + online | C-only seed + online |
|---|---:|---:|---:|
| charged structural wire | 27,594,704 B | 25,999,211 B | **24,897,754 B** |
| byte-weighted ratio | 261.19x | 277.22x | **289.48x** |
| equal-corpus harmonic ratio | 266.84x | 290.86x | **316.40x** |
| C-only seed change | - | **+8.78%** | **+18.57% vs empty** |
| C-only seed strict endpoint wins | - | **15 / 16** | **16 / 16** |
| independently exact rows | 16 / 16 | 16 / 16 | 16 / 16 |

The screen covers 7,207,461,456 raw `.ii` bytes. C-only seeding removes 2,696,950
structural bytes relative to empty learning and 1,101,457 bytes relative to an
installed package. The byte-weighted gains are +10.83% and +4.42%, respectively.

The result is broad. All 16 corpus endpoints are strictly smaller than empty learning.
Fifteen are smaller than installed pretraining. Eigen is the single installed-package
win at TU 200: seed-only is 1,326.48x versus installed 1,435.77x, while still beating
empty's 1,130.07x by 17.38%.

This changes the design conclusion from “send an optional startup package” to “seed
the existing C-side candidate store and let ordinary first-profitable-use publication
decide what F ever sees.”

## Acceptance boundary

This is the exact **Region-program structural layer**, not complete `.ii` transfer.
Every row reconstructs every Region sequence independently, but the measurements do
not yet charge:

- first-use Line text;
- Region-to-Line composition;
- typed values and literal residuals;
- missing-object requests and replies;
- the final production message layout and framing.

Consequently, 316.40x is not a total cold ratio and cannot be compared directly with
the requested complete cold-400 target.

The best full-run cross-context structural hybrid remains 462.92x equal-corpus and
611.58x byte-weighted. Adding only the separately measured Line-text leg yields an
optimistic, still incomplete 183.49x equal-corpus cold result and 262.80x half-cold
result. Therefore:

| requirement | current evidence | status |
|---|---:|---|
| complete cold >=400x | optimistic incomplete 183.49x | not met |
| complete half-cold >=200x | optimistic incomplete 262.80x | provisional only |
| chronological online learning | exact C-only seed learner | met for this layer |
| reorder/change stability | prior hybrid matrix positive; seed-only matrix pending | open |
| complete-path >=1 GB/s | Python is executable semantics only | open |

## Exact state machine

### Persistent state

C retains:

1. a target-disjoint static seed in the existing candidate vocabulary;
2. the bounded chronological target learner;
3. the append-only package of definitions actually published to F;
4. dense Region IDs derived from completed exact TUs;
5. the ordinary dynamic encoder state.

F retains:

1. only the append-only definitions it has actually received;
2. dense Region IDs independently derived from reconstructed TUs;
3. the ordinary dynamic decoder state.

F does **not** receive the seed at startup and does not run a duplicate learner.

### Per-TU transition

For TU `t`:

1. C starts from state committed through TU `t-1`.
2. C encodes the empty/frozen baseline candidate.
3. C encodes seed+online candidates from the same pre-TU state.
4. For each candidate, C includes the actual compressed bytes for every definition
   not yet installed at F, the payload frame, four-byte lengths, and selector bytes.
5. C selects the smallest complete current-TU representation. There is no future
   horizon or project-name rule.
6. If the baseline wins, no candidate definition is published.
7. If a seed/online candidate wins, C sends its exact missing definitions through the
   ordinary definition block, followed by the selected payload.
8. F installs those immutable definitions, decompresses the payload, and reconstructs
   the complete exact Region sequence.
9. C and F independently advance their decoder-visible state from that exact result.
10. Only after TU `t` is complete does C expose it to the learner for TU `t+1` and
    later.

This is still the same first-profitable-use rule. A seed phrase is merely an
unpublished C candidate at startup.

### Mixed definition block

Target-learned phrases refer only to Regions observed in earlier TUs, so the previous
definition block could encode them entirely as delta-coded dense Region IDs. A seed
phrase can contain Regions first encountered in the TU that makes the phrase useful.

The version-2 mixed batch therefore chooses independently for each definition:

- representation `0`: phrase length followed by signed-delta dense Region IDs, when
  every Region is already in the shared dense store;
- representation `1`: byte length followed by the canonical exact phrase key, when
  at least one Region has not yet been observed.

The batch retains one version, one definition count, and one zstd frame. It does not
add a new message kind. F validates each reference or exact key, rejects trailing
bytes and duplicates, appends definitions in message order, and then decodes the
payload.

Focused tests cover a winning seed phrase made entirely from first-seen Regions and a
batch containing both dense and exact definitions.

## Balanced startup curve

The primary curve is the harmonic mean of per-corpus ratios, giving every eligible
corpus one vote. The eligible count is explicit because short corpora leave the
absolute-TU curve after completion.

| TU | eligible | empty | installed | C-only seed | seed vs empty | seed vs installed |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 16 | 27.43x | 0.36x | **27.43x** | tie | +7,605.71% |
| 5 | 16 | 50.63x | 50.04x | **73.30x** | +44.77% | +46.48% |
| 10 | 16 | 74.26x | 80.89x | **107.12x** | +44.24% | +32.42% |
| 25 | 16 | 133.75x | 148.98x | **181.81x** | +35.93% | +22.03% |
| 50 | 15 | 196.20x | 216.01x | **250.80x** | +27.83% | +16.11% |
| 100 | 10 | 241.96x | 256.50x | **278.72x** | +15.19% | +8.66% |
| 200 | 9 | 285.31x | 301.01x | **315.24x** | +10.49% | +4.73% |
| per-corpus endpoint | 16 | 266.84x | 290.86x | **316.40x** | **+18.57%** | **+8.78%** |

TU 1 is a 16/16 tie between seed and empty, not a seed win: the actual-byte selector
publishes no seed definition. The installed package is poor at TU 1 because its full
79-82 KiB frame is charged before any use.

At TU 5, C-only seed is strictly smaller than both alternatives on 15/16 corpora.
Eigen is the sole early exception. It becomes permanently smaller than empty at TU
126 but does not become permanently smaller than installed by TU 200.

For 14 corpora the seed is permanently smaller than empty beginning at TU 2. Cereal
crosses permanently at TU 46. The seed's final Cereal win is intentionally tiny:
1,203.58x versus 1,203.18x. This is useful evidence that the actual-byte fallback can
avoid the installed package's earlier 16.51% Cereal regression without forcing seed
use.

## Per-corpus screen

| corpus | TUs | empty | installed | C-only seed | seed/empty | seed/installed |
|---|---:|---:|---:|---:|---:|---:|
| LLVM | 200 | 438.71x | 479.13x | **504.26x** | +14.94% | +5.24% |
| RocksDB | 200 | 107.44x | 108.71x | **110.12x** | +2.49% | +1.29% |
| DuckDB | 200 | 390.64x | 449.98x | **472.94x** | +21.07% | +5.10% |
| Abseil | 200 | 136.91x | 141.66x | **144.39x** | +5.47% | +1.93% |
| OpenCV | 200 | 248.73x | 268.45x | **287.75x** | +15.69% | +7.19% |
| Godot | 200 | 688.98x | 745.48x | **875.28x** | +27.04% | +17.41% |
| fmt | 50 | 112.76x | 122.17x | **130.85x** | +16.05% | +7.10% |
| spdlog | 34 | 212.50x | 258.73x | **323.06x** | +52.03% | +24.86% |
| Catch2 | 200 | 469.72x | 462.88x | **550.67x** | +17.23% | +18.97% |
| nlohmann/json | 99 | 358.47x | 406.54x | **453.12x** | +26.41% | +11.46% |
| range-v3 | 200 | 622.03x | 709.64x | **795.00x** | +27.81% | +12.03% |
| Eigen | 200 | 1,130.07x | **1,435.77x** | 1,326.48x | +17.38% | -7.61% |
| RE2 | 72 | 281.34x | 336.44x | **436.32x** | +55.08% | +29.69% |
| LevelDB | 72 | 174.75x | 199.16x | **222.04x** | +27.06% | +11.49% |
| simdjson | 153 | 514.31x | 620.81x | **690.18x** | +34.20% | +11.17% |
| cereal | 84 | 1,203.18x | 1,004.57x | **1,203.58x** | +0.03% | +19.81% |

RocksDB and Abseil remain the important lower structural tail. C-only seeding improves
their startup bytes, but by only 2.49% and 5.47% versus empty. It is a bootstrap
improvement, not a solution to their distributed residual.

## Target-disjoint cross-fit

The original exact phrase package was trained on RocksDB + OpenCV, so it cannot score
those two targets. A reciprocal package trained on LLVM + Godot supplies their seed:

| package | training traces | scored targets | phrases | raw / compressed | SHA-256 |
|---|---|---|---:|---:|---|
| A | RocksDB + OpenCV | other 14 corpora | 4,476 | 248,831 / 79,134 B | `8fb47610e74309ea15bd1c0c2080ee07b5931518bb8e638b49ddb2785f322430` |
| B | LLVM + Godot | RocksDB + OpenCV | 4,557 | 248,586 / 82,083 B | `4a2950b18807e6981e87cbacb28311e5789c089fbc630669eae9ebb9a4c504a6` |

Every target is disjoint from the package used to score it. Both packages are now
committed so the experiment has no `/tmp` model dependency.

This remains a two-package cross-fit capability baseline. It is not a claim that one
portable package is ready. The eventual package should be derived from portable raw
source and tested across target projects and toolchains without target overlap.

## State and publication accounting

The seed artifact is C-local and is therefore reported separately rather than counted
as C-to-F wire. It is about 249 KiB uncompressed and 79-82 KiB compressed.

The deterministic logical learner state at each screened endpoint ranges from
2,017,646 to 24,292,016 bytes, with a median of 6,500,057 bytes. This is exact
key-and-counter accounting for the research algorithm; it excludes Python allocator
overhead and is not a C++ allocation estimate. The product learner still needs its
own measured memory budget and compact layout.

Only definitions used by a winning current-TU candidate are installed. For example:

- LLVM publishes 1,499 seed phrases and 12,028 total phrases by TU 200;
- RocksDB publishes 1,508 seed phrases and 19,405 total phrases by TU 200;
- Godot publishes 1,041 seed phrases and 6,991 total phrases by TU 200;
- cereal publishes 1,130 seed phrases and 3,340 total phrases by TU 84.

The compressed definition frame can contain seed and target-learned phrases together,
so the actual frame is charged once. The report does not invent a separable compressed
byte attribution for phrases sharing a frame.

## Design conclusion

Retain C-only seeding as one isolated initialization step for the existing C learner:

1. no startup package transfer;
2. no F-side seed or learner;
3. no new message family;
4. ordinary immutable first-use definitions;
5. complete actual-byte comparison against empty on every TU;
6. target-local chronological learning remains the core mechanism.

Do not optimize around Eigen's TU-200 installed-package win until the complete-corpus
and order/change matrices are available. One exception out of 16 does not justify a
second decoder state machine. The empty candidate already bounds regressions.

## Required next gates

1. Run C-only seed to completion on all current corpora and publish equal-corpus,
   byte-weighted, per-corpus, C50, and H200 results.
2. Run reverse order, at least three deterministic shuffles, scheduler-like order,
   widely shared Region replacement, delayed change, and reverted change. Every row
   must replay exactly.
3. Extend the target-disjoint experiment to the implementer's 25-corpus set. Primary
   reporting remains equal-corpus; compiler/toolchain variants must be labeled.
4. Train a portable raw-source seed and compare it against expanded-trace capability
   packages without target overlap.
5. Integrate the retained structural path with Line text, Region composition, values,
   residuals, missing exchanges, selectors, and final framing.
6. Run the real C++ C/F cold and actual half-cold scenarios at >=1 GB/s.

## Standalone evidence site

`linecache/report-site/index.html` is a self-contained publication artifact generated
by `linecache/render_codec_bakeoff_report.py`. It includes:

- the complete objective ledger;
- inline SVG startup and per-corpus gain plots;
- the C/F state and message flow;
- all 16 corpus rows;
- cross-fit package identities;
- exactness and scope boundaries;
- next gates and reproduction commands;
- print CSS verified as a nine-page A4 rendering.

It has no external runtime assets or JavaScript dependency. It can be hosted directly
through ChatGPT Sites or any static file host.

## Retained evidence and reproduction

Committed machine artifacts:

- `linecache/ml-artifacts/online-bootstrap-c-only-seed-screen-16corpus.tsv`
- `linecache/ml-artifacts/online-bootstrap-c-only-seed-screen-16corpus-summary.json`
- `linecache/ml-artifacts/online-bootstrap-common-rocks-opencv.zst`
- `linecache/ml-artifacts/online-bootstrap-common-llvm-godot.zst`

Run one C-only seed row with:

```sh
python3 linecache/online_bootstrap_curves.py \
  --package-in PACKAGE.zst \
  --test linecache/traces/ml-CORPUS.bin \
  --online-budget 524288 --thresholds 2 \
  --level 3 --model-level 3 \
  --publication first-use --budget-basis ids32 \
  --row-set pretrained --pretrained-mode seed-only \
  --max-tus 200 \
  --curve-tsv /tmp/online-bootstrap-seed-CORPUS-200.tsv \
  --report /tmp/online-bootstrap-seed-CORPUS-200.json
```

Regenerate the static site after regenerating the summary:

```sh
python3 linecache/render_codec_bakeoff_report.py
```

Verification performed before publication:

- all 16 C-only seed rows exact;
- all three curves have identical per-TU raw boundaries at every compared prefix;
- canonical package A and B hashes reproduced;
- package B regenerated from LLVM + Godot in a fresh run;
- installed-mode one-TU regression totals unchanged;
- mixed dense/exact definition tests pass;
- focused unit suite passes;
- Python compile and lint pass;
- generated HTML loads without external assets;
- screen and print rendering inspected;
- summary regeneration deterministic;
- staged diff check passes.
