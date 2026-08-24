# Protocol-50 adopted endpoint seam

`P50ServerEndpoint::accept_one` and `P50ServerEndpoint::run_adopted` share one
connected-socket reducer.  Listener mode owns the result of exactly one
`accept`; adopted mode consumes one already-connected TCP socket and performs
no second `listen`, `accept`, or read before that ownership transfer.

`adopt_connected_fd` is the native-fd boundary for a future handoff.  It takes
ownership on every path, requires a connected IPv4/IPv6 stream socket, sets
`CLOEXEC`, and closes invalid, closed, non-socket, wrong-family, and
unconnected descriptors.  The returned socket is move-only; the endpoint
closes it on normal completion, disconnect, and terminal error.

The shared reducer retains the existing SessionHello negotiation, profile
validation, exact ZSTD_TU transaction decode/materialization, completion-stamp
checks, commit serialization, and bounded cleanup.  This seam does not wire a
daemon, receive `SCM_RIGHTS`, persist a store, or change the public cache
advertisement, which remains `0/0/0`.
