# Protocol-50 sidecar supervisor

`p50_sidecar_supervisor` is the lifecycle and private StoreIdentity boundary for
the `icecc-cache-service` sidecar. It does not open a listener, attach to a
daemon, select a scheduler owner, or alter the Login advertisement. A daemon
adapter consumes its state, counters, and immutable current READY lease without
reimplementing process ownership.

## Child contract

`Config::executable` must be an absolute regular executable path. Arguments are
passed directly as `argv` to `execve`; no shell or `PATH` lookup is performed.
The supervisor creates READY and exec-status pipes plus a one-byte private
`socketpair` launch gate before `fork`. The child cannot leave the launch gate until the parent has acquired
its exact pidfd; closing the gate makes it exit without executing user code.
After permission, the child creates a fresh session, writes a private session
proof marker, and continues toward `execve`. The child receives the READY write
descriptor number in `ICECC_CACHE_SERVICE_READY_FD`; that descriptor alone is
made inheritable, while the launch gate is closed and every other supervisor
descriptor remains `FD_CLOEXEC`. The marker is followed by an `errno` record
only when setup or `execve` fails. Parent-side gate sends use `MSG_NOSIGNAL`,
so an externally killed child cannot turn the safety handshake into a daemon
`SIGPIPE`; parent close/death instead gives the child EOF. Marker-plus-EOF proves both
child-side `setsid` and successful exec.

Without a configured lease root, the child must write exactly `READY\n` to the
READY descriptor and then close it. With a lease root, the structured READY-v2
contract below replaces that legacy six-byte message. The parent accepts
readiness only after EOF and only when the complete selected frame is exact;
partial, extra, delayed, or never-closed messages are rejected or time out.
Readiness is bounded by
`Config::readiness_timeout`; exec failures, pre-READY exits, malformed READY
data, and timeouts have separate counters. A ready child is observed through
`poll()`. Post-READY exits consume restart attempts from a fixed
`steady_clock` window. Exhaustion is terminal `DegradedLegacy`; the component
never invents a nonzero endpoint advertisement. Each synchronous recovery also
has `max_attempts_per_recovery`, so a call remains bounded even when a failed
attempt lasts longer than the restart window. Attempt/restart configuration is
bounded and lifecycle counters saturate instead of wrapping.

Before `execve`, Linux uses `close_range(..., CLOSE_RANGE_CLOEXEC)` to mark all
ambient descriptors close-on-exec. `ENOSYS`, `EINVAL`, and `EPERM` use a
bounded raw `/proc/self/fd` enumeration instead of an `OPEN_MAX` scan; targets
without that Linux primitive use a deliberately bounded `fcntl` fallback and
fail closed if its limit cannot be used. Only the READY descriptor is then
made inheritable. The child is a session leader with SID=PGID=PID, which the
parent revalidates while it is live. Helpers inherit that private group, and
unrelated daemon-session siblings cannot join it.

The readiness and shutdown bounds may be zero for an immediate bound; the
restart window must be positive so even a crashing child cannot create an
unbounded retry loop.

## Current READY lease

When `Config::lease_root` and a shared `Config::launch_identities` allocator are
supplied, every launch consumes a fresh nonzero attempt from that allocator and
derives its `F_STORE_GUID` canonically from `(generation, attempt)`. The
allocator is deliberately owned outside `Supervisor`, so restart and complete
controller/Supervisor recreation cannot reset or reuse an attempt. Exhaustion
fails closed rather than wrapping.

Each launch creates a fresh `mkdtemp` directory named with the generation and
attempt plus a unique suffix. The supervisor scrubs and then publishes the
complete six-field structured environment tuple: READY format, generation,
attempt, StoreIdentity derivation version, expected C/F store GUIDs, exact socket
path, and path digest. The service
rejects a partial tuple. It must close this bounded structured frame:

```text
READY v2 generation=N attempt=N DERIVATION_VERSION=1 pid=N C_STORE_GUID=... F_STORE_GUID=... PATH=/... DIGEST=... DEV=N INO=N
```

The supervisor accepts the lease only when all fields match the allocated
StoreIdentity root, role GUIDs, and direct PID, the GUID and path digest recompute, and pathname
`lstat` proves the exact 0600 socket node and advertised device/inode. After
READY EOF it crosses a small bounded scheduling barrier and rechecks that the
direct child is still live before promoting the lease. An immediately dying
publisher therefore never becomes current.

Cleanup occurs only after direct-child and owned-process-group death are both
proved, and then re-lstats both exact identities; a replacement pathname or
inode is never blindly unlinked. If death is uncertain, the unique path is
deliberately leaked for external recovery and no replacement launch is
authorized. `current_lease()` is the sole lease observation for a daemon
adapter, and a dispatcher may use only that matching path, GUID, StoreIdentity,
PID/credentials, and listener identity.

## Shutdown

Structured launch is available only when `pidfd_open` and
`pidfd_send_signal` are usable; an unsupported platform fails before `fork`.
At shutdown the supervisor first sends `SIGSTOP` through the exact pidfd and
observes that same child as stopped with `waitid(P_PIDFD, ... WNOWAIT)`. Only
that live, unreapable leader plus an exact `getpgid` match authorizes a
nonzero signal to the numeric process group. Helpers receive a bounded
`SIGTERM` grace period while the stopped leader anchors the PGID, after which
the group and exact child receive `SIGKILL`. An exact-handle reap follows only
after exit or a kill result is established. The direct child is never
signalled through its numeric PID.

Cleanup authority is returned internally only after both direct and group
absence are proved. The process-group path uses the raw `kill` syscall, so a
libc wrapper returning persistent `EINTR` cannot strand descendants. Every
owned descriptor, including the pidfd, is closed. A competing SIGCHLD reaper
cannot reap a stopped live leader, so it cannot open a PGID-reuse window before
the group KILL. Reaping is likewise bound to the pidfd with
`waitid(P_PIDFD, WEXITED)`; an external SIGCHLD reaper or later numeric PID
reuse therefore cannot make the supervisor reap an unrelated child. Group ownership is explicit and cleared on every teardown
path; an unowned/stale numeric PGID is never signalled. If the leader already
exited, was externally reaped, moved out of the group, or cannot be stopped and
observed exactly, the supervisor uses only the pidfd for direct-child teardown.
Any surviving or uncertain old group then forces `DegradedLegacy`, deliberately
leaks its unique lease, and forbids a replacement launch rather than risking an
unrelated process group.
The API is synchronous and intentionally leaves listener ownership and network
integration to a later reviewed slice.
