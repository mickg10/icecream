# Protocol-50 cache service (S2 local-control skeleton)

`icecc-cache-service` is an installed, deliberately small sidecar control
process.  It does not implement a cache codec, store, remote frame, or daemon
integration.  `iceccd` continues to own the public TCP listener; a later slice
will pass clean-boundary cache-session descriptors over this private control
relationship.  Public advertisement remains the existing `0/0/0` value.

## Invocation

The service requires every identity and path value explicitly:

```
icecc-cache-service \
  --socket /run/user/4103/icecc/cache-service.sock \
  --peer-uid 4103 --peer-gid 3513 \
  --generation 7 --attempt 1
```

The socket path must be absolute, its existing parent must be owned by the
effective uid and have exactly mode `0700`, and the socket node is created with
exact mode `0600` and `CLOEXEC`.  The service never replaces an existing node.
The peer must match both configured uid and gid, obtained with Linux
`SO_PEERCRED`; unavailable credentials fail closed.

The service refuses to run with uid or gid zero.  A privileged launcher may
instead supply both `--drop-uid` and `--drop-gid` (both nonzero).  Before bind,
it clears supplementary groups, calls `setgid` then `setuid`, and proves real,
effective, and saved ids plus an empty supplementary-group set.  A partial or
unproven transition fails closed.

## Readiness and control

`ICECC_CACHE_SERVICE_READY_FD` is mandatory and is parsed as a decimal,
nonnegative, already-open descriptor (no shell/PATH lookup).  Exactly `READY\n`
is checked-written and the descriptor is closed only after privilege state,
bind, listen, and listener identity capture complete.  If any earlier step
fails, no READY message is emitted.

The bounded loop polls the listener and accepts at most one control connection
at a time.  Each accepted descriptor has one move-only transport owner, one
reader, and one writer.  The service verifies `SO_PEERCRED`, receives one exact
`Hello` from `Daemon` with the configured nonzero generation and attempt, and
returns one `HelloAck` from `Sidecar`.  Malformed, truncated, oversize, stale,
wrong-role, wrong-credential, and timed-out inputs are closed and do not stop
the listener.  SIGTERM and SIGINT stop the poll loop and close the listener.

Shutdown compares the open listener's `fstat` device/inode with the pathname's
`lstat` device/inode before unlinking.  A replacement node is never removed.

This is a control-only slice; no session FD passing or data frame handling is
present yet.
