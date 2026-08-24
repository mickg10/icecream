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

The payload is capped at 64 KiB.  A reader validates the cap before allocating
and requires an exact frame; malformed, truncated, extra-byte, unsupported,
and stale-identity inputs are refused.  A handshake payload is exactly one
role byte (`daemon` or `sidecar`) and is checked against both identity fields.

Each relationship has one `SingleWriter`.  It owns the only write path and
rejects a concurrent send with `Busy`; it intentionally has no implicit queue
or unbounded buffering.  The eventual supervisor/async adapter is responsible
for one bounded queue and orderly shutdown around this boundary.

On Linux, peer verification uses `SO_PEERCRED`.  The provider argument is a
test/integration seam; absent a provider, unsupported or failed OS credential
queries return `PeerCredentialUnavailable` and never grant access.

