# P50 perf-opt: F-side materialize candidates (2026-10-07)

Status: historical/working record for the `perf-opt` branch bench. Not a
qualification artifact; see `research/README.md` for the usual caveats.

This document intentionally contains no CodebaseN file names, directory or
component names, source paths, or code. CodebaseN content never leaves
C1/H1 and is never committed to this repository; only aggregate bench
statistics are recorded here.

Scope: all candidates below are **wire-neutral**. Every candidate was
verified byte-identical on the CacheWire/P29 wire (inner `.cf`/`.fc` dumps
and the `state_digest`/`transaction_digest` fields) against an unmodified
baseline over the full 60-TU corpus described in
`P50_PERF_OPT_CORPUS.md`. None of them change what goes on the wire; they
only change how F's materialize path gets the bytes into memory.

## Headline result

The metric that matters is wall time in `f_apply_materialize_ns`
(production field, already wraps the whole materialize-and-verify call,
including any copy-back), not minor-fault count. Minor faults are a proxy
that can be made to look arbitrarily good while making wall time worse, if
the faults are simply moved into an untimed stage. That is exactly what
happened to the first allocator-based candidate below.

| Candidate | Allocator/type change | Copy-back | `f_apply_materialize_ns` p50 | p99 | vs baseline |
|---|---|---|---|---|---|
| baseline (no macro) | no | no | ~10.7-11.7 ms | ~28-36 ms | - |
| opt-4, `8e1fe736` (2-MiB-aligned allocator) | **yes** | **yes** | ~21.9-22.8 ms | ~40-46 ms | **~2x worse** |
| opt-5, interior madvise (this commit) | no | no | ~7.0-7.2 ms | ~24-28 ms | **~35% better** |

Ranges above span all three measurement shapes run this round: a single
60-TU pass, a 5x interleaved-solo pass (5 repetitions, 3 arms interleaved
per repetition to control for drift), and an 8-way concurrent pass (8
simultaneous single-process runs per arm, within the 8-CPU cap). All three
shapes agree on both direction and rough magnitude.

**Verdict: opt-4 (`8e1fe736`, 2-MiB-aligned custom allocator) is rejected.
Opt-5 (reserve + 2-MiB-aligned interior `madvise(MADV_HUGEPAGE)`, plain
`std::vector<uint8_t>`, no copy-back) wins on time and goes forward as the
production-shaped candidate.**

## Why opt-4 looked good on minor faults and loses on time

