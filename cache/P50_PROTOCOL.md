# Protocol 50 and CacheWire revision 1

This is the current cache protocol and runtime contract for Sorbet 1.5.0.
It describes the live path first and identifies separately tested components
that are not connected to that path. It is not an implementation chronology
or a claim that every possible deployment has been verified.

## Layers and implementations

| Layer | Purpose | Definition |
|---|---|---|
| Ordinary Icecream protocol 50 | Scheduling, assignment, source admission, socket transition, results | [comm.h](../services/comm.h), [comm.cpp](../services/comm.cpp) |
| CacheWire revision 1 | Session negotiation, route identity, transactions and commits | [protocol50.h](protocol50.h), [protocol50.cpp](protocol50.cpp) |
| P29V1, profile 1 | Content reuse and requested Region transfer | [P29v1 format](codec/P29V1_FORMAT.md) |
| ZSTD_TU / ZSTD_ROUTE, profiles 2 / 3 | Independent TU frames / frames with committed route history | [ZSTD formats](codec/ZSTD_FORMATS.md) |
| Private local transport | Daemon/sidecar control and descriptor transfer | [p50_local_transport.h](p50_local_transport.h), [p50_fd_handoff.h](p50_fd_handoff.h) |

These version numbers are independent. The old codec v0 fixtures are not
deployable CacheWire revision-1 profiles. Ordinary legacy FileChunk compression
also remains separate from the cache profiles.

C means the submitting cache role and F the fulfilling cache role; S is the
scheduler. A TU is one exact preprocessed translation unit. A compiler attempt
is not a cache transaction: one committed TU may support a replacement attempt.

## Selection and advertisement

The only implemented profiles are:

| Profile ID | Advertisement bit | Environment selector |
|---:|---:|---|
| 1 | 0 | `P29V1` |
| 2 | 1 | `ZSTD_TU` |
| 3 | 2 | `ZSTD_ROUTE` |

Default preference is P29V1, ZSTD_TU, then ZSTD_ROUTE.
An explicit `ICECC_P50_PROFILE` request is exact; `OFF` suppresses cache
assignment. P29 v0, GRZ and streaming experiment labels are not alternatives
that a failed transaction can silently select.

On a protocol-50 scheduler link, Login appends three network-order u32 words:
cache endpoint port, CacheWire revision, and profile mask. Absence is exactly
`(0, 0, 0)`. Presence requires port 1..65535, revision 1 and a nonempty known
profile subset. P43/P48/P49 omit the tail and decode it as absent. The endpoint
host is the daemon address observed by the scheduler; store identities and
session limits are negotiated later, not duplicated in Login.

`iceccd` owns the public TCP listener. The sidecar adopts cache sockets from
that listener, rather than opening another public port. Advertisement requires
the bound listener, a Ready lifecycle and its exact current READY lease.
Idle time between one-shot private connections does not withdraw presence.
Listener loss, sidecar failure, stale lease or shutdown withdraws it.
The adapter retains a cumulative post-READY exit count across supervisor
replacement; regression, saturation and identity exhaustion reject presence.
When exit and recovery occur in one poll, it publishes absence before replacement
presence and reannounces each transition to scheduler connections.

Scheduler selection intersects advertised capabilities with the wrapper's
request. It cannot grant an unadvertised profile. See
[advertisement](../doc/p50-cache-advertisement.md) for the ordinary-link details.

## Assignment and source admission

The assignment owner is `(assignment_epoch, existing job_id, assignment_nonce)`.
There is no second serialized job ID. P50 UseCS and CompileFile carry the epoch
and nonce as high/low network-order u32 words. Complete absence is represented
by both being zero; partial identities are invalid. Strict-nonce mode is an
explicit operator choice, not an automatic consequence of seeing a P50 peer.
It requires compatible scheduler links to both daemons. Older links retain
their layouts and cannot manufacture a complete strict identity. The default
assignment mode remains legacy; local work does not require a remote assignment.
See [assignment identity](../doc/p50-assignment-identity.md).

