# In-stream closure

> ## TWO WITHDRAWALS
>
> **1. The general "+711 per warm rebuild" is withdrawn.** The constant marginal is a
> small-corpus regime and must not be quoted as a general warm-rebuild cost.
>
> **2. My "fundamental bounded-history law" is ALSO withdrawn — it was wrong.** I explained
> the large-corpus spikes as pure `--hist` eviction. local-oracle showed that is at most
> partly true, and the source confirms it: in `grz2g.cpp` the anchor insert
> (`tbl[slot] = wp + 1`, line 392) sits **inside the `if (!did)` no-match branch**, while a
> successful COPY does `wp = lit_start` (line 381) and **jumps past the copied span without
> planting any anchors in it**. So build 2 matches build 1 but never anchors its own bytes;
> build 3 can therefore only match build 1, and once build 1 leaves the window build 3 pays
> cold.
>
> **The tell was in my own table and I missed it.** range-v3 alternates
> **+4,987 / +625,060 / +4,991** — build 2 cheap (matches build 1), build 3 cold (build 1
> evicted, build 2 never anchored), build 4 cheap again (matches build 3, which *was*
> anchored as literal). **Pure eviction would degrade monotonically, not alternate.** I
> asserted a mechanism from correlation without checking that the pattern was consistent
> with it.
>
> **What is actually established:** for corpora where a build exceeds roughly `hist/3`, the
> *current* matcher policy — no re-anchoring across COPY output — leaves warm rebuilds
> unable to chain, and the marginal alternates between near-free and near-cold. That is a
> **fixable policy artifact, not a codec law**. A bounded anchor-refresh experiment (plant at
> normal sampling positions while advancing through COPY output; report
> compression/rate/churn/memory) is the follow-up, scheduled after the P29 work. Corpora
> genuinely larger than the window are a separate question that does need more history.

## GRZ2 across 9 cells (4 docker profiles, native, 51-861 TUs/build)

`grz2-breadth-proof.tsv`. Per cell: one continuing stream with `--build-tus`, offsets at
every build close, each k-build stream decoded against builds 1..k, and `cmp` proving the
k-build stream is a byte-prefix of the 4-build stream.

**Points 4 and 6 hold on every cell: `prefix_1in4` and `prefix_2in4` IDENTICAL, and
`decode_k1/k2/k4` EXACT, 9 of 9.** Every build boundary is a group close in all 9.

(`all_closed_by_build=NO` on three cells is expected, not a defect: when TUs-per-build
exceeds `--gtu 112` there are additional gtu-closes *between* the build closes. Every build
boundary is still a close — the column name was misleading and is explained here.)

## The finding: warm rebuilds are cheap only while the earlier build is still in the window

Marginal bytes per build are constant on small corpora and **spike** on large ones:

| cell | raw/build | b1 | b2 | b3 | b4 |
|---|---:|---:|---:|---:|---:|
| re2 / debian-gcc | 85 MB | 362,244 | +711 | +711 | +711 |
| fmt / linuxbrew | 129 MB | 595,150 | +543 | +543 | +543 |
| leveldb / conan-gcc | 178 MB | 621,501 | +887 | +887 | +887 |
| nlohmann-json / debian-gcc | 296 MB | 760,323 | +928 | +928 | +928 |
| spdlog / linuxbrew | 262 MB | 498,311 | +1,618 | +1,618 | +1,618 |
| cereal / fedora-clang-libcxx | 278 MB | 381,743 | +776 | +776 | **+154,086** |
| range-v3 / debian-gcc | 866 MB | 625,096 | +4,987 | **+625,060** | +4,991 |
| catch2 / debian-gcc | 873 MB | 627,706 | **+251,604** | **+502,895** | +241,673 |

This is **not a harness artifact — it is the codec's bounded history doing exactly what it
says.** `range-v3.4build.groupcurve.tsv` shows the mechanism directly, with `--hist 1024`
= 1,073,741,824 B and 866,488,941 B per build:

- groups 5-6 (build 2): `hist_base=0`, `hist_extent` 1,010,835,026 — build 1 still fully in
  the window, `comp_bytes` ~1,031-1,035, i.e. essentially free;
- group 7 onward: `hist_base` becomes non-zero (55 MB → 251 MB → 474 MB → 659 MB) and
  `hist_extent` pins at exactly the 1 GB cap;
- once `hist_base` passes build 1's extent, **build 1 is evicted**, so build 3 can no longer
  match against it and `comp_bytes` jumps back to 234,295 / 144,295 / 66,212 — cold-like.

**The law: a warm rebuild stays nearly free only while the matching earlier build is still
inside the retained history window; as eviction eats into it the marginal cost rises toward
a second cold build.** The constant +711 is a small-corpus regime, not a universal.

That makes the 4-pass experiment worth having done: with a single pass this is invisible,
and quoting "+711 per rebuild" as general would have been wrong for any corpus above roughly
`--hist / 3`.

## Immutability across the other codecs

- **zstd-3 per TU — byte-level PROVEN.** With a concatenated length-prefixed wire, the
  1-build wire (16,618,723 B) is **byte-identical to the first 16,618,723 bytes** of the
  4-build wire. Expected, since each TU frame is independent, but now verified rather than
  assumed.
- **fast interner — value-level PROVEN, not byte-level.** All 72 per-TU wire values are
  identical between the 1-build and 4-build runs, and build 1's cumulative is identical
  (5,765,164 both). `production-fused` emits no concatenated wire artifact, so this is a
  per-TU value comparison rather than a `cmp` of bytes; stated as such deliberately.
- **P29 — the only holdout**, blocked as documented above.

So **three of four codecs have real per-build closure**; P29 remains 4x-totals-only pending
the scope decision.
