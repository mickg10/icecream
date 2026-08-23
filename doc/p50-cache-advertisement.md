# Protocol-50 inert cache-endpoint advertisement

This M2.5 slice adds endpoint metadata to the persistent daemon-to-scheduler
Login only. It does not make that metadata a scheduling input, forward it in
`UseCS`, connect to an endpoint, select cached input, or attach compiler input.

## Login tail

On a negotiated main-protocol-50 scheduler link, `LoginMsg` appends three
32-bit network-order words after `supported_features`:

1. `cache_endpoint_port`
2. `cache_protocol`
3. `cache_profile_mask`

The canonical absent value is exactly `0/0/0`. A present value has a TCP port
in `1..65535`, the qualified cache protocol value 50 (reported as
`cache_wire=v1`), and a nonempty subset of the endpoint profiles runnable in
this product. The converged M2 endpoint currently makes only `zstd_tu`
advertisable. Partial values, unknown bits, and labels without an implemented
endpoint codec are invalid on both send and receive.

P43, P48, and P49 omit all three words and decode them as absent. The
intermediate pre-advertisement P50 development commit is not a deployment
compatibility generation; the unified P50 successor owns this fixed Login
tail.

The endpoint host is the daemon address observed by the scheduler. Store GUID,
session identity, profile intersection, frame limits, and FILL limits remain
owned by the CacheWire handshake rather than duplicated in Login.

## Ownership and visibility

The real daemon publishes absence until a later owner has successfully bound
and listened on a dedicated endpoint and armed its accept loop. There is no
listener or command-line port override in this slice. Initial Login and
environment reannouncement use the same canonical snapshot.

The scheduler retains the snapshot on the `CompileServer` connection, replaces
it on a later Login, and destroys it with that connection. Login trace and
`listcs` expose either `cache=off` or the qualified endpoint. No eligibility,
score, dispatch, assignment, environment, client, or compiler-input code reads
the retained fields.

## Reserved streaming labels

Profile IDs 4 and 5 are reserved as `z3_long` and `z3_shared_long`. They remain
outside the implemented/known profile mask, endpoint advertisement mask,
session negotiation, transaction validation, and codec dispatch.
`z3_shared_long_b1` remains an experiment-control label and has no product
protocol ID.