Before source transfer, C sends `P50_SOURCE_ARM` with the complete assignment,
selected F endpoint, logical job/attempt, C-store generation/GUID, nonzero
source request ID and profile/source mode. Revision must be 1 and the single
profile bit must agree with the numeric source mode. F admits the exact arm
and replies `P50_SOURCE_ARMED`; C may not start CacheWire before this ACK.

F retains the waiting owner in `WAITP50INPUT`. A later CompileFile must match
the assignment and the live READY lease. Exact committed input, its ready
identity and a valid sealed descriptor are required before the wait becomes
forkable. `take_for_fork()` is one-shot; closure rejects late input and releases
a staged descriptor. The ordinary empty CACHE_SESSION discriminator alone
cannot create a source arm or compiler owner.

## Ordinary socket transition

CACHE_SESSION is an ordinary length-prefixed message with no fields after its
discriminator. Its complete bytes are:

```text
00 00 00 04  50 f0 00 00
```

After accepting the transferred descriptor, the sidecar writes this raw,
unframed ownership marker on that socket:

```text
50 f0 00 01
```

Only then may C send CacheWire SESSION_HELLO. The marker carries no store,
job or transaction identity; SESSION_HELLO provides the C-store identity.
Both ordinary message encode and decode require exactly protocol 50.

F's `release_fd_if_input_empty()` detaches only after the exact message,
with no buffered/read-ahead bytes, pending output, EOF or error and at the
next ordinary length boundary. A non-consuming kernel peek also checks queued
bytes and EOF. Refusal leaves ownership with MsgChannel.

Before detaching, daemon dispatch opens a fresh private connection to the
current lease, verifies peer PID/UID/GID, exchanges HELLO/HELLO_ACK and sends
the CacheSession operation envelope. It rechecks the listener path/inode and
lease immediately before release. Success transfers the descriptor exactly
once through SCM_RIGHTS. The private connection is one-shot, but consuming
it does not consume the advertised READY lease.

C's `release_fd_after_cache_session_ready(deadline)` is armed only by a
complete CACHE_SESSION send. It consumes that arm on every call, accepts only
the exact marker with no queued following byte, and retains the descriptor
for teardown on failure. Any later ordinary send or receive clears the
corresponding release arm; a flush cannot recreate it. A retry needs a fresh
ordinary connection. Partial reads/writes share the original absolute deadline;
the socket's TCP_USER_TIMEOUT is extended past that application deadline.

## CacheWire framing and negotiation

All integers in this section are unsigned big-endian. GUIDs and digests are
16 stored bytes without a length prefix. A frame is type u8, payload length
u24, then exactly that many bytes. Unknown types, malformed shapes and trailing
control bytes are rejected. The initial maximum payload is 1 MiB; negotiation
takes the smaller peer limits and must permit the 116-byte TX_BEGIN.
The initial FILL-record cap is 2^32 bytes, not a promise of that much memory:
profile and endpoint resource limits impose additional bounds.

| Type | Message | Payload, in order |
|---:|---|---|
| 1 | SESSION_HELLO | revision u16; C GUID; system-source fingerprint; profiles u32; frame cap u32; FILL cap u64 |
| 2 | SESSION_STATE | revision u16; negotiated profiles u32; frame cap u32; FILL cap u64; F GUID; system-source fingerprint; flags u8; history nonce u64; next relative sequence u64; state digest; optional TxCommit |
| 3 | HISTORY_RESET | history nonce u64; initial-state digest |
| 4 | ERROR | code u16; detail byte length u32; detail bytes |
| 5 | TX_BEGIN | the fixed 116-byte record below |
| 6 | BODY | profile bytes, possibly fragmented |
| 7 | NEED | profile bytes, possibly fragmented |
| 8 | FILL | profile bytes, possibly fragmented |
| 9 | TX_COMMIT | history nonce, relative sequence, TU sequence (u64 each); transaction, raw and post-state digests |

SESSION_STATE flag bits 0, 1 and 2 indicate namespace present, route present
and last commit present; all other bits are invalid. A commit is 72 bytes.
The codec validates flag/cursor consistency as well as exact length.
Revision disagreement is error code 4. Negotiation requires revision 1 and a
nonempty intersection of operational profiles; a response cannot exceed the
original offer. ERROR terminates that message session.

