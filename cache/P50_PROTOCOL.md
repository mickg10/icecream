# Ordinary protocols 50/51 and CacheWire revisions 1/2

This is the current cache protocol and runtime contract for Sorbet 1.5.0.
It describes the live path first and identifies separately tested components
that are not connected to that path. It is not an implementation chronology
or a claim that every possible deployment has been verified.

## Layers and implementations

| Layer | Purpose | Definition |
|---|---|---|
| Ordinary Icecream protocols 50/51 | Scheduling, assignment, source admission, socket transition, results | [comm.h](../services/comm.h), [comm.cpp](../services/comm.cpp) |
| CacheWire revisions 1/2 | Session negotiation, route identity, transactions and commits | [protocol50.h](protocol50.h), [protocol50.cpp](protocol50.cpp) |
| P29V1, profile 1 | Content reuse and requested Region transfer | [P29v1 format](codec/P29V1_FORMAT.md) |
| ZSTD_TU / ZSTD_ROUTE, profiles 2 / 3 | Independent TU frames / frames with committed route history | [ZSTD formats](codec/ZSTD_FORMATS.md) |
| Private local transport | Daemon/sidecar control and descriptor transfer | [p50_local_transport.h](p50_local_transport.h), [p50_fd_handoff.h](p50_fd_handoff.h) |

These version numbers are independent. The old codec v0 fixtures are not
deployable CacheWire revision-1 profiles. Ordinary legacy FileChunk compression
also remains separate from the cache profiles.

Ordinary negotiation supports protocol 51. Named R1 gates and
`protocol_supports_p50_r1_bridge` permit unchanged R1 records on ordinary
protocol 50 or 51. This does not convert CACHE_SESSION release/READY or its
one-TU EOF contract into a persistent session. Versions outside the explicitly
supported range are rejected. Assignment identity remains a version-50 feature
threshold independent of the ordinary maximum.
The cache-advertisement fields retain the existing v50 layout; their revision
field distinguishes R1 from opt-in R2. R2 assignment requires ordinary 51 at
both submitting and fulfilling peers, matching cache revisions, and scheduler
opt-in. An unknown cache revision must not invalidate an otherwise usable worker.

### Revision selection

Set `ICECC_P51_MODE=on` in the wrapper, participating daemons/sidecars, and
scheduler environments to request R2. Unset or `off` selects R1; any other
value disables cache selection. Explicit R2 cannot be selected over ordinary
50 and does not silently become R1. `ICECC_P50_MODE=off` remains the wrapper
cache-use kill switch. These settings do not change the separate policy for
allowing or prohibiting local compilation.

R2 is a candidate path, not a fully qualified release default. See
[validation status](../PROJECT_STATE.md) for the exact runtime evidence and
remaining recovery, restart, capacity and mixed-farm gates.

C means the submitting cache role and F the fulfilling cache role; S is the
scheduler. A TU is one exact preprocessed translation unit. A compiler attempt
is not a cache transaction: one committed TU may support a replacement attempt.

## Protocol-51 source control

