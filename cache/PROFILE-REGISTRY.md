# CacheWire Profile-ID Registry (S0.3, ratified)

**Status:** consolidates the registry ALREADY LANDED in the reviewed objects below; no new
allocation. Wording incorporates local-oracle's S0.3 corrections (bus, 2026-08-23).
BigOracle concurrence: pending backport.

## Normative production anchors (define the IDs/bits/payload law)
- `cache/protocol50.h`: `enum class ProfileId : uint16_t { P29 = 1, ZSTD_TU = 2, GRZ = 3, Z3_LONG = 4, Z3_SHARED_LONG = 5 }`.
- `services/comm.{h,cpp}`: Login-advertisement bit positions, masks, and the `valid_payload` law.

Enforcement anchors (bind the above): `unittests/p50cacheadvertisement.cpp` (behavioral),
`unittests/p50cacheadvertisement-source.sh` (textual: stable-ID counts, declared-labels-codec-free,
selection-slice absence).

## The registry (frozen)
| ProfileId | Name | Login bit | Status | Owner semantics |
|---|---|---|---|---|
| 1 | `P29` | `CACHE_PROFILE_P29` (1<<0) | declared/known wire profile; the research `CW_P29_BSC_Z3_M64` codec is the intended S6 product port, not presently landed/runnable in the daemon path | per-TU serializer (owner's "p20/p23") |
| 2 | `ZSTD_TU` | `CACHE_PROFILE_ZSTD_TU` (1<<1) | **the sole currently Login-advertisable/runnable profile** (`CACHE_ADVERTISABLE_PROFILE_MASK`) | whole-TU zstd, stateless |
| 3 | `GRZ` | `CACHE_PROFILE_GRZ` (1<<2) | declared/known wire profile; the GROUP-RLZ(+residual/BWT) codec is the intended S6 product port, not presently landed/runnable in the daemon path | GROUP-RLZ long-range |
| 4 | `Z3_LONG` | `CACHE_PROFILE_Z3_LONG` (1<<3) | declared-only; codec-free by source gate | **= product/plan `ZSTD_ROUTE`**: one continuing level-3 long-window stream per C/F relationship (per-route in the product topology), with TU flush boundaries |
| 5 | `Z3_SHARED_LONG` | `CACHE_PROFILE_Z3_SHARED_LONG` (1<<4) | declared-only; codec-free by source gate; deferred/sim-first | **= product/plan `ZSTD_COHORT`**: shared raw-content reference/out-dictionary plane plus a thin retained per-F level-3 window |

`z3_shared_long_b1` remains an experiment/control label and must never receive a product ProfileId.

## Rules
1. **IDs 1–5 are permanently allocated as above; never reuse.** The next allocation is >= 6 and
   requires a reviewed successor of this document.
2. **DECLARED != ADVERTISABLE != SESSION-SELECTED.** Declaring a name reserves the ID and label.
   A profile may be Login-ADVERTISED only when its codec reconstructs input byte-exact
   (today: `ZSTD_TU` only).
3. **Login advertisement is not cache-session negotiation.** The P50 Login mask is the F endpoint
   availability advertisement retained by the scheduler; it does not itself negotiate a cache
   session. A cache session selects the intersection of the C `SESSION_HELLO.supported_profiles`
   offer and the F endpoint's actually enabled/admissible profile set. Every `TX_BEGIN.profile`
   must be in that session-selected set.
4. **Wire naming:** the cache channel's protocol constant 50 is "CacheWire v1"
   (`CACHE_WIRE_PROTOCOL_V1`) in user-facing output — distinct from the ordinary-link
   `PROTOCOL_VERSION 50` (assignment identity).
5. **`valid_payload` law (Login tail):** wholly-absent (0/0/0) or fully-valid-present (bounded
   port, CacheWire v1, nonzero mask within the advertisable mask); anything else rejects the login.