Matching nonzero system-source fingerprints enable P29v1 source-file reuse.
This choice is pinned for the route and cannot change mid-route.

### Transaction fields and digests

| TX_BEGIN offset | Bytes | Field |
|---:|---:|---|
| 0 | 8 | history nonce |
| 8 | 8 | route-relative sequence |
| 16 | 8 | C-wide TU sequence |
| 24 | 2 | profile ID |
| 26 | 16 | pre-state digest |
| 42 | 2 | BODY encoding, exactly the profile ID |
| 44 | 8 | encoded BODY bytes |
| 52 | 8 | decoded BODY bytes |
| 60 | 16 | encoded BODY digest |
| 76 | 8 | exact raw bytes |
| 84 | 16 | exact raw digest |
| 100 | 16 | transaction digest |

There is one BODY descriptor, no outer DICT, root-mode field or NEED flags
word. A descriptor is 34 bytes. BODY must match its declared count and digest.
For ZSTD, decoded bytes equal raw bytes; the P29 specification defines its
profile-specific BODY meaning.

`Digest128` is canonical XXH3-128 as implemented in
[services/digest128.cpp](../services/digest128.cpp). Domain labels are exact
ASCII bytes without a terminating NUL. The digest builder appends fixed-width
integers big-endian:

- Transaction: `ICECC-P50-TX-R1`, history nonce, relative sequence, TU sequence,
  profile u16, pre-state digest, complete BODY descriptor, raw count, raw
  digest, BODY byte count u64, then BODY bytes. The transaction-digest field
  itself is excluded.
- Post-state: `ICECC-P50-POST-V1`, pre-state digest, history nonce, relative
  sequence, TU sequence, transaction digest.
- Initial route: `ICECC-P50-ROUTE-V1`, C GUID, history nonce.

### Dialogue

```text
C                                      F
SESSION_HELLO ------------------------->
              <----------- SESSION_STATE
[authorized cold/idle HISTORY_RESET --->
              <----------- SESSION_STATE]
TX_BEGIN ------------------------------>
BODY ---------------------------------->
[P29v1 only:  <-------------------- NEED
 FILL -------------------------------->]
              <--------------- TX_COMMIT
```

BODY closes before NEED. P29v1 carries framed inner streams; it does not expose
Key64 in those streams. ZSTD profiles have no NEED/FILL dialogue.
Neither the ordinary ownership marker nor P29's inner TU_END is an outer
commit. Input materialization, exact raw validation and publication precede
TX_COMMIT.

## Routes, commit and restart behavior

C owns immutable prepared input and routes selected by F/profile identity.
F owns namespaces, tentative dialogues, resource reservations, immutable
input records and retained commit witnesses. History nonces and session
serials do not wrap or reuse within an incarnation. TU sequence is C-wide;
relative sequence orders one route. The cache key remains exactly
`(C_STORE_GUID, TU_SEQ)`, not an assignment or process identifier.

A disconnected F overlay is discarded without erasing C's active transaction
or a retained F commit that C has not acknowledged. Reconnect is classified
against exact store, route and transaction state: it can resume the route,
replay the same immutable transaction, accept the exact retained commit,
start an authorized cold route for a changed F store, perform one allowed
idle reset, or refuse. An ambiguous disconnect is not permission to send
different bytes, reset active history or fall back to FileChunk.

| Event | Required boundary |
|---|---|
| Sidecar/store replacement | Withdraw the old lease; allocate fresh store identity and private path; reject old completions and attachments. |
| C-store replacement | Use a fresh C namespace; do not reuse its previous store identity with empty state. |
| Lost TX_COMMIT response | Reconcile the exact retained commit or exact pending transaction; do not infer non-commit from EOF. |
| Compiler attempt replacement | Change the assignment owner, not the input key; stale owners cannot attach or close the replacement. |
| Scheduler/assignment replacement | Validate the current epoch/job/nonce; cache availability alone does not authorize a compiler. |
| Job closed before cache completion | Validate the late commit without reopening attachment. |

Here a retained/durable commit means the endpoint's commit witness for that
store incarnation. It does not promise recovery of the same store identity
from arbitrary disk or process loss.

