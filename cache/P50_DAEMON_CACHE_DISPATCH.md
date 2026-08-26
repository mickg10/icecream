# Protocol-50 daemon cache-session dispatch

`p50_daemon_cache_dispatch` is the bounded daemon adapter between the real
`iceccd` ordinary-link event loop and a private sidecar relationship.  The
public TCP/unix listener remains in `iceccd`.

The adapter accepts only an exactly decoded `CACHE_SESSION` discriminator on a
negotiated Protocol-50 `MsgChannel`.  If the sidecar is unavailable it returns
before calling `release_fd_if_input_empty()`; the daemon closes the ordinary
link through its existing client teardown. If the release barrier succeeds,
the returned descriptor is moved immediately into one `FdHandoffSender` and
cannot be retried by this ordinary-link handoff. Every terminal sender result
closes or transfers that descriptor. The fresh private relationship is
one-shot, while the immutable current endpoint lease is retained for a later
TU to establish a different relationship.

Each request binds the supervisor/store generation, logical attempt, and a
monotonic nonzero request id. The fresh move-only `Connection` verifies exact
OS peer credentials and performs HELLO/HELLO_ACK against the
constructor-bound identity. The caller cannot replace that identity. Stale
identity, disconnect, duplicate, timeout, malformed ACK, or sidecar restart
therefore fails closed.
No cache bytes are read by this adapter, no second listener is created, and
Login advertisement remains owned by the production sidecar adapter.  It is
nonzero only while the exact supervised child, private socket inode, peer
credentials, HELLO identity, and public-listener observation are all current;
otherwise the daemon immediately projects `0/0/0`.

After HELLO/HELLO_ACK, dispatch sends an exact version-1 `Data` operation
envelope identifying `CacheSession`, the bound identity, and request id. Only
after that frame is accepted does it call `release_fd_if_input_empty()` and
send SCM_RIGHTS. A failed operation send leaves the ordinary descriptor owned
by the daemon; it cannot be mistaken for a compiler-input stream.

In the on-demand constructor, the private relationship source is an immutable
current READY-lease endpoint, not an attached descriptor. The endpoint binds
the exact generation/attempt-derived F-store GUID, socket path digest,
pathname device/inode, and expected PID+UID+GID. After the discriminator is
decoded, the daemon revalidates the current pathname, creates one fresh
AF_UNIX connection, and performs a
nonblocking connect, poll/SO_ERROR completion, exact PID+UID+GID
`SO_PEERCRED`, HELLO, and HELLO_ACK before it calls
`release_fd_if_input_empty()`.  Any failure in that prefix leaves the public
descriptor owned by `MsgChannel`; the temporary relationship is closed by
RAII.  A daemon-owned nonzero request id then names the one-shot handoff.

The HELLO send uses `Connection::send_until()` over one absolute wall-time
budget for the complete encoded frame, including partial writes.  Its bounded
ACK receive uses `Connection::receive_until()` with that exact unchanged
deadline, so a delayed ACK cannot restart the budget.  One-writer `Busy` and
SIGPIPE protections remain in force.

The positive path is wired through `DaemonSidecarAdapter`: real `iceccd`
launches the installed service, captures its exact READY/socket identity,
projects the supervisor's current immutable lease into `OnDemandEndpoint`,
advertises the public listener endpoint, and routes decoded `CACHE_SESSION`
here.  There is no cached-connection entry point: each TU creates a new
authenticated operation relationship, while every incarnation edge withdraws
the old lease.  After SCM_RIGHTS is accepted, the sidecar writes raw
`50 f0 00 01` on the adopted ordinary descriptor; the client waits for that
witness before emitting CacheWire.

This slice ends after the ordinary descriptor handoff. The reviewed
handoff-offer/trailing-byte barrier, `ATTACHMENT_PHASE_OPEN`, `WAITP50INPUT`,
and reverse sealed-input FD delivery are separate required production phases
and must not be inferred from an accepted generic handoff ACK.
