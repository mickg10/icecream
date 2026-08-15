# Cross-context hybrid Region codec: balanced 16-corpus report

## Decision boundary

This report covers the exact Region-program layer of the proposed `.ii` transfer
codec. It does not claim a complete-file ratio. Line definitions, typed values,
literal residuals, missing-object exchanges, framing, and the C/F production path
must still be integrated and charged before the complete transfer can pass.

The acceptance view is deliberately corpus-balanced. Every one of the 16 corpora
gets one vote through the harmonic mean of its final ratio. DuckDB is one row, with
the same 512 KiB online budget and the same algorithm as every other row.

## Selected contender

The selected contender combines two exact representations that share one installed
phrase package:

1. a context-run representation whose raw gaps use a persistent exact-key table;
2. an atom representation whose raw gaps use the existing dense Region-ID store.

For each TU, C constructs run-local and cross-context parses in both
representations, compresses the complete candidates, and selects the smallest
actual frame. The common context-run sidecar and one-byte representation selector
are charged. No project classifier or future TU information is used.

The important correction is that the chosen representation does not determine the
next persistent codec state. F reconstructs the complete exact Region stream first.
Both endpoints then update the two persistent states from that stream:

- dense Region IDs are appended in first-observation order;
- the context-run table is updated by the same canonical run-local parse over the
  shared installed phrase package and the transmitted run boundaries.

Only C holds the bounded candidate learner. F needs the installed definitions,
the two compact decoding stores, and the canonical update code. A comparison that
duplicated the full candidate learner at F produced no material byte advantage on
the screening corpora, so it is not part of the selected design.

## State ownership

| state | C | F | update point | lifetime |
|---|---|---|---|---|
| installed exact phrase package | encoder copy | decoder copy | append selected definitions before payload decode | cache GUID |
| dense Region-ID store | key-to-ID map plus values | dense values plus lookup needed for canonical update | after exact TU reconstruction | cache GUID |
| canonical context-run table | exact key-to-ID map | dense exact-key vector | after exact TU reconstruction | cache GUID |
| bounded candidate learner | yes | no | after TU has been encoded and decoded | cache GUID |
| frame-local new Region IDs | encoder scratch | decoder scratch | only within one atom payload | one frame |
| run-boundary sidecar | generated | decoded and validated | before canonical state update | one TU |

All persistent IDs are append-only. Definitions never renumber existing phrases.
The receiver rejects a sidecar unless every run length is positive and the lengths
cover the reconstructed Region count exactly.

## Wire blocks and exact per-TU procedure

The capability harness charges the same block convention proposed for the complete
protocol: a four-byte frame length followed by a zstd frame. The selected Region
layer uses:

1. zero or one definition frame;
2. one context-run sidecar frame;
3. one byte selecting the payload representation;
4. one selected payload frame.

The sidecar's uncompressed form is intentionally small: a varint run count followed
by one positive varint Region count for each consecutive semantic-context run. It
does not transmit context values.

For TU `t`, with state containing only TUs before `t`:

1. C serializes and compresses the run-length sidecar.
2. C computes the frozen/empty baseline plus four online candidates:
   run-local/context gaps, cross-context/context gaps, run-local/atom gaps, and
   cross-context/atom gaps.
3. A phrase promoted after an earlier TU remains C-only until a candidate actually
   uses it. The candidate includes every not-yet-installed definition it needs.
4. C compresses each definition-plus-payload candidate and compares actual charged
   bytes. It commits only the smallest candidate.
5. F installs that candidate's definitions, decodes the selected payload, and
   reconstructs the complete Region sequence.
6. F validates the sidecar against the reconstructed Region count and recreates the
   context runs. C and F independently compute the same canonical run-local parse
   using their installed package.
7. C and F update the context-run table from that canonical parse and update dense
   Region IDs from the complete Region sequence.
8. Only after the current TU is complete does C expose it to the bounded candidate
   learner. Promotions caused by `t` cannot encode `t`.

In the atom representation, a Region already present before the TU is a persistent
dense-ID reference. A Region first seen within the TU is defined once with a
frame-local ID and may be referenced again in that frame. After decode, frame-local
IDs are discarded; the canonical dense store is rebuilt independently from the
exact reconstructed order.

## Endpoint invariants exercised by the harness

- Initial package bytes round-trip canonically before the first TU.
- Every received definition frame decompresses to the exact sent bytes.
- Encoder and decoder installed packages are identical after each append.
- Every payload decompresses to the exact encoded payload bytes.
- Every payload expands to the complete expected Region sequence.
- Context-run lengths are positive and cover that sequence exactly.
- C and F independently produce identical canonical context parses.
- C and F context-run dictionaries are identical before and after each TU.
- C and F dense Region stores are identical after each TU.
- Learning observes a TU only after its payload has been selected and decoded.