The client sender retains its prepared handle across its bounded retry, using
a fresh ordinary socket for each attempt. It requires a directly validated
TxCommit and exact input key rather than treating diagnostic trace output as
a completion witness. See [client sender](../client/P50_ZSTD_SENDER.md).

## Compiler input and result lifecycle

The service's InputFdAttachment operation uses the complete cache key and
assignment owner. It schedules lookup on the endpoint owner, obtains an
independent cursor, and materializes a Linux sealed memfd under one absolute
deadline. Copying, digest checks and writes advance in at most 64 KiB chunks
with deadline checks. The service verifies length/digest, reopens the complete
memfd read-only and CLOEXEC, rewinds it, then transfers it.

The receiver rechecks read-only/CLOEXEC/regular-file properties. Every request
gets an independent open-file description and byte-zero offset. An already
transferred snapshot remains readable if the sidecar exits. Unsupported
platforms and invalid, stale, missing or timed-out requests return no descriptor.

Compiler ownership is `(logical_job, assignment_epoch, assignment_nonce)`;
the private operation is scoped to the sidecar generation/attempt:

- CancelAttempt revokes the current attempt but retains the logical-job lease
  for a fresh assignment.
- CloseAcceptedJob and CancelJob are terminal: no new attachments, but already
  authorized descriptors survive. Collection waits for the last cursor.
- Capacity is reserved before input publication. A terminal operation arriving
  first leaves a bounded tombstone; late commit retires it without publication.
- Exact operation replay returns AlreadyApplied. Reusing an operation ID with
  different identity or action returns ConflictingReplay.

The daemon retains failed lifecycle deliveries in a bounded retry queue. If a
command cannot be retained, or a verified reply violates the protocol, it
withdraws and destroys that exact sidecar/store incarnation. Commands are
never rebound to a replacement store.

A P50 compiler child sends CompileResult and all object/DWO output, then waits
up to 30 seconds for an exact ResultDisposition from the submitter. It writes
one canonical 144-byte big-endian P50CompletionRecord to its parent and closes
the pipe. Legacy children retain their 32-byte native-word status record.
The parent checks exact size, identity and the retained InputFdRequest.
Accepted maps to CloseAcceptedJob; DefinitiveCancel maps to CancelJob.
Malformed, short, extra, missing or mismatched records are attempt-only.
Terminal settlement consumes the active lease so ordinary teardown cannot
settle it twice. The full ordinary result wire is in the
[result-disposition contract](../research/reports/P50_RESULT_DISPOSITION_WIRE_AUDIT.md).

## Private control and descriptor transfer

### Local frames

The AF_UNIX SOCK_STREAM header is 28 bytes, all big-endian:
magic `P50L` (4 bytes), version u16 (1), message type u16, payload length u32,
generation u64 and attempt u64. Types are HELLO, HELLO_ACK, DATA and GOODBYE.
Payloads are capped at 64 KiB. HELLO/HELLO_ACK payloads contain exactly one
role byte and must match the expected incarnation.

One move-only Connection owns the socket and write path; concurrent sends
return Busy. Descriptors are CLOEXEC, peer UID/GID (and the lease PID where
required) are verified, and sends suppress SIGPIPE. Absolute steady-clock
deadlines cover whole frames across partial operations; no host clock
time_point is serialized. Unsupported required OS facilities cause refusal.

The CacheSession DATA operation remains a 32-byte version-1 envelope.
Owner-bearing InputFdAttachment and InputLifecycle operations use this
88-byte version-2 layout:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 2 | operation version (2) |
| 2 | 2 | operation kind |
| 4 | 4 | exact length (88) |
| 8 | 8 | sidecar generation |
| 16 | 8 | sidecar attempt |
| 24 | 8 | request/operation ID |
| 32 | 16 | C GUID |
| 48 | 8 | TU sequence |
| 56 | 8 | logical job |
| 64 | 8 | assignment epoch |
| 72 | 8 | assignment nonce |
| 80 | 2 | lifecycle action; zero for attachment |
| 82 | 2 | result+1 for lifecycle replies; zero for requests |
| 84 | 4 | reserved zero |

