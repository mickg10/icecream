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

## Proof gates made real, and a corrected zstd-3 figure

**`selector_grzproof.sh` was evidence collection, not a gate** — it exits success even after
a DIFFER, a FAIL or a skipped cell. `selector_grzgate.sh` is the gate. Against a declared
cell list it requires: every declared cell present **exactly once** (no missing, duplicate
or stale rows), four **strictly increasing** build-close offsets, container size greater than
the last offset with the terminator equal to the difference, **build 4 carrying the
terminator**, `prefix_1in4`/`prefix_2in4` IDENTICAL, `decode_k1/k2/k4` EXACT, and
`build_close_rows` exactly `4/4`.

Verified to fail, by true exit status (not a piped one — my first test read `tail`'s status
and reported a false pass):

| case | exit |
|---|---|
| real 7-cell table | **0** |
| a `DIFFER` injected | 1 |
| duplicate rows appended | 1 |
| missing table | 2 |
| no declared cells | 2 |

**zstd-3 physical bytes were understated.** The wire writes a 4-byte length before each TU
frame, but the encoder's curve summed payload only. `selector_zstd3recv.cpp` is a receiver
that consumes **only the wire artifact**, parses the frames, decompresses and reconstructs
every TU against the manifest:

```
ZSTD3RECV tus=72  physical=16,618,723  payload=16,618,435  framing=288    raw=110,231,455  all_reconstructed=YES
ZSTD3RECV tus=288 physical=66,474,892  payload=66,473,740  framing=1,152  raw=440,925,820  all_reconstructed=YES
```

So physical = payload + **4 B per TU** exactly; the published per-TU cumulative was low by
288 B over 72 TUs. Small, but it was wrong, and the figure now comes from a receiver that
reconstructs the input rather than from the encoder's own accounting.

**The fast interner remains VALUE-level.** Its `--wire` is emitted and `cmp`-compared, but
there is no receiver parsing and reconstructing from it yet, so it does not get the
byte-level claim until there is.

## Fast interner: the wire is not self-contained, and USED_KEYS is uncounted

Building the receiver local-oracle asked for surfaced a bigger problem than the missing
receiver itself.

`production-fused.cpp` never serialises `used_g` — the per-TU list of global line keys. F
reads C's in-process vector directly at line 728 (`F.have(used_g[i])` for the MISSING
bitmap) and again at 744-745 when resolving every non-missing local index back to a store
entry. So:

1. **The emitted wire is not self-contained.** A receiver consuming only the body frames
   cannot reconstruct, because the local-index → global-key mapping never crosses it. The
   `--wire` I added captures the body frames only; it is not the complete protocol wire.
2. **USED_KEYS is never charged.** `st.comp` accumulates only `csz`, the compressed body
   (line 737). The keyset message — which the header comment describes as a real C→F
   transmission — contributes **zero** to the reported wire.

Measured on re2 (72 TUs, 1 build):

| | bytes |
|---|---:|
| reported wire (body only) | 5,765,164 |
| USED_KEYS transmitted | 1,554,824 keys |
| uncounted at 4 B/key (raw u32) | 6,219,296 → reported wire is **48.1%** of that basis |
| uncounted at 2 B/key (varint estimate) | 3,109,648 → **65.0%** |

Over the 4-build run: 20,913,868 B reported against 6,219,296 keys, so the reported figure
is **45.7%** of the 4 B/key basis.

**So the fast-interner line is understated by roughly 2x, not by a rounding term.** This is
not the zstd-3 case (a real wire, missing its 4-byte frame headers); here a required protocol
message is absent from the wire entirely and absent from the accounting.

Consequences, stated conservatively:

- The fast interner **cannot** be given a byte-level immutability claim: there is no
  self-contained wire to reconstruct from. It stays value-level, and the earlier `cmp` was
  over an incomplete artifact.
- Its published per-TU and cumulative figures should be read as **body-only, a lower bound**,
  until USED_KEYS is serialised and charged.
- This is the same class of defect as P29's: an accounting total that does not correspond to
  a physical stream. It is exactly why local-oracle's two-sink requirement (charge only real
  protocol frame header+payload, on a real directional sink) is the right bar.

Fixing it means emitting USED_KEYS as a real C→F frame and charging it — a change to
`production-fused.cpp`, not to the harness. Flagged rather than started, since it changes a
published line and the P29 work is queued ahead of it.

## PER-TU EMISSION: the correct transport, and what it costs

The owner is right that `--gtu 112` is not a legal transport. A compile farm dispatches each
TU the moment it is produced; the codec cannot hold 112 TUs before emitting, because the
remote cannot wait for a group to close.

Re-run with **`--gtu 1`** — one frame per TU, emitted immediately — while **retaining
matcher, history and dictionary state across TUs**, so the cross-TU long-range dedup is
untouched and only the emission granularity changes. `--build-tus N` still marks the build
closes, and the two are compatible: each build is N per-TU frames, the last of which is the
build close.

Frame counts confirm it: 288 / 204 / 320 / 376 frames for 4x72 / 4x51 / 4x80 / 4x94 TUs —
**exactly one frame per TU**, against 4 frames for the grouped run. The 4-build per-TU stream
**decodes EXACT**.

| cell | TUs/build | build 1 | build 2 | build 3 | build 4 | cost vs `--gtu 112` |
|---|---:|---:|---:|---:|---:|---:|
| re2 | 72 | 430,143 | 14,076 | 14,076 | 14,112 | **1.30x** |
| fmt | 51 | 682,565 | 8,820 | 8,820 | 8,856 | **1.19x** |
| cereal | 80 | 438,330 | 12,088 | 12,088 | 444,132 | **1.69x** |
| leveldb | 94 | 774,297 | 13,872 | 13,875 | 13,911 | **1.31x** |

**Per-TU framing costs 1.19-1.69x the grouped wire** — the price of emitting immediately.

**But the cross-TU win survives intact**, which is the important part: a warm rebuild is
still ~3% of its cold build (re2 14,076 against 430,143). The long-range history match is
doing its job; what is lost is only intra-group batching and the per-frame overhead, now
paid once per TU instead of once per 112. On re2 that is ~195 B/TU warm against ~10 B/TU
grouped — the same ~185 B/TU frame cost, charged 72 times instead of once.

cereal's build-4 spike persists unchanged (444,132), as expected: it is the
no-copy-reanchoring behaviour documented above, which is independent of emission
granularity.

**This replaces the grouped line as the transport-correct GRZ2 result, and it is also the
correct learning curve — a real point at every TU rather than one per 112.** Per-TU curves:
`{re2,fmt,cereal,leveldb}.pertu.tsv`.
