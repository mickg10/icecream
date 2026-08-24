# Protocol-50 ordinary-link `CACHE_SESSION`

This slice defines the production transition from an ordinary Protocol-50
Icecream link to CacheWire on the same TCP socket.  The C client sends one
ordinary `CACHE_SESSION`; F proves a clean parser boundary, transfers that
exact descriptor to its authenticated sidecar with `SCM_RIGHTS`, and the
sidecar returns one raw READY witness on the now-owned socket.  C cannot emit
CacheWire until it validates that witness.

## Wire and compatibility fixtures

`CACHE_SESSION` is an ordinary four-byte-length frame whose four-byte payload
is only the big-endian message discriminator:

```text
00 00 00 04  50 f0 00 00
```

The sidecar-ownership transition witness is an unframed four-byte network-order
constant on that same socket:

```text
50 f0 00 01
```

The ordinary payload after the discriminator is empty.  The discriminator is
private to Protocol 50 (`0x50f00000`), outside the historical vocabulary and
the Protocol-49 private block.  Both encode and decode require the negotiated
ordinary protocol to equal 50; P43/P48/P49 continue to emit/decode their
retained bytes and reject this type.

The separate CacheWire `SessionHello` follows READY and binds `C_GUID` there.
Neither `CACHE_SESSION` nor READY carries a mutable identity.  READY is a fixed
ownership-transition marker, not an ordinary frame and not a CacheWire field.

## Bounded release contract

`MsgChannel::release_fd_if_input_empty()` returns the server descriptor and
sets `fd = -1` only immediately after a successful `CACHE_SESSION` decode.  It
requires exact Protocol 50, a live non-error channel, no internal buffered or
read-ahead byte, `NEED_LEN` at the next ordinary boundary, no EOF, no pending
output/frame, and an fd still owned by the channel.  A non-consuming
`MSG_PEEK|MSG_DONTWAIT` closes the kernel-queue/EOF check without consuming a
byte.  Any refusal retains ownership.

The client mirror is
`release_fd_after_cache_session_ready(absolute_deadline)`.  A fully flushed
`CACHE_SESSION` arms it once.  It reads exactly `50 f0 00 01` with nonblocking
I/O and poll under the caller's unchanged steady-clock deadline, rejects EOF,
wrong/partial/late READY, and rejects any byte already queued behind READY.
Every call consumes the arm even on failure; failure retains the descriptor so
normal channel teardown closes it, while success returns it and sets `fd = -1`.
Thus a late token cannot resurrect a failed connection and the sender's retry
must obtain a fresh ordinary link.

Each directional release arm is message-specific.  The inbound arm is installed
only after exact decode and the outbound arm only after a complete synchronous
send.  Both are cleared by the next `get_msg()` use or any later ordinary send
attempt; a later generic flush cannot resurrect an arm and there is no generic
clean-boundary escape.  The F sidecar keeps its authenticated daemon control
relationship across idle periods, but each subsequently received operation,
descriptor handoff, and READY send receives a fresh bounded operation budget.

## Wire audit delta (owner ruling)

| Bucket | Delta |
| --- | --- |
| **BOUND** | The `50 f0 00 00` discriminator is bound to exact negotiated Protocol 50 and a clean ordinary-parser boundary.  Acceptance of `50 f0 00 01` is bound to the exact descriptor already accepted from the authenticated F daemon by the sidecar; C retains ownership and emits no CacheWire until that witness is exact and alone. |
| **WIRE-PLACEHOLDER / DERIVED-GUARD** | READY is a fixed transition/liveness token with no mutable identity.  Ordinary length/type checks, release arms, read-ahead/EOF/error checks, and absolute deadlines are derived guards rather than payload fields. |
| **CURRENTLY MODEL-UNREPRESENTED** | None added by this transition.  CacheWire `SessionHello` remains the authority that binds `C_GUID`; READY deliberately makes no identity or transaction claim. |

Fixture delta: the ordinary request remains byte-identical and the raw
`50 f0 00 01` READY fixture is added; retained P43/P48/P49 fixtures are
unchanged.  The committed `p50cachesession` behavioral matrix covers both
directional detaches and fd continuity, exact-once transfer, no CacheWire before
READY, exact/wrong/partial/missing/late/extra-byte READY rows, split reads,
early-byte and partial/complete next-frame barriers, EOF, pending/prior output,
arm non-resurrection, and Protocol-49 send/decode refusal.
`p50cachesession-source.sh` is only a supplemental source-shape audit; behavior
remains the deletion/mutation-sensitive gate.