## Balanced result

All 16 selected cold-start rows replay exactly and improve over their prior
run-local row. The hybrid removes 13,016,512 charged structural bytes, or 21.80%,
from the same 28,554,671,510 raw input bytes. Both the byte-weighted and
equal-corpus ratios improve by approximately 27.9%; the minimum improves by 20.1%.

| metric | prior run-local first-use | selected hybrid |
|---|---:|---:|
| byte-weighted aggregate | 478.25x | **611.58x** |
| equal-corpus ratio | 361.94x | **462.92x** |
| minimum | 122.17x | **146.71x** |
| lower quartile | 271.85x | **350.61x** |
| median | 513.67x | **731.96x** |
| corpora at least 400x | 9 / 16 | **10 / 16** |
| structural H200 by 50% | 10 / 16 | 10 / 16 |
| exact final replay | 16 / 16 | **16 / 16** |

| corpus | cold start | prior | hybrid | ratio gain | sidecar bytes |
|---|---:|---:|---:|---:|---:|
| Abseil | package | 284.97x | 343.63x | +20.59% | 43,198 |
| Catch2 | package | 818.99x | 964.72x | +17.79% | 20,118 |
| Cereal | empty | 1203.18x | 2157.44x | +79.31% | 2,025 |
| DuckDB | package | 288.91x | 378.74x | +31.09% | 38,803 |
| Eigen | package | 2507.23x | 3096.72x | +23.51% | 12,209 |
| fmt | package | 122.17x | 146.71x | +20.08% | 3,036 |
| Godot | package | 751.05x | 1130.69x | +50.55% | 99,005 |
| LevelDB | package | 199.16x | 249.11x | +25.08% | 4,122 |
| LLVM | package | 819.50x | 1054.75x | +28.71% | 69,353 |
| nlohmann-json | package | 406.54x | 585.33x | +43.98% | 5,186 |
| OpenCV | empty | 639.18x | 960.64x | +50.29% | 52,150 |
| range-v3 | package | 811.83x | 1017.15x | +25.29% | 14,315 |
| RE2 | package | 336.44x | 452.58x | +34.52% | 3,451 |
| RocksDB | empty | 187.89x | 215.08x | +14.47% | 36,582 |
| simdjson | package | 620.81x | 878.59x | +41.52% | 6,798 |
| spdlog | package | 258.73x | 357.58x | +38.20% | 1,611 |

The 46,690,193-byte charged structural ledger is:

| block | bytes | fraction |
|---|---:|---:|
| initial package frames | 1,028,794 | 2.20% |
| first-use definition frames | 605,733 | 1.30% |
| context-run sidecars | 411,962 | 0.88% |
| selected payload frames | 44,634,412 | 95.59% |
| selectors | 9,292 | 0.02% |

The retained machine-readable summary and per-corpus table will live at:

- `linecache/ml-artifacts/cross-context-hybrid-16corpus-summary.json`
- `linecache/ml-artifacts/cross-context-hybrid-16corpus.tsv`

Raw capability reports and `/usr/bin/time` outputs are retained under
`/tmp/cross-hybrid-installed-full-<corpus>.json` and
`/tmp/cross-hybrid-installed-full-<corpus>.time` on the measurement host.

## Cold empty start versus a pretrained package

The common 79,138-byte package was learned from RocksDB and OpenCV, so those two
projects are not package holdouts. Across the other 14 corpora, using the same
run-local learner on both sides of the comparison:

- TU 1 strongly favors empty start because the package is fully charged;
- the equal-corpus curves are approximately even by TU 5;
- the package is ahead by 10.93% at TU 10 and 13.38% at TU 25;
- the final equal-corpus ratio is 335.17x empty versus 373.50x packaged, +11.44%;
- the package wins final bytes on 13/14 holdouts, while Cereal favors empty start;
- Eigen crosses permanently at TU 93 and Catch2 at TU 277.

This supports an optional charged bootstrap candidate, not a DuckDB-specific or
mandatory package. The detailed curves remain in
`online-bootstrap-pretraining-14corpus-summary.json` and its TSV companion.

## Stability controls

The selected rule is tested under standard order, three independent shuffles,
reverse order, and a shared-Region perturbation on fmt and LevelDB. One shuffle and
one perturbation are also run on the larger Abseil and RocksDB corpora. All 14
variant rows replay exactly.

