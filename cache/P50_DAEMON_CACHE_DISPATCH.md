# Protocol-50 daemon cache-session dispatch

`p50_daemon_cache_dispatch` is the bounded daemon adapter between the real
`iceccd` ordinary-link event loop and a private sidecar relationship.  The
public TCP/unix listener remains in `iceccd`.

The adapter accepts only an exactly decoded `CACHE_SESSION` discriminator on a
negotiated Protocol-50 `MsgChannel`.  If the sidecar is unavailable it returns
before calling `release_fd_if_input_empty()`; the daemon closes the ordinary
link through its existing client teardown.  If the release barrier succeeds,
the returned descriptor is moved immediately into one `FdHandoffSender` and
cannot be retried.  Every terminal sender result closes or transfers that
descriptor, and the controller drops the private relationship after the
one-shot exchange.

Each request binds the supervisor/store generation, logical attempt, and a
monotonic nonzero request id.  Peer credentials must already be verified on
the exact move-only `Connection`, and `attach_authenticated()` then performs
the HELLO/HELLO_ACK exchange itself against the constructor-bound identity.
The caller cannot replace that identity.  Stale identity, disconnect,
duplicate, timeout, malformed ACK, or sidecar restart therefore fails closed.
No cache bytes are read by this adapter, no second listener is created, and
Login advertisement remains the existing `0/0/0` until a separate reviewed
READY integration.

After HELLO/HELLO_ACK, dispatch sends an exact version-1 `Data` operation
envelope identifying `CacheSession`, the bound identity, and request id. Only
after that frame is accepted does it call `release_fd_if_input_empty()` and
send SCM_RIGHTS. A failed operation send leaves the ordinary descriptor owned
by the daemon; it cannot be mistaken for a compiler-input stream.

The HELLO send uses `Connection::send_until()` over one absolute wall-time
budget for the complete encoded frame, including partial writes.  Its bounded
ACK receive uses `Connection::receive_until()` with that exact unchanged
deadline, so a delayed ACK cannot restart the budget.  One-writer `Busy` and
SIGPIPE protections remain in force.

The positive real-daemon sidecar path is **not wired yet**.  The daemon starts
with the controller unavailable because the lifecycle-only
supervisor/service checkpoint intentionally does not yet expose a live
connection to this object.  A later adapter may connect the private socket,
verify OS peer credentials, and call `attach_authenticated()`; the method
itself proves the current sidecar identity before retaining the relationship,
without changing ordinary-link parsing or ownership rules.
