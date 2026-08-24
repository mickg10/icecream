# Protocol-50 daemon sidecar adapter

`DaemonSidecarAdapter` is the main-loop lifecycle owner for one cache-service
incarnation.  It creates a fresh private runtime subdirectory and socket name
for each checked `(generation, attempt)` pair, launches the exact absolute
service executable through `sidecar::Supervisor`, and authenticates the
control connection with both OS peer credentials and the identity-bound
HELLO/ACK exchange.

The daemon owns its public listener.  It reports the listener's current bound
state and port with `observe_public_listener`; the adapter never creates,
closes, or infers a public descriptor.  Presence is projected only when the
listener has the configured port, the supervised child is READY, and the
private relationship is authenticated.  A consumed one-shot handoff therefore
withdraws presence and causes bounded reconnect attempts against the same
incarnation without creating another listener.

The adapter keeps its post-READY exit high-water counter across Supervisor
recreation.  A replacement poll emits withdrawal before recovery presence, and
counter regression, saturation, attempt overflow, stale runtime nodes, and
credential/identity mismatches fail closed.  Shutdown disables dispatch before
bounded Supervisor teardown and returns the ordered withdrawal transition.
The runtime parent, captured attempt directory, and captured socket inode are
revalidated on every live poll.  A graceful service removes its listener; if
SIGKILL prevents that cleanup, the adapter unlinks only the exact socket
device/inode captured for the exact still-owned attempt directory, and only
after Supervisor has established child/process-group death.  Replaced
pathnames and directories are never touched.

`local::connect_unix_until` is provided by the bounded local-transport slice and
is intentionally not implemented here.

## Deployment identity

The current private-transport law requires the runtime parent, each attempt
directory, and the socket node to be owned by the daemon effective UID/GID and
to use exact mode `0700` for directories.  iceccd is deployed already as the
unprivileged `icecc` identity (with any cap-ng setup completed by its launcher),
so the sidecar inherits that identity.  A distinct pre-bind privilege drop is
rejected by this adapter: the dropped socket owner would be rejected before
SO_PEERCRED authentication by the local connector.  The explicit drop fields
remain in the launch configuration for compatibility with the service option
surface but are unsupported under this transport law.