Opt-4 backs `pending_.materialized` with a custom allocator that mmaps a
2-MiB-aligned region directly, so the receive-fill stage itself sees minor
faults drop to near zero (p50 23-24 vs baseline's p50 ~8.6k). But the
custom allocator's buffer type isn't the plain `std::vector<uint8_t>` the
rest of the pipeline expects, so a copy-back into a fresh plain vector was
required at the end of materialize-and-verify. That copy-back was
previously untimed. Once it was instrumented as its own stage
(`ReceiverMaterializeCopyback`), the picture flips:

- Copy-back alone: `ns` p50 ~8.6 ms, p99 ~11.7-13.9 ms (100% of rows pay it
  — it's unconditional on this path).
- Copy-back's own minor faults: p50 ~8.3k, p99 ~11.4-11.5k — i.e. almost
  exactly the fault count that disappeared from receive-fill. The faults
  did not go away; they moved into a stage nobody was timing or counting
  before this round.
- Net: opt-4's `f_apply_materialize_ns` is roughly **double** baseline, not
  an improvement, despite the receive-fill minor-fault count looking
  dramatically better in isolation.

## Why opt-5 wins

Opt-5 keeps `pending_.materialized` as the plain `std::vector<uint8_t>`
the rest of the pipeline already expects — no allocator-type change, no
public-type change, no copy-back. It does a single `reserve()` (glibc
mmaps allocations above its mmap threshold, so the reservation is already
page-aligned at both ends), then `madvise(MADV_HUGEPAGE)` over only the
2-MiB-aligned **interior** of that reservation —
`[round_up(data()), round_down(data()+capacity()))` — before any write
touches it. The unaligned head and tail (each under 2 MiB) are left alone
and still fault at normal 4 KiB granularity.

Measured effect: receive-fill minor faults drop from baseline's p50 ~8.6k
to p50 ~0.6-0.7k (roughly an order of magnitude, as expected from "interior
only" rather than opt-4's "whole allocation" alignment), with **zero**
copy-back cost (confirmed: `receiver_materialize_copyback_ns` is 0 in
100% of rows for this candidate, since the copy-back code path isn't
compiled in when there's no allocator-type change). Net `f_apply_materialize_ns`
comes in ~35% below baseline at p50, consistently across the single-run,
5x-interleaved-solo, and 8-way-concurrent measurement shapes.

## Candidate ledger

- **`902409a2`** — opt-3, full-extent `MADV_HUGEPAGE` hint over the whole
  reservation (not just the aligned interior). Superseded by opt-5 for the
  current round of measurement; kept as the macro
  `ICECC_P29V1_MADVISE_HUGEPAGE_MATERIALIZED`, now an `#elif` sibling of
  opt-5's macro in the same reserve-path block (mutually exclusive).
- **`5013d5f2`** — opt-3 variant, sticky high-water-mark reserve hint for
  the sender body. **Rejected.** Tagged
  `perf/po-rejected-reservehint-5013d5f2`.
- **`8e1fe736`** — opt-4, 2-MiB-aligned custom allocator for the F
  materialize buffer. **Rejected this round** — see headline result above.
  Net ~2x regression in `f_apply_materialize_ns` once the copy-back it
  requires is timed; the minor-fault win it reports is real but is paid
  back with the copy-back's own fault count plus the copy's own CPU time.
- **This commit** — opt-5, reserve + 2-MiB-aligned interior `madvise`.
  **Winner.** No allocator-type change, no copy-back, smaller fault-count
  win than opt-4 claimed in isolation, but a real and the only
  net-positive wall-time win measured this round. The compile-time macro
  from the prior round is replaced by a runtime switch,
  `ICECC_P29V1_MATERIALIZE_THP=1` (default OFF, read once per process),
  so a single binary can be A/B'd per worker on the farm without a
  rebuild. A unit test (`run_materialize_thp_switch_controls`, in
  `unittests/codec_wire_test.cpp`) forces the switch on and off via a
  test-only override slot and confirms the `madvise()` call site is taken
  only when forced on, with materialized bytes unaffected either way.
  Building that test surfaced and fixed an unrelated, genuine decode-side
  bug in `ContinuingDecoder::decode()` (`p29_wire.h`): its loop-exit test
  did not check whether the current zstd frame had already finished
  before continuing on `output.pos == output.size` alone, so a decoded
  payload landing on an exact multiple of the internal output-buffer size
  triggered one spurious extra iteration, which read as "stream did not
  close" even though the frame was already complete. The fix only changes
  how many times the decode loop iterates in that exact-multiple edge
  case; it cannot and does not change a single byte on the wire. Full
  wire/digest identity was re-verified after the fix (see Methodology
  notes).

## Independent lever: `hugetlb=1`

Separate from all of the above (no code change on this branch at all): a
host-level `transparent_hugepage=madvise` + `hugetlb=1`-equivalent farm
mount/boot configuration was independently validated as a viable
production lever on a live farm leg, with the same wire-neutral property
(it is a kernel/host setting, not a code path, so it cannot touch
anything on CacheWire/P29). It composes with opt-5 above rather than
competing with it, since opt-5's `madvise()` call is exactly the hint this
host configuration is designed to honor fastest. Recorded here for
completeness; the farm-leg validation itself lives outside this
repository's history.

## Methodology notes

- All measurements use the 60-TU real-corpus bench described in
  `P50_PERF_OPT_CORPUS.md`, `ICECC_P50_PROFILE=P29V1`, on an isolated,
  CPU-capped (8 CPUs) container with no other load.
- Each arm (baseline / opt-4 / opt-5) runs from its own binary, built with
  a dedicated compiler-wrapper macro set, from the same source commit, so
  only the macro-gated code differs between binaries.
- Wire/digest identity was re-verified for every arm, every round: inner
  `.cf`/`.fc` dumps byte-identical by SHA-256 across all three arms, and
  `state_digest`/`transaction_digest` identical row-for-row in the batch
  output. No candidate changes a single byte on the wire.
- Timing source: `f_apply_materialize_ns` (production field, wraps the
  whole materialize-and-verify call including any copy-back) plus the
  per-stage `P29StageTimerEntry` breakdown and per-stage
  `RUSAGE_THREAD`/`ru_minflt` counters, both gated behind sim-only
  instrumentation macros and confined to `cache/codec/p29_wire.h`,
  `cache/p50_slice0.cpp`, and `cache/sim/p50sim.cpp` — the three files this
  commit touches. Production code
  (`cache/p50_endpoint.{h,cpp}`, `cache/p50_cache_service.cpp`) is
  untouched.
- Fault counts and `ns` figures above are p50/p99 across n=60 (single
  run), n=300 (5x interleaved solo: 5 repetitions x 60 TUs, arms
  interleaved per repetition), and n=480 (8-way concurrent: 8 simultaneous
  single-process runs x 60 TUs) rows respectively; all three shapes agree
  in direction and rough magnitude, so only the range across shapes is
  quoted in the headline table rather than each shape individually.