The old ownerless 56-byte attachment form is parsed only to return an explicit
mixed-version refusal; it cannot authorize a compiler. Lifecycle requests
reject a result field or queued trailing data before mutation. Replies echo
the full identity. The daemon-side caller sends an empty identity-bound GOODBYE after the reply so
stream closure does not race reply delivery. Losing that ACK does not undo
the operation.

### Descriptor records

The dedicated one-shot handoff exchange uses a fixed 40-byte big-endian record:
`P50F` magic (4), version u16 (1), kind u16 (request=1, ACK=2, NACK=3),
record length u32 (40), generation u64, attempt u64, nonzero request ID u64,
result u32 (zero for requests).

A request carries exactly one SCM_RIGHTS message with exactly one descriptor.
Positive partial sendmsg transfers rights once; remaining bytes are sent
without ancillary data. Readers consume exactly one record and reject queued
pipelined bytes, missing/extra descriptors, malformed ancillary data,
truncation, wrong identities, duplicates and expired deadlines.

The receiver establishes CLOEXEC and transfers the descriptor to its adopted
owner before ACK. If ACK delivery fails, that owner still owns the descriptor.
The sender closes its copy on every terminal outcome and never retries a
sent handoff. This raw exchange is not interleaved with framed control.

## Sidecar process and lease ownership

The daemon adapter drives SidecarLifecycle from the daemon poll loop and
uses CentralChildReaperRegistry for child-exit ownership. The adapter pre-binds the private
listener before fork; the service adopts it and never binds or unlinks that
structured pathname. Paths use a fresh 0700 per-attempt directory and a 0600
socket, owned by the daemon's effective nonzero UID/GID. Replaced nodes are
never removed.

Each launch consumes a fresh control attempt, F-store generation and random
StoreIdentity root. Its C/F GUIDs are role-tagged projections of that root,
not hashes of generation/attempt counters. The shared allocator outlives
controller replacement and rejects exhaustion.

The structured launch supplies nine all-or-none identity environment fields:
READY_FORMAT, EXPECTED_GENERATION, EXPECTED_ATTEMPT,
EXPECTED_F_STORE_GENERATION, EXPECTED_DERIVATION_VERSION,
EXPECTED_F_STORE_GUID, EXPECTED_C_STORE_GUID, EXPECTED_SOCKET and
EXPECTED_SOCKET_DIGEST, each prefixed `ICECC_CACHE_SERVICE_`.
READY_FORMAT is 2 and derivation version is 1. Separate READY_FD and LISTENER_FD
fields carry the inherited descriptors. The tuple overrides stale standalone
CLI identity values; partial or invalid tuples are rejected.

The closed readiness pipe must contain exactly:

```text
READY v2 generation=N attempt=N F_STORE_GENERATION=N DERIVATION_VERSION=1 pid=N C_STORE_GUID=... F_STORE_GUID=... PATH=/... DIGEST=... DEV=N INO=N
```

Readiness requires successful exec, exact tuple/PID/GUID/path matching, a
matching live socket node and a still-live child. Pathname device/inode values
come from lstat; they are not equated with the socket descriptor's fstat tuple.
The standalone/test form uses the separate legacy `READY\n` message and
does not replace this production launch contract.

The child waits behind a launch gate until the parent has its exact pidfd,
then establishes a private session/process group. Only required launch
descriptors survive exec. Readiness and restart budgets are bounded; failure
never publishes a nonzero endpoint.

Shutdown withdraws dispatch first. The adapter and lifecycle use the exact pidfd to
stop and observe a live leader before signaling its anchored process group;
TERM grace, KILL, reap and cleanup are bounded. If exact child/group death
cannot be established, it leaves the unique lease for external recovery and
forbids replacement rather than acting on a reused numeric process identity.

The service admits at most 64 control workers. Idle verified connections have
stop-cancellable waits; readable operations receive fresh bounded budgets.
SIGTERM/SIGINT handlers only set a flag and wake a pipe; normal threads stop
workers and cancel endpoint I/O. Each new cache session uses a fresh operation
connection while the endpoint's route state survives between connections.

## Endpoint execution and cancellation

