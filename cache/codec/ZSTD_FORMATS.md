# ZSTD_TU and ZSTD_ROUTE formats

These are profile IDs 2 and 3 in [CacheWire revision 1](../P50_PROTOCOL.md).
The implementation is [p50_zstd.h](../p50_zstd.h) and
[p50_zstd.cpp](../p50_zstd.cpp). Both transport exactly one terminated zstd
frame per TU. Neither is the ordinary legacy FileChunk compression format.

## Shared wire contract

TX_BEGIN's profile and BODY encoding must both equal the selected profile ID.
BODY is the complete zstd frame, with no codec-local header or separate
dictionary message. It may span outer BODY frames. The descriptor contains
the encoded length/digest and decoded length; decoded length must equal
TX_BEGIN's exact raw length. The complete BODY also participates in the
[outer transaction digest](../P50_PROTOCOL.md#transaction-fields-and-digests).

The exchange is TX_BEGIN, BODY, then TX_COMMIT. NEED and FILL are invalid
for both ZSTD profiles. Empty encoded BODY is invalid even when the raw input
is empty.

A receiver checks:

- selected/negotiated profile and encoding, nonterminal route sequence,
  local encoded/raw bounds and addressable lengths;
- exact encoded length, BODY digest and transaction digest;
- a zstd decoder window no larger than the configured cap;
- exactly one complete frame: no truncation, concatenated frames, trailing
  bytes or output exceeding the declared length;
- exact decoded length and raw digest;
- commit identity and the recomputed post-state digest before promotion.

Compression parameters are not additional CacheWire fields. A decoder accepts
a compatible frame within its local bounds; it does not need the encoder's
compression level to decode it. The raw digest is always checked independently
of any zstd frame checksum.

## ZSTD_TU: independent source

Profile and encoding are 2. The encoder resets the context and parameters
for every TU, sets compression level and window log, and calls
`ZSTD_compress2`. There is no predecessor prefix and no cross-TU dictionary.
Context reuse is an allocation optimization, not a dependency between frames.

The codec and client configuration default to compression level 1. The
codec API allows other levels in the linked zstd library's accepted range.
Default window log is 27 (128 MiB). Parameters not explicitly set after reset
use the linked library's defaults. Raw empty input is supported by the codec
and is represented by a nonempty complete frame.

One transaction is active at a time. A second TX_BEGIN while one is active,
NEED/FILL or a BODY overflow terminates the dialogue. Materialization precedes
commit; an exact successful commit clears the active transaction. Disconnect
makes that dialogue terminal. Retry requires a reconciled endpoint and a
complete immutable TU, not continuation of a half-decoded frame.

## ZSTD_ROUTE: committed raw prefix

Profile and encoding are 3. The encoder requires compression level **3**;
other constructor values are rejected. It resets zstd state and attaches
the exact committed raw predecessor suffix through `ZSTD_CCtx_refPrefix`.
The decoder supplies that same suffix through `ZSTD_DCtx_refPrefix`.
The prefix is not sent separately.

Let H be the retained committed suffix and R the current exact raw TU:

```text
L = min(max_history_bytes, 2^max_window_log)
BODY = one complete zstd frame for R using H as its prefix
after an exact commit: H := suffix_L(H || R)
```

The initial prefix is empty. Default L is 128 MiB. The prefix is single-use
at the zstd API boundary and is explicitly attached for each frame; this is
not one endless zstd stream. A later frame cannot necessarily decode without
the correct committed prefix merely because it has its own frame boundary.
This codec requires nonempty raw input.

A route preserves its history nonce. After the first committed TU, relative
sequence advances by exactly one and TX_BEGIN's pre-state digest must equal
the previous commit's post-state digest. A candidate's raw output remains
tentative until the exact commit matches. Commit appends it to H and trims
the old prefix; rejecting or discarding the candidate cannot advance H.

At the low-level ZstdRouteDialogue boundary, disconnect clears its local
history and makes the object terminal; reset is allowed only from Terminal.
This is distinct from the endpoint's retained route/commit ownership: an
ordinary socket disconnect discards tentative work but retains the committed
route dialogue where valid; it does not call the low-level disconnect simply
because that socket closed. Reconnect must use the endpoint's exact
reconciliation rules, never assume an empty new dialogue represents an existing route.
The codec also supports reconstruction from committed envelopes. Changed
store identity or authorized history reset establishes a new cold route;
a missing prefix is not a license to decode a warm frame as cold.

## Limits and defaults

`ZstdTuLimits` is shared by both profiles:

| Setting | Default or requirement |
|---|---|
| max_window_log | 27; validated range 10..31 |
| max_history_bytes | 128 MiB; nonzero and addressable |
| Endpoint encoded BODY cap | 64 MiB |
| Endpoint raw TU cap | 2 GiB |

These are configurable local limits, not negotiated compression parameters.
The history field is validated by the shared limits type even for ZSTD_TU,
which does not use predecessor history. The standalone `encode_zstd_tu`
helper defaults its byte caps to u64 maximum; that is not the endpoint's
admission policy. The route helper defaults to the endpoint's 64 MiB / 2 GiB
byte caps.

Retained input, pending transactions, decoder windows, route histories and
preparation queues have separate aggregate accounting. In particular, the
separate 512 MiB pending-encoded and retained-encoded defaults do not change
the default 128 MiB per-route prefix. Larger research windows are not current
default settings.

## Verification

The `p50zstd`, `p50profile`, endpoint and simulator tests check round trips,
limits, incorrect digests, frame closure, route order, reset and commit
behavior. The real compile gate exercises selected profiles separately.
See [test entrypoints](../../farmharness/integration/tests/README.md).

Frozen historical zstd tuples remain described in [FORMAT.md](FORMAT.md).
They preserve their original configuration and expected decoded bytes; they
must not be silently relabelled as current product defaults. Compressed bytes
can differ across zstd versions, so same-build byte reproducibility and
cross-version exact decoded-content tests are distinct checks.
