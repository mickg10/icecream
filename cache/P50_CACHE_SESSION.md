# Protocol-50 ordinary-link `CACHE_SESSION`

This slice adds one ordinary Icecream message solely to mark the boundary at
which a successfully negotiated Protocol-50 link may transfer its still-owned
descriptor to a later CacheWire implementation.  It does not start a
sidecar, pass `SCM_RIGHTS`, advertise an endpoint, or change any daemon/client
call site.

## Wire and compatibility fixture

`CACHE_SESSION` is an ordinary four-byte-length frame whose four-byte payload
is only the big-endian message discriminator.  The exact fixture is:

```text
00 00 00 04  50 f0 00 00
```

The payload after the discriminator is empty.  The discriminator is private
to Protocol 50 (`0x50f00000`), outside the historical vocabulary and the
Protocol-49 private block.  Both encode and decode require the negotiated
ordinary protocol to equal 50; P43/P48/P49 continue to emit/decode their
retained bytes and reject this type.

The separate CacheWire `SessionHello` follows later on the transferred fd and
binds `C_GUID` there.  `CACHE_SESSION` has no CacheWire fields, placeholders,
or model-unrepresented payload values.

## Bounded release contract

`MsgChannel::release_fd_if_input_empty()` returns the descriptor and sets
`fd = -1` only immediately after a successful `CACHE_SESSION` decode.  It
requires exact Protocol 50, a live non-error channel, no internal buffered or
read-ahead byte, `NEED_LEN` at the next ordinary boundary, no EOF, no pending
output/frame, and an fd still owned by the channel.  A non-consuming
`MSG_PEEK|MSG_DONTWAIT` closes the kernel-queue/EOF check without consuming a
byte.  Any refusal returns `-1` and retains both ownership and input bytes;
the destructor therefore cannot close a transferred descriptor.

The release arm is message-specific.  It is installed only by a successful
`CACHE_SESSION` decode and cleared by the next `get_msg()` decode/use or by a
successful transfer; there is no generic clean-boundary escape.

## Wire audit delta (owner ruling)

| Bucket | Delta |
| --- | --- |
| **BOUND** | The discriminator is bound to exact negotiated Protocol 50; the release arm and fd ownership transfer are bound to a successful decode plus explicit parser/input/output boundary predicates. |
| **WIRE-PLACEHOLDER / DERIVED-GUARD** | None in the message wire.  The length, exact-type check, read-ahead barrier, EOF/error check, and pending-output check are parser guards, not payload fields. |
| **CURRENTLY MODEL-UNREPRESENTED** | None for this empty discriminator message.  The separate CacheWire `SessionHello` binds `C_GUID` in its own later slice; this ordinary frame does not claim to carry or model it. |

The retained P43/P48/P49 ordinary fixtures are unchanged.  The committed
`p50cachesession` behavioral matrix covers successful detach and fd continuity,
exact-once transfer, other-message refusal, split reads, early-byte and
partial/complete next-frame barriers, EOF, pending output, and Protocol-49
send/decode refusal.  `p50cachesession-source.sh` is only a supplemental
source-shape audit; behavior remains the deletion/mutation-sensitive gate.
