# Protocol-50 adopted endpoint seam

`P50ServerEndpoint::accept_one` and both
`P50ServerEndpoint::run_adopted` overloads share one connected-socket reducer.
Listener mode owns the result of exactly one `accept`; adopted mode consumes
one already-connected TCP socket and performs no second `listen`, `accept`, or
read before that ownership transfer.

The typed post-P5CO overload consumes one complete move-only
`P5coEndpointHandoff`: the exact canonical `ADOPTED` outcome, the original
absolute monotonic deadline, and the retained socket lease. Socket authority
cannot be publicly extracted from that bundle. The endpoint decodes the
canonical claim, requires the exact F store, requires the first CacheWire
`SESSION_HELLO` to name the claimed C store, and binds every completion to the
unchanged `P50FSessionOperationId` and deadline. A stale lease or failed native
socket adoption executes one explicit operation fence. An RAII transfer guard
keeps that fence armed from private lease extraction through native adoption
and session registration, including allocation/cap failures after descriptor
release.

`P5coRetainedSocketLease` is the concrete post-`SCM_RIGHTS` authority. It owns
one `CLOEXEC` `HandoffFd`, writes only with
`send(MSG_DONTWAIT | MSG_NOSIGNAL)`, never changes shared `O_NONBLOCK` state,
and exposes its descriptor only through the endpoint-private virtual release.
`adopt_connected_fd` consumes that descriptor on every path, requires a
connected IPv4/IPv6 stream socket, preserves `CLOEXEC`, and closes invalid,
closed, non-socket, wrong-family, and unconnected descriptors.

## Absolute deadline and asynchronous ownership

Before the first CacheWire read, the typed server path creates a
`CLOCK_MONOTONIC` `timerfd` with `TFD_NONBLOCK | TFD_CLOEXEC` and arms the
original deadline using `TFD_TIMER_ABSTIME`. Every asynchronous completion,
worker return, product job-state selection, and commit seam receives a fresh
clock/operation recheck. Expiry or explicit cancellation closes only the exact
active socket and wakes a pending materialization wait, so an abandoned codec
worker cannot retain the endpoint coroutine or publish later.

Complete-BODY codec materialization runs on one product-wide two-worker thread
pool. Running plus queued work is capped at eight jobs; further admissions fail
without entering the pool, so abandoned or permanently blocked jobs cannot
grow an unbounded queue. The worker owns a move-only dialogue job but no
endpoint publication authority. It validates and allocates an immutable
`InputRecordStore::PreparedPublish`; the owner later consumes that one-shot
node at the durability linearization point. The store reserves its full
configured bucket bound at construction, so the owner insertion allocates no
node or bucket. Capacity failure leaves route and retained-input state
unchanged, and a late closed-job completion validates or discards its prepared
node without reopening compiler attachment.

The worker-shared completion state contains no Asio object or owner executor.
The coroutine owns one nonblocking `eventfd` descriptor and the worker owns a
`CLOEXEC` duplicate of the same counter. Worker completion, deadline, and
cancellation wake the owner with one allocation-free `write`; a late worker may
write and close its duplicate safely after the coroutine and `io_context` have
already been destroyed.

The shared reducer retains SessionHello negotiation, profile validation,
exact ZSTD_TU transaction decode, completion-stamp checks, commit
serialization, and bounded cleanup. The concrete lease is ready for a
production caller, but this successor does not yet add the daemon-to-sidecar
call site, the canonical FInput Ready/attachment bridge, persistent store
recovery, or a public cache advertisement; advertisement remains `0/0/0`.
Those missing integrations are why this component is not an S2 exit claim.

## Operation-scoped C cancellation

`P50ClientEndpoint::request_cancel_for_test()` is retained only as a standalone
lineage test seam; production cancellation uses an exact typed permit. The C-role
counterpart of the
server endpoint's owner-affine cancellation seam.  Its caller posts onto the
endpoint owner context; it closes only the current dialogue and never resets
the route, promotes fallback, or changes another role.

The client records the first point at which any CacheWire byte may have begun
leaving C.  A cancellation before that point, with no retained active
transaction, reports `AbortedPreDurable` and may discard only the locally
queued copy.  Once remote transmission may have begun—or an earlier active
transaction already exists—the result is `ReconcileRequired`; the prepared or
active identity remains available for exact retry.  Ordinary disconnects keep
`None`, so transport failure cannot impersonate explicit owner cancellation.
The current C API still accepts an absolute `steady_clock::time_point` rather
than the typed P5CO deadline DTO. It now samples that deadline before and after
every completion and immediately before accepting a commit. Carrying one typed
cumulative run lease through the production C-side handoff remains an
integration requirement.

## Endpoint run identity bridge

`EndpointRunRegistry` is the implementation-only owner-affine bridge for that
integration. Admission mints an `EndpointRunIdentity` containing the complete
`sidecar::LaunchIncarnation`, both role-store GUIDs, the exact
`P50FSessionOperationId`, endpoint generation/session serial, a nonzero run
sequence, and socket-ownership generation. The move-only `EndpointRunHandle`
retains the identity and original absolute deadline; it can produce only an
`EndpointCancelPermit{identity, observation_id, reason}`.

`request_cancel` compares every identity field and observation before invoking
the exact socket target. Late timer, descriptor, codec, or cancellation events
therefore resolve as `Stale` after the row is consumed, even if a descriptor
number or socket object address is reused. `cancel_all_for_incarnation` is a
separate shutdown/failure operation and is not reachable through an ordinary
permit. The current standalone endpoint lineage still uses its owner-affine
test seam while daemon OP_CANCEL/FSession terminal settlement is wired; the
registry tests are the production-shaped source material for that handoff.

`ClientRunResult::settlement` is an identity-bound observation, never a local
replacement authority. Disconnected, terminal-error, cold-store, and route
reset outcomes remain unresolved/reconciliation observations; only the owning
FSession operation may settle `CommittedInput` or `AbortedPreDurable`.
