# Pass-boundary closure: proof batch

local-oracle's decisive objection to the first 4-pass harness was right. A single 4x stream
at `--gtu 112` lets a GRZ2 group span the cold build into the first warm rebuild, so the
cold build was **not complete or independently decodable at its own last TU** — only the
final residual closed. In the 44-cell set just 48 of 176 pass endpoints were GRZ2 close
points. The owner's cold+warm totals require a real close at every build boundary.

## The fix, and why it needs no code change

Neither `grz2g` nor `codec50` has a flush-at-boundary flag (checked: grz2g exposes only
`--gtu --graw --gadd --hist --anchor-budget --select --retry-test --curve`). So instead of
one 4x stream, each cell now encodes the **1x, 2x, 3x and 4x prefixes separately**:

- every prefix is a complete stream that ends exactly on a build boundary;
- every prefix is **independently decoded** (`grz2g dec`, which requires `END_FRAME` and
  verifies digests) and the decode is `cmp`'d byte-for-byte against its input;
- the warm state carried into build k is exactly the k-1 earlier passes present in the
  prefix — nothing more is retained and nothing is borrowed from the future;
- per-build wire = successive differences of closed totals.

`zstd-3/TU` and the fast interner need no such treatment — both are closed per TU by
construction (independent frame; per-TU byte-exact reconstruction) — but the harness still
verifies `sum_ok=1` and `verify=PASS` respectively.

**The 4x endpoints are unchanged.** re2/debian-gcc still totals 364,291 (GRZ2) and 458,487
(P29+BSC), exactly as the old harness reported. The fix does not move the final number; it
makes the three intermediate builds real.

## Four independently-closed builds, per cell

`per-build-totals.tsv` — build 1 is the cold build, 2-4 are warm rebuilds:

| cell | codec | build 1 | build 2 | build 3 | build 4 | cumulative | build 1 share |
|---|---|---:|---:|---:|---:|---:|---:|
| re2 / debian-gcc | GRZ2 | 362,280 | +721 | +576 | +714 | 364,291 | **99.45%** |
| re2 / debian-gcc | P29+BSC | 453,331 | +2,996 | +1,080 | +1,080 | 458,487 | 98.88% |
| re2 / fedora-clang-libcxx | GRZ2 | 396,468 | +723 | +577 | +714 | 398,482 | 99.49% |
| re2 / fedora-clang-libcxx | P29+BSC | 501,668 | +3,001 | +1,080 | +1,080 | 506,829 | 98.98% |
| fmt / debian-gcc | GRZ2 | 665,724 | +431 | +546 | +408 | 667,109 | **99.79%** |
| fmt / debian-gcc | P29+BSC | 1,006,187 | +1,766 | +765 | +765 | 1,009,483 | 99.67% |
| cereal / debian-gcc | GRZ2 | 374,637 | +789 | +788 | +641 | 376,855 | 99.41% |
| cereal / debian-gcc | P29+BSC | 466,708 | +1,352 | +1,126 | +1,126 | 470,312 | 99.23% |
| re2 / native-gcc11 | GRZ2 | 401,210 | +721 | +576 | +714 | 403,221 | 99.50% |
| re2 / native-gcc11 | P29+BSC | 436,132 | +2,998 | +1,080 | +1,080 | 441,290 | 98.83% |

Every one of these 40 build totals is a closed stream, and every GRZ2 stream decoded back to
its input byte-for-byte.

**With honest per-build closure the flattening claim gets stronger, not weaker.** The cold
build is **98.8-99.8%** of the four-build wire, and a warm rebuild costs **408-3,001 bytes**
against a 326 MB-1.5 GB input. Previously pass 1 could not even be stated for GRZ2 on small
projects; now it is the headline.

Both structural codecs flatten; **P29+BSC's second build is consistently the more expensive
one** (+1,766 to +3,001 vs GRZ2's +431 to +789), and its builds 3 and 4 are identical to the
byte (1,080 / 1,080 and 765 / 765), which is the signature of a fully-learned state emitting
a fixed per-build reference cost.

## Gates, now enforced rather than echoed

The old `selector_envcell.sh` was `set -uo pipefail` (no `-e`) and merely printed its checks,
and the join read none of them. `selector_passcell.sh` **exits nonzero and writes `FAIL`
into a per-cell `.status` file** on any of: missing `corpus.json`, payload sha mismatch,
extracted TU count != `tu_count`, extract failure, `zstd3 sum_ok != 1`, fused
`verify != PASS`, GRZ2 encode failure, **GRZ2 decode failure or decode != input**, P29
failure, P29 endpoint != `TOTAL=`, or P29 not `byte-exact=OK`. The status file is the
artifact a join must consult before publishing a cell.

Per-cell status files are committed here so each number is traceable to its checks.

## Still to come in this rework

- Per-TU curves rebuilt from the closed prefixes (stitching each build's own segment).
- Provenance manifest: payload hash, TU count, executable hash, exact command, output hash,
  decoder result per row.
- Empty-prediction training short-circuit; semantic names for the `corpusN` ids.
- The giants stay **paused** until the flushed harness is accepted, so no giant compute is
  spent on a grouping that would have to be redone.
