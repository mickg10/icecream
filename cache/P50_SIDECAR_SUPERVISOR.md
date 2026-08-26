# Protocol-50 sidecar supervisor

`p50_sidecar_supervisor` is the lifecycle and private-incarnation boundary for
the `icecc-cache-service` sidecar. It does not open a listener, attach to a
daemon, select a scheduler owner, or alter the Login advertisement. A daemon
adapter consumes its state, counters, and immutable current READY lease without
reimplementing process ownership.

## Child contract

`Config::executable` must be an absolute regular executable path. Arguments are
passed directly as `argv` to `execve`; no shell or `PATH` lookup is performed.
The supervisor creates two private pipes before `fork`: the child inherits only
the READY write end, and receives its number in
`ICECC_CACHE_SERVICE_READY_FD`. That descriptor is explicitly made inheritable
in the child; all other supervisor descriptors are `FD_CLOEXEC`. Before that
step the child writes a private process-group proof marker to the second pipe.
The marker is followed by an `errno` record only when setup or `execve` fails;
marker-plus-EOF therefore proves both child-side group setup and successful
exec.

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
made inheritable. The child is placed in its own process group (with a
parent-side race-closing `setpgid`), so helper processes inherit the group and
cannot be orphaned by direct-child shutdown.

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
attempt, expected F-store GUID, exact socket path, and path digest. The service
rejects a partial tuple. It must close this bounded structured frame:

```text
READY v2 generation=N attempt=N pid=N F_STORE_GUID=... PATH=/... DIGEST=... DEV=N INO=N
```

The supervisor accepts the lease only when all fields match the allocated
incarnation and direct PID, the GUID and path digest recompute, and pathname
`lstat` proves the exact 0600 socket node and advertised device/inode. After
READY EOF it crosses a small bounded scheduling barrier and rechecks that the
direct child is still live before promoting the lease. An immediately dying
publisher therefore never becomes current.

Cleanup occurs only after direct-child and owned-process-group death are both
proved, and then re-lstats both exact identities; a replacement pathname or
inode is never blindly unlinked. If death is uncertain, the unique path is
deliberately leaked for external recovery and no replacement launch is
authorized. `current_lease()` is the sole lease observation for a daemon
adapter, and a dispatcher may use only that matching path, GUID, incarnation,
PID/credentials, and listener identity.

## Shutdown

`shutdown()` sends `SIGTERM` to the owned process group and, on Linux, to the
exact `pidfd` acquired immediately after `fork`. It waits at most
`shutdown_timeout`, then sends `SIGKILL` through the same two independently
bound authorities and performs a blocking `waitpid` only after exit or a kill
result is established. The direct child is never signalled through its numeric
PID: a SIGCHLD owner may have reaped it and that number may already identify an
unrelated process. Platforms without `pidfd_send_signal` retain proven
process-group teardown but fail closed instead of falling back to a direct
numeric signal.

Cleanup authority is returned internally only after both direct and group
absence are proved. The process-group path uses the raw `kill` syscall, so a
libc wrapper returning persistent `EINTR` cannot strand descendants. Every
owned descriptor, including the pidfd, is closed. Exit is observed without
reaping first, keeping the group leader's zombie PID in place until group
signalling finishes and preventing a PGID reuse race. Group ownership is
explicit and cleared on every teardown path; an unowned/stale numeric PGID is
never signalled. If a service moves out of the owned group, that group identity
is invalidated. The exact pidfd may safely terminate the original child, but
the uncertain old group still forces `DegradedLegacy`, deliberately leaks its
unique lease, and forbids a replacement launch.
The API is synchronous and intentionally leaves listener ownership and network
integration to a later reviewed slice.
