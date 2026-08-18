# Active 44-cell replay: provenance and reuse boundary

Date: 2026-08-18 UTC

Role: mickg10/local-oracle

Retained run:

```text
/home/ttuser/issue16-selector-v1/matrix44-20260818T0230Z
```

This replay is a measurement cache, not a fitted or resource-normalized selector result. It
traverses the 44 verified `ice-ii-corpus-v1` cells (11 projects by four build profiles) in one
foreground, resumable process. Every cell independently verifies the archive and ordered TU
digests, reconstructs the complete GRZ2 input exactly, and retains the complete codec wires,
curves, timing logs, and whole-program zstd references.

The exact cell identities, TU/raw extents, declared payload paths and sizes, archive payload
digests, `corpus.json` digests, and `manifest.tsv` digests are frozen in
`verified-44-cells.tsv`. This is a complete 11-by-4 matrix; it contains no partial-profile
projects. `freeze_selector_ledger.py` regenerates or checks that ledger from the declared corpus
generation.

Some cell directories also retain an older project-named archive, for example
`catch2-debian-gcc.ii.tar.zst`. That is a different corpus generation. Corpus selection must
never glob `*.ii.tar.zst`: the only active payload is the relative path in
`corpus.json.payload.path` (currently `ii.tar.zst` for all 44 cells). The replay resolves that
field, checks its byte count and digest, then verifies every extracted TU against the manifest.

The extended ledger SHA-256 is:

```text
6a496fe71ed9282c06cc308599deb4f5ce38a0301e8193531efeea15c79d438d
```

It was regenerated and then independently checked in place with:

```text
freeze_selector_ledger.py \
  --matrix-root /home/ttuser/ictmp/ii-matrix \
  --expected-cells 44 \
  --check /home/ttuser/issue16-selector-v1/verified-44-cells-v2.tsv
verified 44 corpus cells against .../verified-44-cells-v2.tsv
```

## GRZ2 source and binary closure

The deployed GRZ2 source is the accepted demand-populated decoder-ring source plus five
wire-neutral first-group anchor counters. The counter-only diff records samples, occupied and
usable slots, unequal usable anchors, and matches; it does not change the parser decision or
container format.

```text
accepted demand-ring source SHA-256
  1f6beb2078b172031f0b24409de44a9608c12c6c2ef79f2959c6e8aa83cb1ed9

deployed anchor-census source SHA-256
  e997b612c3445c951fa8bfc4abd2942fbad532fac85ebae5aa5fe753fe64a6b9

deployed and independently rebuilt binary SHA-256
  647883b7a346ae48d76ea2ac542240e5c78b4a56049372ee6067f5ec94276af9
```

Build reproduced on `tt-quietbox2`:

```text
g++ -O3 -march=native -std=c++17 -Wall -Wextra -Wpedantic -Werror \
    -I /home/ttuser/libbsc -c grz2g-selector.cpp -o grz2g-selector.repro.o
g++ grz2g-selector.repro.o /home/ttuser/grouprlz/libbsc.a \
    -lzstd -lpthread -o grz2g-selector.repro
```

The rebuilt file compares byte-for-byte with the deployed binary. The deployed binary passes
the expanded 12/12 independent gate, including the late oversized-TU growth case:

```text
/home/ttuser/issue16-selector-v1/gate-anchor-census-retained-20260818T0230Z
/home/ttuser/issue16-selector-v1/gate-anchor-census-retained-20260818T0230Z.log
```

The anchor-census and accepted demand-ring binaries also encode that late-growth/wrap fixture to
the same container:

```text
07dae55d892410629cd51b61656ec5ac4a91b161561d6c5fcf86f553270d900a
```

The replay therefore uses the current demand-populated decoder, not the earlier decoder whose
small-corpus F rates were dominated by populating the unused logical ring.

## P29 source and known correction boundary

The active replay predates the chronological Root-token correction:

```text
P29 source commit
  00f83f8  linecache: integrate bounded BSC residual frames
P29 source SHA-256
  2fccb899d518441ca81357f7543028bfe58fa734155c550926e22ff3e4fc125f
P29 binary SHA-256
  8adb8b394970dd4e67aa82222874ad9a95c2d808c577c649a290b0d7d52a2493
```

The corrected source is commit `56c1744` on
`local-oracle/issue16-p29-prefix-state`. It uses stable Region/Block Root tags and has a
suffix-blind identity gate. The active replay does not contain that correction.

The active runner also truncates its short P29 run to GRZ2's first-group TU count. GRZ2 may close
before TU112 because of its raw or ADD cap, while P29's first literal group still spans TU112.
Those rows compare different source extents and are not selector inputs.

## What may be reused

- verified project/profile identity, ordered TU count, raw extent, and raw digest;
- whole-program zstd-6-long and zstd-19-long references;
- complete GRZ2 wire, exact reconstruction, group curve, first-group census, and current-decoder
  rate from the deployed demand-ring binary;
- complete legacy P29 physical-size ledger and component curve as a diagnostic comparison (the
  research harness retains its literal-group wire, not one monolithic P29 container);
- all retained logs and hashes needed to audit those measurements.

## What must be regenerated before fitting

- complete P29 with stable Root tags;
- suffix-blind P29 through `min(112, total_TUs)`, regardless of where GRZ2 closes group one;
- exact complete-versus-suffix-blind prefix identity for every P29 row;
- P29 C-stage timing that directly accounts for the shared input producer, interning, planning,
  material construction, entropy work, and serialization without subtracting an inferred F time;
- a TU112 probe makespan and core-seconds measurement under one fixed total core budget;
- final labels and selected totals using only the corrected complete wires;
- lineage-held-out selector evaluation, fixed-16 replay, native-25 replay, and all verified v2
  cells.

Until those rows exist, any complete-size minimum is a hindsight ceiling and any rate-composed
selection total is diagnostic only.
