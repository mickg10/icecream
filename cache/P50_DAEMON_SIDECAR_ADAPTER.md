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
bounded Supervisor teardown and uses identity-checked directory cleanup; it
does not unlink a socket pathname owned by the service.

`local::connect_unix_until` is provided by the bounded local-transport slice and
is intentionally not implemented here.