These ordinary-message and descriptor codecs serve the opt-in
[persistent-link implementation](../doc/p50-transfer-concurrency.md#7-persistent-links-and-w30-implementation-specification).
Ordinary 51 alone does not enable R2; revision selection above still applies.

All integers below use network byte order, with no struct padding.

| Message | Discriminator | Payload |
| --- | --- | --- |
| P51_SOURCE_LEASE_REQUEST | `0x51f00000` | Exactly 32 bytes: job u32, assignment epoch u64, assignment nonce u64, profile u32, requested cache revision u32, requested window u32 |
| P51_SOURCE_ARM | `0x51f00010` | Existing source-arm field ordering with cache revision 2, followed by requested window u32; separate validation from R1 |
| P51_SOURCE_ARMED | `0x51f00011` | Exact P51 ARM echo, existing F identity/observation/budget/two-attempt-value fields, then reservation ID 16 bytes, logical relationship ID 16 bytes, relationship epoch u64, selected revision u32 and selected window u32 |
| P51_CACHE_LINK_SESSION | `0x51f00012` | Empty link-setup request and empty READY echo; production transition remains under implementation |

Requested revision is 2; requested window is 1–30. ARMED must select revision
2 and a nonzero window no larger than requested. Reservation and relationship
IDs must be nonzero. A valid ARMED record echoes the complete request; it does
not by itself establish live daemon/sidecar job ownership.

The P51 local descriptor reply is a distinct fixed 96-byte version-4 record.
Its first 64 bytes retain the version-3 layout, except the version word is 4;
offsets 64 and 72 contain C store generation and derivation version (u64 each),
and offset 80 contains the 16-byte C store GUID. The legacy version-3 reply
remains exactly 64 bytes. Each reply transfers exactly one descriptor and is
consumed once under a bounded deadline. The version-4 ticket retains the full
32-byte request, including revision/window, even though those two fields are
not repeated in the raw descriptor reply. Ordinary traffic, changed request
identity, malformed descriptor data or a non-clean boundary invalidates the
exchange. End-to-end asynchronous daemon integration remains a separate gate.

The R2 auxiliary-link transition uses one protocol-51 ordinary connection per
physical link. C sends empty P51_CACHE_LINK_SESSION and waits for the same
empty message as READY. F first checks local sidecar availability and the
clean ordinary parser boundary, then sends and flushes READY and hands off
the descriptor without another ordinary read. C likewise hands off after
decoding READY. Only then does C send LINK_HELLO. Any failure after READY
closes that physical connection; it must not resume ordinary parsing.

Buffered ordinary input prevents handoff. Kernel-queued R2 input after READY
does not: a fast peer may already have sent HELLO while F finishes handing off
the descriptor. Sending READY must preserve exactly the one-shot handoff
associated with the decoded request; unrelated ordinary sends must not do so.
The auxiliary connection owns no compiler job. Per-job ARM/ARMED stays on the
original compiler connection, and JOB_BIND consumes its exact reservation on
the persistent link. These are implementation requirements, not a claim that
the production transition has passed its integration gate.

### CacheWire R2 records

The opt-in persistent endpoint uses the following records. Record availability
does not prove recovery correctness; recovery gates remain separately tracked.
R1 records and bytes remain unchanged.
The outer frame remains `type:u8, payload_length:u24, payload`,
all record integers are big-endian, and GUIDs/digests/reservation IDs are 16
raw bytes. Revisions and profiles are u16; windows and frame caps are u32.
Payloads have no padding or reserved extensibility bytes. Type 25 has a
qualified codec; endpoint emission and sender handling remain pending.

| Type | Record | Exact payload fields / byte offsets | Bytes |
|---:|---|---|---:|
| 10 | LINK_HELLO | revision u16@0; profile u16@2; window u32@4; max frame u32@8; raw/encoded/output caps u64@12/@20/@28; reservation ID@36; relationship ID@52; relationship epoch u64@68; physical link generation u64@76; C GUID@84; C store generation u64@100; F GUID@108; F store generation u64@124; C control generation/attempt u64@132/@140; C system-source fingerprint@148; history nonce u64@164; verified floor A u64@172; start mode u8@180 (`0` initial, `1` reconnect) | 181 |
| 11 | LINK_STATE | revision u16@0; profile u16@2; window u32@4; reservation ID@8; relationship ID@24; relationship epoch u64@40; physical generation u64@48; C GUID@56; C store generation u64@72; F GUID@80; F store generation u64@96; C control generation/attempt u64@104/@112; selected frame cap u32@120; selected raw/encoded/output caps u64@124/@132/@140; F system-source fingerprint@148; history nonce u64@164; next REL_SEQ u64@172; state digest@180; committed prefix K u64@196; acknowledged prefix Q u64@204 | 212 |
| 12 | JOB_BIND | reservation ID@0; physical generation u64@16; relationship ordinal u64@24; job ID u32@32; assignment epoch/nonce u64@36/@44; logical job/attempt/source request/TU sequence u64@52/@60/@68/@76; profile u16@84; raw bytes u64@86; raw digest@94 | 110 |
| 13 | TU_BEGIN | relationship ordinal u64@0 followed by the exact 116-byte R1 TX_BEGIN payload at@8 | 124 |
| 14 | BODY | exact encoded BODY bytes; may be fragmented across bounded frames | variable |
| 15 | FILL | exact encoded FILL bytes; may be fragmented across bounded frames | variable |
| 16 | TU_END | relationship ordinal u64@0; binding digest@8; outer transaction digest@24 | 40 |
| 17 | R2_TX_COMMIT | relationship ordinal u64@0; binding digest@8; outer transaction digest@24; exact 72-byte R1 TX_COMMIT payload@40 | 112 |
| 18 | COMMIT_ACK | relationship ID@0; relationship epoch u64@16; physical generation u64@24; contiguous verified ordinal u64@32 | 40 |
| 19 | RECOVER Begin | relationship ID@0; relationship epoch u64@16; new physical generation u64@24; recovery operation ID@32; kind u8@48=`0`; floor A u64@49; prepared prefix P u64@57; witness count u32@65 | 69 |
| 19 | RECOVER Witness | same common identity through @47; kind u8@48=`1`; ordinal u64@49; binding digest@57; transaction digest@73; exact 116-byte TX_BEGIN@89 | 205 |
| 19 | RECOVER End | same common identity through @47; kind u8@48=`2`; witness count u32@49; transcript digest@53 | 69 |
| 20 | RECEIPTS Row | relationship ID@0; relationship epoch u64@16; physical generation u64@24; recovery operation ID@32; kind u8@48=`0`; ordinal u64@49; binding digest@57; transaction digest@73; exact 72-byte TX_COMMIT@89 | 161 |
| 20 | RECEIPTS End | same common identity through @47; kind u8@48=`1`; floor A u64@49; committed prefix K u64@57; acknowledged prefix Q u64@65; receipt count u32@73 | 77 |
| 21 | RESET | relationship ID@0; old/new relationship epochs u64@16/@24; physical generation u64@32; operation ID@40; settled prefix K u64@56; old/new history nonces u64@64/@72 | 80 |
| 22 | RESET_ACK | exact RESET payload@0..79; fresh initial state digest@80; next REL_SEQ u64@96 | 104 |
| 23 | RESET_CONFIRM | relationship ID@0; new relationship epoch u64@16; physical generation u64@24; operation ID@32; new history nonce u64@48; settled prefix K u64@56 | 64 |
| 24 | CLOSE | empty payload; closes only an idle bound link and settles no receipt | 0 |
| 25 | R2_LINK_REJECT | reason u16@0 (`1` StoreReplaced, `2` ReservationMissing); exact offered-HELLO digest@2 | 18 |

The rejection digest is XXH3-128 over ASCII `R2-link-offer-v1` without a NUL,
followed by the canonical 181-byte LINK_HELLO payload (no outer frame header).
Unknown reasons, truncated payloads and trailing bytes are invalid. Neither
reason grants commit or compiler-admission credit. The receiver emits
StoreReplaced for a different F GUID or a mismatch with its explicitly known
F generation. The sender validates the exact offered HELLO, shares the
rejection across queued callers, and retires the affected route without
discarding an already validated positive receipt. Retired route preparation
is removed only after its callers and background pumps release it.
ReservationMissing is decoded but is not yet emitted by the runtime:
generic lookup failure still closes the connection and uses bounded retry.
The distinction between definite absence and invalid/stale lookup is specified in
[the implementation plan](../doc/p50-transfer-concurrency.md#741-planned-link-rejection-completion).
See [validation status](../PROJECT_STATE.md) for the tested scope and remaining gates.

LINK_HELLO starts revision 2 and pins one profile/window to a physical link.
R1 transaction bytes are nested at TU_BEGIN and R2_TX_COMMIT but do not by
themselves bind job ownership or the R2 transaction envelope. LINK_STATE
returns actual selected budgets and F's fingerprint; the receiver validates
its echo against the original offer and retained reservation. P29 source-file
reuse requires equal nonzero C and F fingerprints. The selected frame cap
must be at least the 212-byte LINK_STATE size and no larger than both peers'
offers, the implementation cap, or the outer u24 limit. Raw, encoded and
materialized-output budgets are independent and must be reserved before
allocation.

JOB_BIND is canonicalized exactly as its 110 payload bytes. Its binding digest
is XXH3-128 over ASCII `R2-binding-v1` (no NUL) followed by those bytes. The
outer TU digest is XXH3-128 over ASCII `R2-transaction-v1` (no NUL), the
binding digest, then TU_BEGIN and every BODY/FILL payload in wire order; each
frame contributes its type u8, payload length u64, then exact payload bytes.
TU_END is excluded from its own digest. F independently derives the expected
P29 NEED and validates FILL; there is no R2 NEED frame. F validates job,
physical generation, ordinal, inner TX_BEGIN identity and both digests before
publishing. COMMIT_ACK advances only a contiguous ordinal and is checked
against F's committed prefix K. This codec checkpoint does not qualify the
development persistent receive loop, receipt recovery/reset, or ordinary
daemon selection/adoption. These records are not a production end-to-end
capability and no R2 advertisement is made.

RECOVER is a three-part request stream: Begin, exactly `P-A` contiguous
Witness records for ordinals `A+1..P` (`P-A <= W`), then End. Its transcript
digest is XXH3-128 over ASCII `R2-recover-v1` (no NUL), followed by the exact
Begin and Witness frame records in order; each contributes type u8, payload
length u64, and payload bytes. End is excluded. F verifies the complete
request before returning any receipts. RECEIPTS returns one exact retained
R2_TX_COMMIT witness for every ordinal in `(A,K]`, then an explicit End with
K, Q, and count. C validates the entire interval before advancing its
verified floor. F refuses `A < Q` or `A > K` and fences older physical-link
generations before processing recovery.

RESET is idempotently keyed by operation ID, advances the relationship epoch
by exactly one, names the reconciled prefix K, and replaces the codec history
nonce. RESET_ACK echoes the logical request fields and the new initial state;
RESET_CONFIRM names the same operation, new epoch, nonce, and K. F accepts a
duplicate RESET before checking stale old-epoch state and returns the cached
logical reset result, including after physical reconnection. A confirmed reset keeps its last
logical reset outcome until a later confirmed reset or relationship retirement.
On a new physical connection a duplicate RESET carries that connection's
current generation; the echoed RESET_ACK envelope uses that generation while
the operation ID, epochs, prefix, nonces, and reset state remain identical.
RESET may settle a reconciled prefix even when the previous COMMIT_ACK was lost; it does
not delete immutable committed input records. These recovery codecs remain
dormant until the F and C recovery lifecycles pass their runtime gates.

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

Independent C/F relationships can make progress concurrently; this is not
multi-TU pipelining on one socket. C-side admission permits one active source
operation per exact F-store GUID/generation, across all profiles, and joins
endpoint aliases onto that same gate before SESSION_HELLO. The default C-side
limits are four active source operations and 2 GiB of reserved source-vector
lengths in total. These limits do not account for all retained codec state or
process memory. Route state remains owned by one executor, and production
retry connection setup runs outside that executor.

CacheWire revision 1 still transfers one TU per ordinary connection. No new
capability bit, wire field or P43 layout accompanies local concurrency. The
[implementation and acceptance contract](../doc/p50-transfer-concurrency.md)
separates independent-link concurrency from future persistent-link/pipeline
extensions and specifies the required tests; it is not a validation receipt.

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
