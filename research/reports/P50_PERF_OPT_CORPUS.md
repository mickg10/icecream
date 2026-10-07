# P50 perf-opt replay corpus (2026-10-07)

Status: historical/working record for the `perf-opt` branch bench. Not a
qualification artifact; see `research/README.md` for the usual caveats.

This document intentionally contains no CodebaseN file names, directory or
component names, source paths, or code. CodebaseN content never leaves
C1/H1 and is never committed to this repository; only aggregate corpus
statistics are recorded here.

## Why this corpus exists

INSTR-1c/PRIO-1 traces on the 3-worker farm show CodebaseN TUs averaging
~36 MB on the wire going into the P50 cache path (`raw_bytes`), with the
F-side route lock held ~70 ms/transfer. A standalone replay bench needs a
corpus of real TUs at that size to reproduce the hot path (C prepare/matcher,
F materialize/digest) without a live farm.

## The size discrepancy and its root cause

A first corpus built from plain `clang -E` output topped out at 16.3 MB
(mean 7.4 MB over several thousand candidate TUs, p99 13.8 MB) — nowhere
near the ~20-45 MB target. This was not a sampling artifact: the real icecc
client never ships plain `-E` output. It always appends `-E <file>` followed
by `-frewrite-includes` (clang) or `-fdirectives-only` (gcc) — this repo's
own `client/cpp.cpp` and `client/local.cpp`
(`compiler_only_rewrite_includes()`) confirm this is the default unless
`ICECC_REMOTE_CPP` / `ICECC_CLANG_REMOTE_CPP` disables it (neither is set on
this leg). `-frewrite-includes` keeps every included file's full text,
including comments and inactive `#if` branches, instead of fully
macro-expanding — hence the ~3x size multiplier over plain `-E`.

Spot-check (plain `-E` vs `-E -frewrite-includes`, 3 sample TUs, real
clang-22, no icecc masquerade): the ratio held at roughly 3.0-3.4x across
all three samples (e.g. one case went from 16.3 MB to 51.8 MB).

This confirmed the fix: regenerate the corpus using the client's exact
command (compile flags, then `-E <file> -frewrite-includes`, same order the
client builds it), run through the real compiler directly.

## Generation method

Generation script kept local-only (not checked into this repository).

1. Full-tree scan (plain `-E`, 16-way parallel) over the CodebaseN source
   tree to get a size-ranked candidate pool cheaply (results kept
   local-only).
2. Candidate pool: TUs whose plain-`-E` size falls in [4 MB, 15 MB] — the
   band whose `-frewrite-includes` mapping lands near the target range.
   Oversampled 220 candidates, evenly spaced across the sorted plain-size
   list.
3. Verify each candidate with the client-equivalent command
   (`-E <file> -frewrite-includes`) via the real compiler, 8-way parallel,
   150s timeout per TU.
4. Keep verified TUs landing in [20 MB, 45 MB] (217/220 verified in-range).
5. Final selection: **value-stratified**, not index-stratified — 60 target
   byte values evenly spaced across [20 MB, 45 MB], nearest-available
   candidate picked per target, no reuse. (A first pass used index-based
   stratification on both the candidate pool and the final pick, which
   skewed the result toward the upper-middle of the range and missed the
   20-34 MB band entirely; the value-stratified pass fixed this.)

## Final distribution

60 TUs, real CodebaseN sources:

| Stat | Value |
|---|---|
| count | 60 |
| min | 26.2 MB |
| p50 | 34.4 MB |
| mean | 34.9 MB |
| p99 | 47.0 MB |
| max | 47.0 MB |

Production trace cites p50 ~28 MB, mean ~36 MB. This corpus's p50/mean
(34.4/34.9 MB) are somewhat higher but the same order of magnitude, and the
60 TUs genuinely span 26.2-47.0 MB rather than clustering at the top — this
was accepted as sufficient for the bench ("spanning the distribution, not
just the largest").

## Storage and handling

Per the hard rule that CodebaseN content never leaves C1/H1: the corpus was
generated entirely on C1 and copied directly C1 -> H1 via `rsync` over ssh
to `/data/mg/perf/work/tu-corpus` (local only; 60 `.i` files + a manifest,
~2.0 GB). No third host, no shared storage, no upload anywhere else.
Transfer verified by file count (61) and manifest md5 match on both ends
(`e08a8a0a1539536dc0492c3465d1452c`).

The per-TU manifest (names/sizes/source identifiers) and the full-tree scan
results stay on C1/H1 only and are never committed to this repository, nor
is the generation script. The bench itself takes the corpus location as an
argument/environment variable at run time; no corpus path, TU name, or
CodebaseN content is hardcoded anywhere in this repository.

Superseded plain-`-E` artifacts on C1 were deleted once this corpus was in
place and verified on H1.
