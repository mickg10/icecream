# Protocol-50 local transport (S2 groundwork)

This library is the bounded wire boundary for the future daemon/sidecar
relationship.  It does not start a sidecar, change supervisor policy, or
advertise a cache endpoint.  Login advertisement remains the existing fixed
`0/0/0` value until a separately reviewed READY policy exists.

## Frame contract

Every `SOCK_STREAM` frame starts with a 28-byte big-endian header:

| Bytes | Field |
| ---: | --- |
| 0..3 | ASCII magic `P50L` |
| 4..5 | protocol version (`1`) |
| 6..7 | message type (`HELLO`, `HELLO_ACK`, `DATA`, `GOODBYE`) |
| 8..11 | payload length |
| 12..19 | connection generation |
| 20..27 | attempt identity |

The payload is capped at 64 KiB.  A reader validates magic, version, type, and
the cap before allocating or waiting for a payload, and requires an exact
frame; a clean EOF at a header boundary is distinct from partial-header and
partial-payload truncation.  Malformed, truncated, extra-byte, unsupported,
and stale-identity inputs are refused.  A handshake payload is exactly one
role byte (`daemon` or `sidecar`) and is checked against both identity fields.

Each relationship has one move-only `Connection`.  It owns the descriptor and
the only framed write path, rejecting a concurrent send with `Busy`; there is
no public raw frame writer or second writer object.  The eventual
supervisor/async adapter is responsible for one bounded queue and orderly
shutdown around this boundary.  Listener and accepted/client descriptors are
`CLOEXEC`; SIGPIPE is suppressed with `MSG_NOSIGNAL` or `SO_NOSIGPIPE`, and
platforms with neither fail closed.

`Connection::send_until(frame, deadline)` is the bounded control-handshake
writer.  It encodes the complete frame before writing and carries one absolute
`steady_clock` deadline across every partial write.  Polling plus nonblocking
writes prevents a writable hint from turning into a blocking send; timeout
returns `Status::Timeout` and preserves the one-writer gate.  Per-call
`MSG_DONTWAIT` avoids mutating `O_NONBLOCK` on the shared open file
description; platforms without that primitive fail closed.  Poll error and
hangup bits are checked before output readiness, and SIGPIPE protection
remains the same as the ordinary writer.

The connection also exposes peer verification and both
`receive_with_timeout()` and `receive_until()` framed receives without
exposing a borrowed descriptor.  The absolute form covers the complete header
and payload under one unchanged deadline, allowing a synchronous control owner
to wake and close a stalled relationship during signal-driven shutdown.

Socket paths must be absolute, contain no embedded NUL, and reside in a
directory owned by the effective user with exact mode `0700`.  The listener
never replaces an existing node; its socket node is owned by the effective
user with exact mode `0600`, which clients also verify before connecting.
After a successful `bind`, any later setup failure returns
`ListenerNodeLeftForCleanup` and leaves the node untouched.  Cleanup must be
performed by the supervisor using its already-validated private runtime
directory, never by a pathname unlink in this helper.  The test-only
compile-time seam `ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS` exercises this
retained-node result deterministically.  It is absent from the production
library, so inherited environment variables cannot force a failure or cause
an unbounded wait in `listen_unix`.

On Linux, peer verification uses `SO_PEERCRED`.  The provider argument is a
test/integration seam; absent a provider, unsupported or failed OS credential
queries return `PeerCredentialUnavailable` and never grant access.

## Accepted-FD handoff

`p50_fd_handoff.h` adds a generic, one-shot daemon-to-sidecar handoff for a
descriptor accepted by a later ordinary-link listener.  It is usable only
after `Connection::verify_peer_credentials` succeeds on the same connection;
the connection remembers that proof and move operations clear it.  The
handoff does not create a listener, advertise an endpoint, or know a store,
codec, or cache-session protocol.

The raw exchange is a fixed 40-byte `P50F` record (version 1, request/ACK/NACK,
generation, attempt, request id, and result code) carried on the already
private authenticated control stream.  The request carries exactly one
`SCM_RIGHTS` descriptor.  Both directions accumulate arbitrary stream
fragments without repeating ancillary rights after a positive short write;
already-queued trailing data is rejected as forbidden pipelining.  The
receiver rejects malformed lengths/types,
missing or extra descriptors/control messages, `MSG_TRUNC` and `MSG_CTRUNC`,
stale identity, replay, disconnect, and deadline expiry.  Received descriptors
are `CLOEXEC` via `MSG_CMSG_CLOEXEC`, with an `fcntl` fallback, before adoption.
The receiver moves the descriptor into its owner before emitting ACK; the
sender closes its copy only at a terminal state and never retries a sent
request.  NACK and all failed terminal paths close without leaking.

The deadline is an absolute `steady_clock` deadline covering the complete
send/response exchange.  A receiver admits one request per instance.  Because
this is a raw stream exchange rather than a framed message, its control
connection must be dedicated to this exchange (no interleaved frame or second
handoff); ordinary-link integration is intentionally a later slice.