The service calls the adopted-socket overload of P50ServerEndpoint; accepted
and adopted sockets use the same reducer. The endpoint does not own daemon
Login or sidecar launch. ProfileDialogue supplies profile-specific parsing,
materialization, terminal promotion and bounded state accounting, while the
endpoint owns routes, reservations and input publication.

Complete-BODY materialization uses a shared two-worker pool with at most eight
running-plus-queued jobs. Workers prepare immutable publication nodes; only
the owner executor can publish after checking current operation, deadline and
job state. Late completion cannot reopen closed input. Eventfd duplicates let
late workers finish without referring to a destroyed Asio executor.

EndpointRunRegistry binds callbacks and cancellation to complete launch,
store, operation, session and socket-generation identity. A permit targets
only that run; stale events cannot act on a reused descriptor number.
An endpoint observation such as disconnect or a parsed commit is not, by
itself, a distributed settlement for a separate FSession owner. That owner
must settle its exact operation before uncertain retained work can be replaced.

## Additional tested components

The reference implementations in `unittests/support/` have focused tests but
are compiled only by `make check`, never into shipped programs. They must not
be confused with the live source-arm / socket-handoff / InputFdAttachment
path described above. Their tests preserve requirements and do not establish
that equivalent behavior is wired into the live path.

| Component | Contract and scope |
|---|---|
| P50CacheSessionJoin | Registers a complete WAIT owner, revalidates connection/READY identity and reserves exact attempts; not called by current daemon cache dispatch. Expiry before detach may release a slot; uncertainty after detach requires exact settlement. |
| SourceIngress / SourceFinalize | Separately latches wrapper EOF and exact-child completion; a revocable finalization token and F ACK gate transfer. This reducer is not the current CompileFile bridge. |
| PhaseOpen / HandoffAuthority | Exact request/arm echo and replay high-water mark with nonrenewable establishment/source deadlines. Linking these helpers does not send their frames in production. |
| ReverseFdOwner / ReceiverLedger | Sealed master plus fresh CLOEXEC duplicate per retry; unchanged delivery token/deadline, remaining-duration wire field, accept-before-ACK and duplicate suppression. Not the live attachment transport. |
| Synchronous Supervisor | Retained test implementation only. The live daemon uses SidecarLifecycle and CentralChildReaperRegistry through its adapter; the adapter supplies the process-domain observations used for replacement. |
| ClientRoleOwner / ServerRoleOwner | Typed pre-adoption role owners; not complete C/F CompileFile wiring. |
| InputAttachmentCore | Reference ready/ACK/replay reducer; the live path uses InputFdAttachment and InputLifecycleRegistry. |
| Typed P5coEndpointHandoff overload | Consumes an exact retained socket lease and operation deadline; the service uses the socket overload instead. |

PhaseOpen helpers and the typed endpoint overload remain separately tested
interfaces, not additional live protocol exchanges. The concrete retained
socket used by the typed-overload tests lives in test support.

The service validates OP_CANCEL shape and bounds, but its zero binding
placeholder does not provide a complete session claim. Daemon OP_CANCEL and
C_SOURCE emission are not established by that parser. Likewise, the
p50s2restartreplay fixture combines supervisor and reverse-FD reducers; it is
not a live daemon kill between compiler-FD adoption and ACK.

## Verification and maintenance

Executable checks live in [unittests](../unittests/Makefile.am):
wire/codec tests, cache-session release tests, source-arm admission, private
transport and handoff, lifecycle, endpoint, supervisor, and real compiler
integration. The profile simulator is [p50sim_batch_test.sh](sim/p50sim_batch_test.sh);
the real compile gate is [p50compilee2e-run.sh](../unittests/p50compilee2e-run.sh).
See [test entrypoints](../farmharness/integration/tests/README.md) for orchestration.

[Formal models and trace checks](formal/README.md) cover bounded, separated
state machines. A model pass is not proof of the entire C++ implementation.
Action traces distinguish materialization, input commit, C commit acceptance,
replay and store replacement; tests should validate those distinctions rather
than count log messages.

Keep current protocol descriptions here and profile formats in codec/.
Implementation chronology belongs in Git history, not one Markdown file per
implementation step. The generated [key-layout census](KEY_LAYOUT_V1_CENSUS.md)
is retained separately because its exact output is checked against measurements.