| corpus | standard | tested variants | observed range | worst change from standard |
|---|---:|---:|---:|---:|
| fmt | 146.71x | 5 | 146.71–150.92x | +0.00% |
| LevelDB | 249.11x | 5 | 243.67–249.11x | -2.18% |
| Abseil | 343.63x | 2 | 343.40–362.36x | -0.07% |
| RocksDB | 215.08x | 2 | 215.08–215.34x | +0.00% |

The perturbation replaces the most widely shared Region in every TU of each tested
corpus: 50/50 fmt TUs, 72/72 LevelDB TUs, 700/700 Abseil TUs, and 622/622 RocksDB
TUs. Charged wire changes by -16, +247, +4,992, and 0 bytes respectively. The
largest order loss is LevelDB reverse order at 2.18%; it remains 22.35% above the
prior standard-order baseline. The full variant ledger is retained in
`linecache/ml-artifacts/cross-context-hybrid-stability.tsv`.

## Rejected or subordinate bakeoff modes

These modes remain reproducible controls; none is part of the product handoff.

### Extra cross-context learning candidates

Adding overlapping whole-TU candidates to the learner improved some projects but
crowded the bounded candidate table and regressed Abseil and range-v3. Increasing
the table masked one regression at extra state cost. The selected design therefore
learns the stable run-local set and only broadens matching.

### Path-dependent dual matching

Trying the run-local and cross-context parse from the current chosen dynamic state
improved 13/16 corpora and raised the equal-corpus ratio above 400x, but it regressed
OpenCV, Godot, and Eigen. A locally smaller frame changed later raw-gap IDs. A
three-TU probe misclassified Godot because it remained ahead until TU 267 and lost
later. Project-neutral early gating was rejected.

### Coverage dynamic program

A maximum-coverage parse added 0.90% on fmt and 1.93% on LevelDB in the Python
screen, but roughly doubled parse work and did not remove the state dependency.
Actual compressed-byte comparison across the two canonical representations produced
larger gains with a simpler receiver.

### Static-only cross-run replacement

Replacing two or more adjacent installed static keys with an already installed
larger phrase kept dynamic state identical but gained only about 0.02% on fmt and
0.01% on LevelDB.

### Canonical atom state alone

The atom-only rule reached 520.17x byte-weighted and 424.88x equal-corpus across all
16 corpora, proving sufficient structural headroom, but full OpenCV fell 23.46%
below its prior run-local result. The hybrid retains atoms as an option and also
keeps the context-run representation.

### Fixed initial-package context state

Canonicalizing context gaps only against the initial package discarded useful online
state. It fell to 354.45x on full OpenCV and was 22.32% below the prior Godot prefix
at TU 600. Canonicalizing against the shared installed package corrected this.

### Duplicate learner at F

Running the complete bounded candidate learner independently at F produced 960.79x
on full OpenCV and 1754.58x on the first 600 Godot TUs. On fmt and LevelDB its total
differed from the installed-package canonical rule by at most six bytes per
complete run. The selected rule keeps the learner C-only.

## Performance and memory boundary

The Python program is a behavior and byte-accounting harness. It constructs and
compresses several candidates and uses allocation-heavy byte strings and maps; its
wall time and RSS are retained to prevent them from being mistaken for product
numbers. Peak measured Python RSS was 2,465,684 KiB on RocksDB; the slowest row was
Godot at 1,995.55 seconds. The production implementation must use the existing dense Region store,
compact exact-key tables, reusable buffers, and the shared C/F interning module.

Acceptance still requires at least 1 GB/s effective encode and decode throughput on
the complete selected transfer path, with the full wire ledger and realistic
parallel preprocessing. Candidate structural streams are hundreds of times smaller
than raw `.ii` input, so comparing several small compressed buffers can be viable,
but that must be measured in C++ rather than inferred.

## Next implementation gate

1. BigOracle reviews whether the installed-package canonical update is the smallest
   sufficient state rule and whether all four candidates are needed in production.
2. The implementer ports only the selected state machine into the shared interner
   and C/F stores; the Python object layout is not a template.
3. Unit tests force each payload representation, new definitions, frame-local atom
   reuse, context-run validation, and canonical state equality.
4. Scenario tests run cold, warm, reordered, one-header-change, C-cache rotation,
   F-cache eviction, and missing-object recovery through the real daemons.
5. The complete line/value/residual ledger must satisfy the cold/half-cold ratio
   targets and the 1 GB/s throughput gate before this work is considered delivered.
