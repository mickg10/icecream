# Protocol-50 cache service (S2 bounded sidecar bridge)

`icecc-cache-service` is an installed sidecar control process. `iceccd`
continues to own the public TCP listener and transfers one clean-boundary
cache-session descriptor over the private authenticated AF_UNIX relationship.
The service does not create a public listener and does not own login or
advertisement state.  The daemon adapter alone projects either the fully
validated public endpoint or `0/0/0` from the supervised service state.

## Invocation

The command-line form remains an explicit standalone/test fallback:

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
sets gid then uid, and proves real/effective/saved ids before adopting the
pre-bound listener.

The installed F-store GUID is the complete authenticated incarnation:
generation and attempt are encoded independently. Consequently a restart
with the same generation and a new attempt cannot reopen the prior empty-store
identity.

Under the supervisor, seven environment fields form one immutable all-or-none
launch tuple:

```
ICECC_CACHE_SERVICE_READY_FORMAT=2
ICECC_CACHE_SERVICE_EXPECTED_GENERATION=...
ICECC_CACHE_SERVICE_EXPECTED_ATTEMPT=...
ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID=...
ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID=...
ICECC_CACHE_SERVICE_EXPECTED_SOCKET=...
ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST=...
```

Any partial tuple, reserved/wrapping identity, noncanonical GUID, path, or
digest exits before listener adoption. A complete valid tuple overrides stale
command-line identity/path values; the supervisor, not a static CLI pathname,
owns the incarnation. The service uses that effective identity throughout
runtime and request handling.

The supervisor creates the private `0700` lease directory and binds the unique
AF_UNIX listener before `fork()`. It passes the descriptor as
`ICECC_CACHE_SERVICE_LISTENER_FD`; the child clears `CLOEXEC` on that one
descriptor, drops privileges, and adopts it with `fstat(2)`. Structured mode
never calls `bind(2)` or unlinks the pathname. The parent performs
identity-checked deletion after the supervised process group is proven dead.

## Readiness, signal shutdown, and control

`ICECC_CACHE_SERVICE_READY_FD` is mandatory and names an already-open decimal
descriptor. After privilege checks and listener identity capture complete, the
service writes legacy `READY\n` only for the standalone form. A structured
supervisor launch writes the exact READY-v2 generation, attempt, PID,
domain-separated nonzero C/F store GUIDs, path, digest, and listener
device/inode frame. A short/interrupted/failing write exits nonzero; the
supervisor owns structured pathname cleanup and no earlier failure emits
readiness.

SIGTERM and SIGINT handlers perform only the async-signal-safe action of
setting a flag and writing a byte to an internal nonblocking wake pipe. The
normal service thread drains that pipe, calls `SidecarRuntime::stop()`, and
closes/joins the bounded control workers. `stop()` closes the duplicated control wait
descriptor and posts cancellation of the active adopted TCP socket onto its
Asio owner context. Thus both an in-flight `FdHandoffReceiver` and an active
`run_adopted` dialogue terminate within the bounded shutdown gate without C++
object work in the signal handler.

The listener admits at most four concurrent control workers. This hard cap
means an authenticated idle dispatcher or one active CacheWire session cannot
consume the slot needed by a compiler-input attachment; excess connections
are closed. Each connection must pass peer-credential verification and one
exact `Hello` from `Daemon` with the configured nonzero generation and attempt;
the service returns one `HelloAck` from `Sidecar`.  That authenticated
relationship may remain idle for its daemon-owned lifetime; idleness is not an
operation timeout, and a stop-cancellable non-consuming poll preserves frame
boundaries.  Once readable, the next frame and handoff share a fresh bounded
operation budget and must be an exact, versioned `Data` operation envelope.
`CacheSession` admits exactly one
handoff for its nonzero request id; `InputFdAttachment` resolves the committed
record and then admits one compiler FD handoff. Wrong operation, identity,
attempt, request id, missing or extra descriptors, truncation, trailing data,
disconnect, timeout, adoption failure, and shutdown are fail-closed and close
all owned descriptors.

After accepting the CacheSession descriptor through SCM_RIGHTS, the service
writes the exact raw network-order token `50 f0 00 01` on that descriptor
before starting `P50ServerEndpoint::run_adopted`.  This socket token is distinct
from the process-launch `READY\n` pipe message above: it proves sidecar
ownership to C, carries no identity, shares the operation deadline, and any
send failure closes without starting CacheWire.

After one adopted dialogue returns, the same `SidecarRuntime` may process the
next freshly authenticated one-shot control connection and request id. Endpoint session leases,
temporary sockets, and handoff busy state are reset before the next session;
the shared `P50ServerEndpoint::run_adopted` reducer remains the sole endpoint
state owner.

The generic `p50_fd_handoff` helper remains a separate ownership primitive. It
does not know the ordinary-link codec, create a listener, or advertise an
endpoint.  Production daemon integration and advertisement remain outside
this service in `DaemonSidecarAdapter`; this process owns only authenticated
control, descriptor adoption, CacheWire reduction, and compiler-input lookup.

Shutdown compares the open listener's `fstat` device/inode with the pathname's
`lstat` device/inode before unlinking. A replacement node is never removed.
