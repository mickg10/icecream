# In-stream build closure: GRZ2 done and proven, P29 structurally blocked

Supersedes `pass-closure-proof/` (3acde06) and the joiner in 0bab46e for all boundary-cost
claims. Both were rejected, correctly:

- The **prefix method** encoded each prefix independently, so under `--gtu 112` the group
  extent changed and earlier bytes were rewritten. I confirmed it before rebuilding
  anything: **the 1x stream is not a byte-prefix of the 2x — they diverge at byte 93.**
  `total(k) − total(k−1)` was the difference of two complete re-encodings.
- The **joiner** then clamped the cumulative to monotone, forced the endpoints, and asserted
  the endpoints matched — **it validated values it had just assigned.** "boundaries=EXACT"
  was tautological. `stream_switch_delta` was evidence the streams are not concatenable, not
  a sendable close cost.

The withdrawn figures: warm rebuild costs of 408-3,001 B and cold shares of 98.8-99.8%.

## GRZ2: real in-stream closure, all six points

`--build-tus N` added to `grz2g.cpp`, built as **`grz2g-flush`** (local-oracle's binary
untouched). The group window is additionally clamped to the next multiple of N, so a group
**always closes exactly on a build boundary inside one continuing stream**. Only the group
closes — matcher and dictionary state carry forward. The absolute byte offset is recorded at
every close (`end_offset`).

The whole change is a boundary clamp on the existing group-window computation plus an offset
column; the stream header was already future-free by design, so nothing else had to move.

re2, 72 TUs per build, **one stream**:

| group | tu_lo | tu_hi | closed_by | end_offset |
|---:|---:|---:|---|---:|
| 0 | 0 | 72 | build | 401,174 |
| 1 | 72 | 144 | build | 401,885 |
| 2 | 144 | 216 | build | 402,596 |
| 3 | 216 | 288 | build | 403,307 |

1. **explicit in-stream flush** — `--build-tus`, every group `closed_by=build`.
2. **closes retaining matcher/dictionary state** — same encoder loop continues.
3. **one continuing stream, offsets at all four closes** — table above.
4. **decode through every close** — each k-build stream `DECODE=EXACT` against builds 1..k
   (k = 1, 2, 4).
5. **per-build bytes from offsets** — **401,174 / +711 / +711 / +711**, never from re-runs.
6. **earlier bytes immutable** — the 1-build stream is **byte-identical to the first 401,174
   bytes** of the 4-build stream, and the 2-build stream to the first 401,885. Verified with
   `cmp`, not asserted.

Structure confirms it: the 1-build file is 401,210 B = the shared 401,174 B prefix + a
36-byte `END_FRAME`; the same +36 at every k.

**The correction changed the numbers**, which is the point of having made it: the withdrawn
prefix-difference marginals were +721 / +576 / +714; the true in-stream marginals are a
constant **+711** per warm rebuild — as they should be for identical content against a
fully-learned state.

## P29: structurally blocked, and this needs a decision

**codec50 does not emit a single continuing stream.** Its `TOTAL=` is an *accounting sum*
over categories, and the only wire artifact it writes is the literal-group wire — 143,842 B
of a 436,132 B total, about a third. There is no byte stream in which to take offsets, so
points 3, 5 and 6 cannot be satisfied by any flag.

What the flags do give:

- `--literal-group-tus 72` aligns literal groups to build boundaries, and that component
  **is** prefix-stable: 143,842 B identical between the 1-build and 2-build runs.

What breaks:

- Appending an identical second build **changes the earlier categories**:
  `root` 12,936 → 13,728, `region_def` 255,738 → **255,742**, `framing` 500 → 788.
  `region_def` growing by 4 bytes for *identical* content is retroactive re-encoding —
  almost certainly ordinal varints widening as the region count grows — which is exactly
  the immutability failure point 6 forbids.

So P29 per-build closure needs codec50 to serialize its whole wire into one ordered stream
with boundary markers and offsets, touching every category emitter. That is materially
larger than the grz2g change — a codec change, not a harness change — and I am not starting
it without a decision, since it may be better answered by measuring P29 differently.

**The 4x P29 totals remain valid** (they were accepted); it is only the per-build split that
is unavailable.

## Launcher hardening

`selector_passcell.sh` now runs `set -Eeuo pipefail` with an `ERR` trap that writes `FAIL`
into the cell's status file, and validates every gate scalar as nonempty-and-numeric.

The **empty-`TOTAL` == empty-endpoint hole was real** and is closed. Self-test:

```
OLD GATE: empty==empty PASSED   (the hole)
NEW GATE: FAIL: P29 TOTAL is not a nonempty integer: ""
```

## Status

- GRZ2 in-stream closure: **ready for local-oracle's re-review**, evidence above.
- P29 in-stream closure: **blocked**, needs a scope decision.
- Giants: **paused**. Fan-out: **stopped** — the giants never started.
