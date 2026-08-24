# Protocol-50 cache service (S2 bounded sidecar bridge)

`icecc-cache-service` is an installed sidecar control process. `iceccd`
continues to own the public TCP listener and transfers one clean-boundary
cache-session descriptor over the private authenticated AF_UNIX relationship.
The service does not create a public listener and does not own login or
advertisement state; public advertisement remains `0/0/0` in this mechanism
slice.

## Invocation

Every identity and path is explicit:

```
icecc-cache-service \
  --socket /run/user/4103/icecc/cache-service.sock \
  --peer-uid 4103 --peer-gid 3513 \
  --generation 7 --attempt 1
```

The socket path is absolute, its existing parent is owned by the effective
uid with exactly mode `0700`, and the new node has exactly mode `0600` and
`CLOEXEC`. Existing nodes are never replaced. The peer must match both
configured uid and gid using `SO_PEERCRED`; unavailable credentials fail
closed.

The service refuses uid or gid zero. A privileged launcher must provide both
nonzero `--drop-uid` and `--drop-gid`; the process clears supplementary groups,
sets gid then uid, and proves real/effective/saved ids before binding.

The installed F-store GUID is the complete authenticated incarnation:
generation and attempt are encoded independently. Consequently a restart
with the same generation and a new attempt cannot reopen the prior empty-store
identity.

## Readiness, signal shutdown, and control

`ICECC_CACHE_SERVICE_READY_FD` is mandatory and names an already-open decimal
descriptor. After privilege checks, bind, listen, and listener identity capture
complete, the service writes exactly `READY\n` and closes the descriptor. A
short/interrupted/failing write exits nonzero and performs identity-checked
listener cleanup; no earlier failure emits readiness.

SIGTERM and SIGINT handlers perform only the async-signal-safe action of
setting a flag and writing a byte to an internal nonblocking wake pipe. The
normal service thread drains that pipe, calls `SidecarRuntime::stop()`, and
joins the connection worker. `stop()` closes the duplicated control wait
descriptor and posts cancellation of the active adopted TCP socket onto its
Asio owner context. Thus both an in-flight `FdHandoffReceiver` and an active
`run_adopted` dialogue terminate within the bounded shutdown gate without C++
object work in the signal handler.

The listener accepts at most one worker connection at a time. Each connection
must pass peer-credential verification and one exact `Hello` from `Daemon`
with the configured nonzero generation and attempt; the service returns one
`HelloAck` from `Sidecar`. The worker then admits exactly one handoff for its
monotonically increasing request id, ACKing only after ownership and
`CLOEXEC` have been proven. Wrong generation, attempt, request id, missing or
extra descriptors, truncation, trailing data, disconnect, timeout, adoption
failure, and shutdown are fail-closed and close all owned descriptors.

After one adopted dialogue returns, the same `SidecarRuntime` may process the
next authenticated control connection and request id. Endpoint session leases,
temporary sockets, and handoff busy state are reset before the next session;
the shared `P50ServerEndpoint::run_adopted` reducer remains the sole endpoint
state owner.

The generic `p50_fd_handoff` helper remains a separate ownership primitive. It
does not know the ordinary-link codec, create a listener, or advertise an
endpoint. This S2 mechanism slice contains no daemon integration, nonzero
advertisement, or production login transition.

Shutdown compares the open listener's `fstat` device/inode with the pathname's
`lstat` device/inode before unlinking. A replacement node is never removed.
