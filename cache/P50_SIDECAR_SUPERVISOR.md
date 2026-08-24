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
in the child; all other supervisor descriptors are `FD_CLOEXEC`. The second
pipe is `FD_CLOEXEC` in the child and carries an `errno` record only when
`execve` fails, so an EOF distinguishes successful exec from an exec failure.

The child must write exactly `READY\n` to the READY descriptor. Readiness is
bounded by `Config::readiness_timeout`; exec failures, pre-READY exits,
malformed READY data, and timeouts have separate counters. A ready child is
observed through `poll()`. Post-READY exits consume restart attempts from a
fixed `steady_clock` window. Exhaustion is terminal `DegradedLegacy`; the
component never invents a nonzero endpoint advertisement.

The readiness and shutdown bounds may be zero for an immediate bound; the
restart window must be positive so even a crashing child cannot create an
unbounded retry loop.

## Shutdown

`shutdown()` sends `SIGTERM`, waits at most `shutdown_timeout`, then sends
`SIGKILL` and performs a blocking `waitpid` if necessary. Every owned pipe is
closed and the child is always reaped, including when the child ignores TERM.
The API is synchronous and intentionally leaves listener ownership and network
integration to a later reviewed slice.
