# Protocol-50 READY advertisement controller

`p50_ready_advertisement` is the pure policy boundary between sidecar
lifecycle observations and the cache capability carried by daemon Login.
It does not start a process, own a socket, send a Login, or modify
`daemon/main.cpp`.

The controller publishes only two kinds of snapshots:

* absent: `(0, 0, 0)`;
* present: `(public daemon TCP port, CACHE_WIRE_PROTOCOL_V1,
  CACHE_PROFILE_ZSTD_TU)`.

Presence requires all of the following in one observation: the daemon's
public listener is bound to a valid TCP port, the supervisor is exactly
`Ready`, and the private sidecar relationship has completed authenticated
HELLO/ACK.  Starting, stopped, degraded, unauthenticated, or listener-loss
observations withdraw the capability.

`post_ready_exits` is a monotonic incarnation edge supplied by the adapter.
If it increases while a capability is present, the controller emits absence
before any recovered presence, even when crash and recovery were compressed
into one adapter poll.  A counter regression or invalid bound port fails
closed until a valid observation catches up.

This checkpoint intentionally leaves Login inert.  The production adapter
must apply each returned transition in order and reannounce it to every live
scheduler connection.  It must also recreate a supervisor with an incremented
attempt for each sidecar incarnation.
