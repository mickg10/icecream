# Cold-to-warm transition matrix (rows 2-4), and what is missing for row 1

## Scoping first: two M5 mains, and only one has the transition

`cap_m5_main.cpp` -> **`cap_m5`** carries `--latejoin-at`; `cap_m5_stream_main.cpp` ->
**`cap_m5_stream`** (the one-pass overlap streamer that produced the 1.04-1.12 GB/s 8-F
numbers) **does not** -- it rejects the flag outright. The transition rows therefore run on
`cap_m5`, not on the one-pass streamer. Worth knowing before the two sets of numbers are
compared.

`--latejoin-at N` is exactly the requested 1->W transition
(`cap_m5_main.cpp:1184`, `initial = latejoin_at==UINT32_MAX ? workers : 1`), confirmed
empirically: 1 distinct worker below the boundary, N above it.

## Result: 96 runs, 4 corpora x 8 configurations x 3 reps, exact 96/96

Every run reports `exact=OK`, `FRAME_LEDGER closure=OK` and `TRANSACTIONS closure=OK`.

| corpus | TUs | 1-F wire | 8-F wire | **x vs 1-F** | 1->4 @50 | @100 | @200 | 1->8 @50 | @200 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| range-v3 | 259 | 1,820,664 | 7,076,645 | **3.89** | 2.41 | 2.37 | 1.93 | 3.86 | 3.02 |
| catch2 | 857 | 2,384,393 | 8,010,182 | **3.36** | 2.29 | 2.27 | 2.25 | 3.33 | 3.30 |
| rocksdb | 622 | 15,692,955 | 26,831,854 | **1.71** | 1.34 | 1.34 | 1.33 | 1.70 | 1.68 |
| eigen | 650 | 3,289,527 | 17,208,713 | **5.23** | 3.00 | 2.92 | 2.69 | 5.20 | 4.66 |

## Three findings

**1. Constraint (a) holds, verified.** Cumulative physical wire at TU50 is byte-identical
between the 1-F control and every transition that joins later -- 811,144 / 658,600 /
2,920,057 / 968,586 on the four corpora, matching on all of them. Running the full input
does not change the bytes emitted before the boundary.

**2. The cost is in HAVING consumers, not in WHEN they join.** Fanning out to 8 consumers
costs 1.71x (rocksdb) to 5.23x (eigen) against a single consumer. Delaying that fan-out
from TU50 to TU200 recovers only a little of it: eigen 1->4 improves 3.00x -> 2.69x,
range-v3 3.86x -> 3.02x on 1->8, while catch2 (2.29 -> 2.25) and rocksdb (1.34 -> 1.33)
are essentially flat. **A later transition is not a substitute for a shared state.** This
is the load-bearing measurement for the threshold question, and it says the threshold has
little leverage on its own -- what matters is whether the consumers share the store.

**3. Time-to-first-compiler-byte is 15-95 ms on every configuration.** The multi-second
head-of-line risk raised for GRZ's 112-TU group close does not appear on this path: M5
emits its first frame in tens of milliseconds regardless of consumer count or transition
point. That is a genuine difference between the two codecs and it means the 0.5 GB/s cold
throughput allowance is not hiding latency here.

## What is NOT covered, and why

- **Row 1, "compressibility-first / shared-state for the whole build", has no
  implementation.** There is no `--shared-state`. The nearest flag, `--cache50`, preloads
  half the regions (`if (int(id & 1) == options.cache50)`) -- a 50%-warm-cache experiment,
  not shared state across consumers. I have not guessed at the semantics; this needs
  local-oracle's shared-cache module.
- **Root/Need/Fill is per RUN, not per TU.** `FRAME_LEDGER` gives the split
  (`Root=… Need=… Fill=… Ack=… Rejoin=…` with counts and `closure=OK`), but the per-TU
  curve carries only `logical, physical, worker, raw, wire, latency_ns, cumulative_raw,
  cumulative_wire`. Per-TU definition/missing-definition bytes need an instrumentation
  change to local-oracle's codec -- doable observation-only, byte-exact verified, the same
  pattern used for the P29 stage split, but it is their source.
- **Constraint (b), the cold codec decoding into the same canonical store, is a codec
  property and cannot be satisfied by a harness.** The cold codecs and M5's warm lane are
  separate programs today. Nothing measured here establishes state continuity, and finding
  2 above is precisely why it matters: the penalty being structural rather than temporal
  means a second incompatible cache would not be recovered by any choice of boundary.

Reproduce with `selector_coldwarm.sh <corpus>`; per-run evidence under
`~/selbind/coldwarm/` on quietbox2 (curve, stdout ledger, stderr, `/usr/bin/time` resource
line, every repetition retained).
