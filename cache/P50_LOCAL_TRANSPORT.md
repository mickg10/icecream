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
