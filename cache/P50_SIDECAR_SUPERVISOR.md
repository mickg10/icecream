# Protocol-50 sidecar supervisor

`p50_sidecar_supervisor` is a lifecycle-only process boundary for the future
`icecc-cache-service` sidecar. It does not open a listener, attach to a daemon,
select a scheduler owner, or alter the Login advertisement (which remains
`0/0/0`). A future daemon adapter can consume the state and counters without
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

The child must write exactly `READY\n` to the READY descriptor. Readiness is
then close it. The parent accepts readiness only after EOF and only when the
complete byte sequence is exactly those six bytes; partial, extra, delayed,
or never-closed messages are rejected or time out. Readiness is bounded by
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

## Shutdown

`shutdown()` sends `SIGTERM` to the owned process group and direct PID, waits at
most `shutdown_timeout`, then sends `SIGKILL` to both and performs a blocking
`waitpid` only after exit or a kill result is established. On Linux the signal
path uses the raw `kill` syscall, so a libc wrapper returning persistent
`EINTR` cannot strand descendants. Every owned pipe is closed and the direct
child is reaped, including when the child or a helper ignores TERM. Exit is
observed without reaping first, keeping the group leader's zombie PID in place
until group signaling finishes and preventing a PGID reuse race. Group
ownership is explicit and cleared on every teardown path; an unowned/stale
numeric PGID is never signaled. If a service moves its direct PID out of the
owned group, the supervisor refuses that stale group ID and falls back to
direct-PID teardown.
The API is synchronous and intentionally leaves listener ownership and network
integration to a later reviewed slice.
