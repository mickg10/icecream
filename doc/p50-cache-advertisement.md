# Protocol-50 cache-endpoint advertisement

The daemon READY controller publishes endpoint metadata through the persistent
daemon-to-scheduler Login. The metadata is a capability/route restriction: it
is validated and intersected with the wrapper request before selection, then
carried through the assignment handoff. It is not a substitute for the
authenticated CacheWire session or compiler-input attachment protocol.

## Login tail

On a negotiated main-protocol-50 scheduler link, `LoginMsg` appends three
32-bit network-order words after `supported_features`:

1. `cache_endpoint_port`
2. `cache_protocol`
3. `cache_profile_mask`

The canonical absent value is exactly `0/0/0`. A present value has a TCP port
in `1..65535`, CacheWire revision `1` (reported as `cache_wire=v1`), and a
nonempty subset of the endpoint profiles runnable in this product. The current
advertisable mask includes `P29V1`, `ZSTD_TU`, and `ZSTD_ROUTE`; partial values,
unknown bits, and labels without an implemented endpoint codec are invalid on
both send and receive.

P43, P48, and P49 omit all three words and decode them as absent. The
intermediate pre-advertisement P50 development commit is not a deployment
compatibility generation; the unified P50 successor owns this fixed Login
tail.

The endpoint host is the daemon address observed by the scheduler. Store GUID,
session identity, profile intersection, frame limits, and FILL limits remain
owned by the CacheWire handshake rather than duplicated in Login.

## Ownership and visibility

`iceccd` owns the public TCP listener and supplies its currently bound port to
the adapter. The supervisor pre-binds the sidecar's private AF_UNIX listener;
the service adopts that descriptor and publishes its immutable READY lease.
The adapter publishes absence until the daemon listener, supervised child, and
matching READY lease are valid. See the [READY policy](../cache/P50_PROTOCOL.md#selection-and-advertisement)
for the policy boundary. There is no separate cache listener port override;
the daemon's public port remains configurable. Initial Login and environment
reannouncement use the same canonical snapshot.

The scheduler retains the snapshot on the `CompileServer` connection, replaces
it on a later Login, and destroys it with that connection. Login trace and
`listcs` expose either `cache=off` or the qualified endpoint. The daemon
intersects the snapshot with wrapper capability and the kill-switch before
forwarding `GetCS`; selection may narrow the request but cannot grant an
unadvertised profile. Assignment and input attachment still perform their own
identity checks.

## Reserved streaming labels

Profile IDs 4 and 5 are reserved as `z3_long` and `z3_shared_long`. They remain
outside the implemented/known profile mask, endpoint advertisement mask,
session negotiation, transaction validation, and codec dispatch.
`z3_shared_long_b1` remains an experiment-control label and has no product
protocol ID.
