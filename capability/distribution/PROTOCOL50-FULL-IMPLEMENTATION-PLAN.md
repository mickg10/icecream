# Protocol 50: Full Icecream Implementation Plan

**Status:** implementation blueprint for issue #16  
**Primary branch:** `implementer/issue16-capability`  
**Scope:** a complete, negotiated Protocol-50 source-input path in Icecream, with production P29 and GRZ profiles, exact fallback, restart handling, simulator correspondence, test-farm validation, and a small formal model.

---

## 1. Definition of done

Protocol 50 is complete when all of the following are true:

1. A current Icecream client and worker can negotiate Protocol 50 and transfer one translation unit through a persistent cache relationship.
2. The worker reconstructs the exact preprocessed byte stream and feeds it through the existing compiler lifecycle.
3. The established Protocol-44/legacy input path remains available and interoperable with old peers.
4. P29 and GRZ are real Protocol-50 codec profiles, not offline ledger generators.
5. Cold, warm, repeated-build, multi-F, restart, eviction, environment-install, retry, and fallback scenarios are exercised on the test farm.
6. Every accepted scenario retains exact directional byte ledgers, input digests, compiler results, state transitions, CPU/RSS, and timing.
7. A bounded TLA+/PlusCal model checks the protocol’s ordering and safety invariants, and implementation traces refine the same action vocabulary.
8. The simulator consumes the same scenario identities and physical transaction events as the implementation, so simulation and reality can be compared rather than maintained as separate stories.

Protocol correctness is independent of whether a particular codec reaches 400×. Codec selection and compression gates are measured on top of the same protocol.

---

## 2. Decisions frozen for the first implementation

```text
Language level                  C++23 for the whole project
Submitting topology             one C box -> one persistent C store/GUID
Workers                         independent F stores
Preprocessing in simulator      zero-time / immediately available
Primary environment assumption  already installed and long-term stable
Environment cold path           existing EnvTransfer protocol, joined with P50 readiness
Remote cache transport          dedicated persistent cache endpoint
Job transport                   existing per-job connection
Object identity                 (C_STORE_GUID, Key64)
Key64                           u64(type, generation, id)
Relationship ordering           route-local REL_SEQ
In-flight relationship window   one uncommitted TU
Restart unit                    replay the complete current TU
Partial-frame resume            not implemented
Immutable object commit         independent of TU/compiler success
Route-history commit            exact input reconstruction commit
Compiler start                  environment ready AND input materialized/verified
P29 and GRZ                     separate codec profiles over one P50 protocol
```

Explicit non-goals for the first implementation:

```text
multiple active TU dialogues on one C/F relationship
cross-F shared object storage
broadcast cache warming
packet-level network simulation
preprocessor CPU simulation
frame-level reconnect resume
wide acknowledgement windows
environment archives encoded as P29/GRZ objects
an ML selector in the correctness path
```

---

## 3. Process architecture

The smallest safe product shape is one cache-service sidecar per `iceccd`.

The sidecar is justified even though the project moves to C++23: `iceccd` forks job children. Starting an asynchronous multithreaded I/O runtime inside the forking daemon would introduce difficult post-fork constraints. The sidecar allows Asio threads and codec workers without changing the daemon’s fork assumptions.

```text
C host
------

icecc clone ---- existing local-daemon/job control ---- iceccd
     |
     +---- Unix socket PREPARE/ROUTE ---- icecc-cache-service
                                            C store
                                            one C_STORE_GUID
                                            interner/object catalogue
                                            PreparedTU store
                                            relationship to each used F

F host
------

iceccd ---- forks F job child ---- compiler
   |
   +---- supervises ---- icecc-cache-service
                          F store
                          one F_STORE_GUID
                          namespace per C_STORE_GUID
                          immutable objects
                          active transaction
                          route history
                          local ATTACH/TAKE_INPUT

network
-------

existing job TCP:
    environment archive when required
    CompileFile metadata and P50 input reference
    diagnostics, object file, CompileResult

dedicated persistent cache TCP:
    SESSION
    TX_BEGIN / DICT / BODY
    NEED / FILL
    TX_END / TX_COMMIT
    restart and history-reset control
```

A host that both submits and compiles runs one sidecar process containing independent C-store and F-store roles with different GUIDs.

---

## 4. Identity model

### 4.1 Store identities

```text
C_STORE_GUID : 128-bit random identity of one C-store incarnation
F_STORE_GUID : 128-bit random identity of one F-store incarnation
```

A destructive C-store restart creates a new `C_STORE_GUID`.  
A destructive F-store restart creates a new `F_STORE_GUID`.

An F namespace is keyed by `C_STORE_GUID`. Objects from different C GUIDs never alias even when their 64-bit keys are numerically equal.

### 4.2 Canonical object key

```text
full identity = (C_STORE_GUID, Key64)

Key64:
    bits 63..56   object type       8 bits
    bits 55..40   object generation 16 bits
    bits 39..0    object ordinal    40 bits
```

Initial rules:

- key zero is invalid;
- type zero is reserved;
- ordinals are monotonic within `(type, generation)`;
- an ordinal is never reused;
- generation is local allocation metadata, not a session epoch;
- old and new generations may coexist;
- one TU may reference multiple generations;
- there is no coordinated generation-roll protocol;
- before generation wrap, C flips `C_STORE_GUID` and starts a new namespace.

The first implementation may use generation zero indefinitely. Increment generation only for explicit compaction or allocator reset. Do not rotate on a timer merely because the field exists.

`Key64` is the canonical store and protocol identity. It need not consume eight bytes for every occurrence:

```text
DICT manifests:
    sort by Key64
    encode type/generation runs
    delta-varint ordinals

TU body:
    use dense LOCAL32 indices
```

There is no second stable-key-to-local-association plane.

### 4.3 TU and attempt identities

```text
TU_SEQ        u64, allocated when PreparedTU is published by C
ATTEMPT_ID    u64, unique compile attempt for one PreparedTU
REL_SEQ       u64, allocated when a C/F relationship accepts ROUTE
HISTORY_NONCE u64, identifies one ordered route-history branch
ENV_KEY       existing target/environment identity, preferably content-bound
```

`REL_SEQ` is route-local admission order. It is not required to be the sorted projection of global `TU_SEQ`.

A cache transaction is identified by:

```text
(C_STORE_GUID, F_STORE_GUID, HISTORY_NONCE, REL_SEQ, TU_SEQ, ATTEMPT_ID)
```

---

## 5. State and commit model

### 5.1 C-side PreparedTU

```text
PreparedTu {
    C_STORE_GUID
    TU_SEQ
    raw_length
    raw_digest
    dependency manifest
    transaction-local body
    reusable object references
    fallback source or exact reconstruction recipe
    per-profile immutable preparation
}
```

Publication is atomic. Before publication, no remote relationship may reference the TU.

A `PreparedTu` may be routed to a second F after failure without reinterning or receiving new object keys.

### 5.2 F object states

```text
ABSENT
INSTALLING
PRESENT
PINNED
```

A complete immutable object is installed immediately after its key, type, dependencies, length, and content digest validate.

Internal transition:

```text
OBJECT_APPLIED(Key64, content_digest)
```

Properties:

- same key and same content is idempotent;
- same key and different content is a fatal namespace error;
- a completely installed object survives TU cancellation and reconnect;
- an incomplete object is discarded;
- a pinned object cannot be evicted.

No object-level acknowledgement is required in the first wire protocol. On reconnect or retry, F recomputes `NEED` from its actual store. This keeps F authoritative and removes an acknowledgement window.

### 5.3 Route-history commit

The route-history transition is separate:

```text
INPUT_COMMITTED(
    HISTORY_NONCE,
    REL_SEQ,
    TU_SEQ,
    transaction_digest,
    raw_digest,
    post_state_digest
)
```

This transition occurs only after F:

1. has the complete transaction body;
2. holds and pins the required object closure;
3. materializes the exact raw input;
4. verifies raw length and digest;
5. verifies the pre-state digest;
6. computes the expected post-state digest.

Compiler success is not part of the cache commit. Compilation is a consumer of an already committed exact input.

### 5.4 Job attachment

Root/data arrival and the job connection are independent.

```text
cache_ready =
    INPUT_COMMITTED

compiler_ready =
    cache_ready
    AND ENV_READY
    AND live job attachment
```

The cache transaction may complete before the F job child exists. A later attachment can consume the retained input.

---

## 6. Protocol 50 wire design

### 6.1 Dedicated cache endpoint

Each F advertises through daemon Login:

```text
P50 capability/version
cache host/port
supported codec profiles
```

The scheduler returns the cache endpoint with `UseCS`. It forwards metadata only; it owns no cache state.

This avoids:

```text
CACHE_SESSION classification on the existing job listener
MsgChannel read-ahead fencing
descriptor extraction and handoff
mixed ownership between MsgChannel and Asio
```

### 6.2 Cache frame

Use one canonical frame on the dedicated endpoint:

```cpp
struct FrameHeader {
    uint32_t payload_bytes_be;
    uint16_t message_type_be;
    uint16_t flags_be;
};
```

Rules:

- payload maximum: 1 MiB initially;
- larger logical components are chunked;
- network byte order;
- unknown required flags reject the session;
- unknown optional message types are ignored only when negotiated;
- parser consumes the complete payload exactly;
- checked arithmetic precedes every allocation;
- compressed components declare raw and encoded sizes;
- decompressed size and configured limits are verified before allocation.

TCP supplies ordered bytes inside one connection. Protocol state, digests, and replay rules supply application correctness.

### 6.3 Message set

```text
SESSION_HELLO
SESSION_STATE
HISTORY_RESET
SESSION_ERROR
KEEPALIVE

TX_BEGIN
DICT_CHUNK
DICT_END
NEED
BODY_CHUNK
BODY_END
FILL_CHUNK
FILL_END
TX_END
TX_COMMIT
TX_ERROR
```

One active transaction means messages do not need a general stream multiplexer. Every transaction message still carries enough identity to reject stale or mismatched traffic.

### 6.4 Session handshake

C sends:

```text
SESSION_HELLO {
    protocol version range
    C_STORE_GUID
    supported profiles
    desired limits
}
```

F returns:

```text
SESSION_STATE {
    selected protocol version
    F_STORE_GUID
    selected limits/profiles
    namespace present?
    current HISTORY_NONCE
    next REL_SEQ
    committed state digest
    last committed REL_SEQ/TU_SEQ/transaction digest
}
```

Only one session may advance a `(C_STORE_GUID, F_STORE_GUID)` namespace. A new accepted session fences the old connection. Frames arriving from a fenced session are discarded.

### 6.5 Transaction flow

```text
C -> F  TX_BEGIN
C -> F  DICT_CHUNK*
C -> F  DICT_END
C -> F  BODY_CHUNK* -------------------------------+
F -> C                 NEED                        |
C -> F                       FILL_CHUNK* / FILL_END|
C -> F  BODY_END                                     |
C -> F  TX_END --------------------------------------+
                                                     |
F materialize + digest verify + history commit        |
F -> C  TX_COMMIT <-----------------------------------+
```

`DICT_END` contains everything F needs to calculate one exact `NEED`. F pins already-present dependencies at that point. Missing dependencies are pinned as they are installed.

`NEED` is sorted and unique.

Fill and body may overlap. `TX_END` may arrive before the final Fill; F waits for all commit conditions.

`TX_COMMIT` includes the transaction digest, raw digest, and post-state digest.

### 6.6 Job-channel input descriptor

Protocol 50 extends `CompileFileMsg`:

```text
InputDescriptor {
    mode = LEGACY_CHUNKS | P50_REFERENCE

    if P50_REFERENCE:
        C_STORE_GUID
        TU_SEQ
        ATTEMPT_ID
        ENV_KEY
        raw_length
        raw_digest
}
```

The existing target platform and environment version remain in the compile job. `ENV_KEY` binds the readiness join and should not silently identify different content over time.

One attempt uses one input mode. A failed cache attempt is followed by a new legacy attempt; the two input encodings are never spliced.

---

## 7. Restart and reconnect

One active transaction permits whole-transaction replay and no frame-level recovery.

### 7.1 On disconnect

```text
partial frame                    discard
active F transaction overlay     discard
complete immutable objects       retain
committed route history          retain
C encoded/current PreparedTU     retain
job attachment                   independent
```

### 7.2 Reconnect cases

#### Case A: exact state match

```text
same F_STORE_GUID
same HISTORY_NONCE
same next REL_SEQ
same state digest
```

Replay the current TU from `TX_BEGIN`, or send the next TU.

#### Case B: final TX_COMMIT was lost

F reports exactly one additional committed transaction and its retained transaction digest matches C’s active transaction.

C accepts that commit and advances without replaying the TU.

#### Case C: F store restarted or namespace vanished

```text
different F_STORE_GUID
or no namespace for C_STORE_GUID
```

Treat object state as cold. Create a new `HISTORY_NONCE`, set `REL_SEQ=0`, and let `NEED` repopulate objects.

#### Case D: object arena survives but route history disagrees

Do not repair the history log.

```text
retain immutable object arena
create a new HISTORY_NONCE
set REL_SEQ=0
use P29/FI/Zstd without old history dependencies until the route warms
```

### 7.3 C restart

A C-store restart creates a new `C_STORE_GUID`. Existing F namespaces under the old GUID remain unreachable except to retained old C state and are eventually evicted.

### 7.4 What is deliberately absent

```text
partial-frame resume
per-frame acknowledgement windows
log reconciliation
cross-session speculative state
generation-roll handshake
```

---

## 8. Compiler environment integration

The current environment protocol remains the payload mechanism.

Current behavior is:

```text
C client -> F daemon:
    EnvTransferMsg
    environment archive chunks
    End
    optional VerifyEnv
    then CompileFileMsg
```

The F daemon installs the environment through an installer child. The compile child is created later.

Protocol 50 adds a readiness join, not a second environment encoding:

```text
ENV_READY(F, ENV_KEY)
AND
INPUT_COMMITTED(F, C_GUID, TU_SEQ)
AND
job attached
    -> compiler input may start
```

Primary farm and simulator scenarios assume `got_env=true`.

The cold-environment stress scenario uses the existing transfer and permits overlap:

```text
branch A: environment transfer/install/verify
branch B: P50 cache transaction
join: compiler start
```

Environment transfer is single-flight per `(F, ENV_KEY)`. Its bytes and time are reported separately from source-codec compression.

Required environment failure behavior:

- stale `got_env=true` followed by missing/damaged environment;
- two jobs concurrently requiring the same environment;
- owner job cancellation during install;
- install succeeds but verification fails;
- F restart preserves environment files but loses P50 object state;
- environment evicted after scheduling;
- environment identity reused for different content.

---

## 9. Codec profile interface

One Protocol-50 implementation hosts several profiles.

```cpp
class CodecProfile {
public:
    virtual ProfileId id() const = 0;

    virtual PreparedEncoding prepare(
        const RawTu&,
        CObjectCatalogue&,
        CRouteHistory&) = 0;

    virtual Manifest build_manifest(
        const PreparedEncoding&,
        const FKnowledgeEstimate&) = 0;

    virtual FillEncoding encode_fill(
        std::span<const Key64> need,
        const CObjectCatalogue&,
        const CRouteHistory&) = 0;

    virtual DecodeResult decode_and_materialize(
        const ReceivedTransaction&,
        FObjectStore&,
        FRouteHistory&) = 0;

    virtual void commit_history(const ExactInput&) = 0;
    virtual void abort_history() = 0;
    virtual void reset_history(HistoryNonce) = 0;
};
```

Every history-bearing implementation has `prepare / commit / abort`. Sending bytes never advances committed history.

### 9.1 `P50_ZSTD_TU`

A stateless exact baseline:

```text
DICT empty
BODY = independently framed zstd-1 or zstd-3 raw TU
no reusable objects
no route-history dependency
```

This is the P50 fallback and protocol calibration profile.

### 9.2 `P50_P29`

P29 must explicitly classify every physical component:

```text
A. transaction-local material
B. immutable reusable Key64 object
C. ordered route-history transport delta
```

Recommended reusable object kinds:

```text
Path
Line
PublicLine
Region
Block
Blob
CanonicalMO
ByteArrayTemplate
other exact material proven reusable
```

The canonical content of a Block is its exact child sequence. A route-local COPY program may be used to transmit that object, but it is not the object’s identity.

The DICT manifest contains every reusable dependency. F requests missing keys. The BODY contains the Root/LOCAL32 recipe and truly one-use residual material.

The current capability stream’s large “LINES teaching” component must be audited: reusable content belongs in Key64 objects and Need/Fill; genuinely one-use content remains BODY. This classification is a binding product task, not a reporting rename.

P29 transaction commit updates its route occurrence history only after exact input commit.

### 9.3 `P50_GRZ`

GRZ is a route-history profile:

```text
DICT normally empty
BODY = one independently bounded current-TU GRZ frame
history = exact raw bytes committed on this relationship
```

Rules:

- COPY sources must lie in committed retained history;
- C and F histories advance only on `INPUT_COMMITTED`;
- retry produces the identical frame;
- history mismatch creates a new `HISTORY_NONCE`;
- malformed or unavailable COPY source rejects the transaction;
- one current-TU frame is the first product mode.

### 9.4 Profile selection

The first production sessions explicitly select P29, GRZ, or Zstd.

Do not begin with a per-TU ML selector. A relationship may change its history-bearing profile only by starting a new `HISTORY_NONCE`; immutable object state remains available.

After both profiles are proven, an `auto` policy may choose at relationship/build boundaries using exact measured bytes and route state.

---

## 10. Source-tree implementation map

### 10.1 C++23 migration

Early, isolated commit:

```text
configure.ac
    require C++23
    verify std::span, std::expected, std::jthread, std::endian

CI/test farm
    GCC 13+
    Clang 17+
    debug, release, ASan/UBSan, TSan where practical
```

Do not use modules or exotic library features merely because C++23 is available.

### 10.2 New `cache/` subsystem

```text
cache/cache_types.*
    GUIDs, Key64, TU/attempt/relationship IDs, digests

cache/cache_frame.*
    canonical framing, parser, bounds, component chunking

cache/cache_protocol.*
    messages, state enums, transition results

cache/object_store.*
    immutable C/F object stores, pinning, eviction

cache/prepared_tu.*
    publication and fallback source

cache/c_store.*
    interner authority, object allocation, Need->Fill

cache/f_store.*
    manifest receive, Need, object install, materialization

cache/relationship.*
    handshake, one active transaction, reconnect

cache/profile.*
cache/profiles/zstd_tu.*
cache/profiles/p29.*
cache/profiles/grz.*

cache/local_protocol.*
    C clone PREPARE/ROUTE
    F clone ATTACH/TAKE_INPUT

cache/cache_channel.*
    standalone Asio reader/writer and bounded queues

cache/trace.*
    formal action names and exact ledgers

cache/service_main.cpp
    combined C/F sidecar roles
```

Plain types, stores, and codecs do not include Asio.

### 10.3 Existing Icecream changes

```text
services/comm.*
    protocol version 50
    cache endpoint/capability fields
    CompileFile InputDescriptor

scheduler/*
    forward advertised cache endpoint/capability
    no object knowledge in first implementation

client/remote.cpp
    local C-cache prepare
    route selected PreparedTU
    environment/cache overlap
    legacy/cache/auto attempt selection

daemon/main.cpp
    start/watch sidecar
    advertise cache endpoint
    keep existing environment handling

daemon/serve.cpp + daemon/workit.cpp
    select compiler input source:
        LegacyChunkSource
        CacheAttachmentSource

top-level build
    build/install sidecar and cache library
```

The existing job result path remains unchanged.

---

## 11. Vertical implementation milestones

The implementation should proceed in vertical, runnable slices. The simulator, formal model, and codec work run in parallel.

### Milestone 0 — language, identities, and formal skeleton

Deliver:

- C++23 build;
- Key64 and GUID types;
- frame parser;
- protocol state enums;
- initial PlusCal/TLA+ model;
- action/event vocabulary.

Gate:

```text
all old tests pass under C++23
Key64 and frame unit tests pass
TLC safety checks pass on the small model
```

### Milestone 1 — in-memory P50 with real P29 objects

One process, independent C and F stores, no sockets.

Deliver:

- PreparedTU publication;
- manifest, Need, Fill;
- independent object install;
- exact materialization;
- route commit;
- abort/retry;
- P29 profile using real capability fixtures.

Gate:

```text
cold/warm exact reconstruction
same-key/different-content rejection
failure at every transition leaves allowed state only
future-suffix prefix invariance
```

### Milestone 2 — local sidecar vertical path

Deliver:

- sidecar process;
- C clone PREPARE over Unix socket;
- F clone ATTACH/TAKE_INPUT over Unix socket;
- real preprocessor output accepted by C store;
- exact input fed into a real compiler pipe.

Gate:

```text
one real TU compiles through sidecar
clone exits at every local-message boundary
sidecar retains or discards state exactly as specified
```

### Milestone 3 — remote P50 with Zstd profile

Deliver:

- dedicated F cache listener;
- session handshake;
- one active relationship transaction;
- `P50_ZSTD_TU`;
- input reference on job channel;
- old/new capability negotiation.

Gate:

```text
two hosts/containers compile real TUs
cold and warm repeat
all directional bytes close exactly
old-C/new-F and new-C/old-F use legacy
```

### Milestone 4 — full P29

Deliver:

- complete P29 object classification;
- Key64 DICT;
- Need/Fill;
- Root/body;
- P29 route history;
- cold/warm and repeated-build retention.

Gate:

```text
fixed corpus suite exact
Firefox 1F cold + four warm exact
physical bytes match profile ledger plus P50 framing
```

### Milestone 5 — restart, environment join, eviction, fallback

Deliver:

- reconnect four-case table;
- lost final commit recovery;
- F GUID flip;
- history-only reset;
- pinned-object eviction rules;
- existing environment transfer joined with P50;
- cache-attempt to legacy retry.

Gate:

```text
fault injection at every protocol boundary
no duplicate accepted compile result
no phantom history or mutable object identity
all failed-attempt bytes retained in ledger
```

### Milestone 6 — full GRZ

Deliver:

- GRZ current-TU frame profile;
- bounded history;
- retry identity;
- history reset;
- malformed COPY rejection.

Gate:

```text
same corpora and farm scenarios as P29
1F and multi-F exact
restart and history mismatch exact
```

### Milestone 7 — multi-F, routing, simulator alignment

Deliver:

- static rendezvous;
- dense frontiers 1/2/3/4/8/20;
- primary home plus bounded spill;
- F restart injection;
- exact farm traces imported into simulator.

Gate:

```text
bytes identical between farm ledger and simulator input
event-order invariants pass
predicted versus observed stage timing reported
```

### Milestone 8 — measured policy work

Only after the preceding milestones:

```text
auto P29/GRZ/Zstd selection
presend/bootstrap
online cache-aware routing
direct F-cache-to-compiler pipe
multiple Asio I/O shards
```

---

## 12. Formal model

Add:

```text
formal/protocol50/Protocol50.tla
formal/protocol50/Protocol50.cfg
formal/protocol50/Protocol50_2F.cfg
formal/protocol50/README.md
tools/check_p50_trace.py
```

Small-state model:

```text
1 C store
1 or 2 F stores
2 TUs
2 immutable objects
one cache and one legacy attempt
disconnect/reconnect
eviction
environment ready/not-ready
```

Actions:

```text
Prepare
Route
OpenSession
BeginTx
ReceiveDict
ComputeNeed
InstallObject
ReceiveBody
EndTx
Materialize
CommitInput
AckCommit
AttachJob
EnvironmentReady
StartCompiler
Disconnect
Reconnect
ResetHistory
RestartF
RestartC
EvictObject
StartLegacyRetry
AcceptResult
```

Safety invariants:

```text
Key64 content is immutable.
One relationship has at most one active uncommitted REL_SEQ.
Committed REL_SEQ values are contiguous within one HISTORY_NONCE.
INPUT_COMMITTED implies exact raw length/digest.
INPUT_COMMITTED implies complete pinned closure during materialization.
Object publication never advances route history.
A stale/fenced session cannot mutate state.
A pinned object cannot be evicted.
The same attempt cannot mix P50 and legacy input.
At most one attempt result is accepted for one logical job.
A history-dependent COPY uses only committed retained history.
All modeled byte/item credits remain bounded.
```

Liveness properties under no further failures:

```text
an active transaction eventually commits or fails/falls back;
a committed input with ENV_READY and a live attachment eventually starts compilation;
a single-flight environment installation eventually wakes all valid waiters.
```

The implementation trace uses the same action names. Acceptance runs fail if a trace event cannot be mapped to the model vocabulary.

---

## 13. Test strategy

### 13.1 Unit tests

#### Identity and encoding

- every Key64 boundary;
- reserved type and zero key;
- canonical pack/unpack;
- sorted/delta key lists;
- mixed-generation manifests;
- ordinal and generation overflow;
- GUID flip;
- noncanonical/overflowing varints.

#### Frame parser

- every split of the 8-byte header;
- every split of representative bodies;
- zero-length legal controls;
- payload limit minus one, limit, limit plus one;
- truncated body;
- unknown type/required flag;
- trailing bytes;
- integer-sum overflow;
- malformed compressed component;
- declared decompressed-size violation.

#### Object store

- empty Need;
- all-missing Need;
- partial Need;
- duplicate keys;
- same key/same content;
- same key/different content;
- dependency ordering;
- dependency cycle rejection;
- pin/unpin;
- eviction before/after pin;
- install concurrent with Need calculation.

#### Codec profiles

- empty TU;
- one-byte TU;
- no reusable objects;
- all reusable objects;
- huge literal;
- P29 route-local COPY source available/unavailable;
- GRZ ring wrap and overlapping-copy semantics;
- deterministic retry;
- prepare/commit/abort mutations.

### 13.2 In-process protocol tests

Inject failure before and after every transition:

```text
TX_BEGIN
each DICT chunk
DICT_END
NEED
each BODY chunk
BODY_END
each FILL chunk
FILL_END
TX_END
materialization
INPUT_COMMITTED
TX_COMMIT delivery
```

After each failure, assert exactly which state survives.

### 13.3 Multiprocess loopback tests

- C and F sidecars over TCP;
- several C clones preparing concurrently;
- several F job children attaching concurrently;
- one active dialogue per relationship;
- different relationships progress concurrently;
- compile of TU q overlaps transfer of q+1;
- sidecar shutdown while clones are connected;
- bounded queue and memory backpressure;
- slow-reader/slow-writer behavior.

### 13.4 Compatibility matrix

```text
old C / old F
old C / new F
new C / old F
new C / new F in legacy mode
new C / new F in P50 Zstd
new C / new F in P29
new C / new F in GRZ
```

Protocol versions below 50 must preserve their prior serialization.

---

## 14. Protocol edge-case matrix

### Session and restart

- disconnect in each header byte and representative body byte;
- old and new sockets simultaneously alive;
- stale old-session frame after fencing;
- same endpoint with new F GUID;
- same F GUID with missing route history;
- C GUID changes while old F namespace remains;
- lost final `TX_COMMIT`;
- F exactly one commit ahead;
- same REL_SEQ with a different transaction digest;
- history reset while objects remain;
- idle relationship close and later resume;
- retry the same PreparedTU on another F.

### Transaction and attachment

- Root/data before job reference;
- job reference before Root/data;
- no job attachment;
- attachment cancellation before ready;
- cancellation after ready but before input take;
- replacement attachment;
- duplicate active attachment;
- body complete before Fill;
- Fill complete before body;
- `TX_END` before missing objects arrive;
- exact input commits while environment is still installing;
- environment ready before input;
- compiler exits early;
- compiler result arrives after a newer retry result.

### Need and Fill

- object becomes present after Need but before Fill;
- duplicate Fill;
- subset/superset Fill;
- Fill object order changes;
- one Fill contains an invalid later object after valid earlier objects;
- complete objects survive; partial object does not;
- Need contains duplicate or unsorted keys;
- object evicted before DICT;
- eviction attempted after pin;
- dependency closure missing despite Root present.

### Route history

- `REL_SEQ+1` queued while current transaction compiles;
- current transaction aborts;
- history digest mismatch;
- GRZ source falls outside retained window;
- P29 COPY source belongs to another F route;
- profile change starts a new history nonce;
- F restart with environment retained;
- edit/revert and A-B-A;
- static route remap between builds.

### Environment

- scheduler says installed but F reports missing;
- two first jobs install the same environment;
- installer owner exits;
- environment verification fails;
- environment transfer ends before P50 input;
- P50 input ends before environment transfer;
- environment is evicted after assignment;
- environment key aliases different archive content.

### Resource bounds

- prepared queue full by TU count;
- prepared queue full by bytes;
- relationship send queue full;
- Fill-reserved queue capacity;
- F pending transaction limit;
- F materialized-input limit;
- object-store high-water eviction;
- huge TU;
- many tiny TUs;
- one slow F among fast Fs;
- one large Fill among small bodies;
- cancellation releases every credit exactly once.

---

## 15. Test-farm scenario suite

Every scenario uses the real scheduler, `iceccd`, sidecars, real compiler, and exact corpus bytes.

```text
T00  legacy reference
T01  P50 Zstd cold/warm 1C/1F
T02  P29 cold + four warm builds 1C/1F
T03  GRZ cold + four warm builds 1C/1F
T04  P29 and GRZ width 1/2/3/4/8/20, round-robin
T05  same widths, stable rendezvous
T06  same widths, dense frontier and primary-home+spill
T07  F sidecar restart mid-build
T08  cache-channel drop at every transaction boundary
T09  lost final commit acknowledgement
T10  object eviction and Need recovery
T11  environment missing, transfer overlapped with P50
T12  environment verification failure and retry
T13  cache attempt failure -> legacy retry
T14  C sidecar restart/new C GUID
T15  concurrent C clones requesting overlapping objects
T16  malformed-frame corpus
T17  header edit, generated-file edit, revert, A-B-A
T18  mixed old/new protocol peers
T19  result-return traffic and complete build ledger
```

Long corpus runs are not normal unit tests. Provide:

```text
make check
    unit tests

make protocol50_integration
    bounded multiprocess loopback/compatibility suite

make protocol50_farm
    named real-host/container scenarios

make protocol50_bench
    large corpora and performance reports
```

---

## 16. Simulator–reality convergence

The simulator remains a policy laboratory, not a substitute for the implementation.

Primary simulator assumptions:

```text
all TUs immediately available
preprocessing time = 0
compiler environment resident
exact physical codec bytes
independent F stores
real compile-duration trace
```

Optional environment scenarios add the explicit `ENV_ENSURE` branch.

The real implementation emits a canonical event ledger:

```text
ScenarioId
C/F GUIDs
TU_SEQ / REL_SEQ / ATTEMPT_ID
action name
before/after state
frame type and physical bytes
object counts/bytes
raw and transaction digests
timestamps
CPU stage durations
queue and memory high-water marks
```

Convergence checks:

1. **Bytes:** exact equality between implementation ledger and simulator physical input.
2. **Ordering:** every real event refines a permitted protocol action.
3. **State:** committed route/object digests match replay.
4. **Timing:** real stage distributions calibrate simulator service times.
5. **Prediction:** report simulator-versus-farm error for input-ready time, build makespan, and queue high-water marks.

Do not tune the simulator to one aggregate makespan while stage-level errors remain hidden.

---

## 17. Required reports

Each accepted scenario retains:

```text
00 resolved configuration and code/model hashes
01 process commands, PIDs, endpoints, negotiated versions
02 exact C->F cache bytes by frame/component
03 exact F->C cache bytes
04 environment-transfer bytes and install/verify time
05 job-channel/result bytes
06 per-TU raw/output digests and compiler status
07 object-store and route-history state curves
08 Need/Fill and retry ledger
09 CPU, RSS, queue and buffer high-water marks
10 timeline: input ready, compiler start/end, transaction commit
11 formal trace-refinement result
12 simulator comparison
13 final pass/fail scorecard
```

Source-compression ratios use cache-source bytes only. Complete network reports keep environment and result traffic in separate columns and in the grand total.

---

## 18. Acceptance gates

### Protocol gate

- exact input for every TU;
- no duplicate accepted result;
- old-peer fallback;
- restart/failure matrix passes;
- bounded memory/queues;
- TLA+ safety checks pass;
- implementation trace refinement passes.

### P29 gate

- cold/warm exactness across the fixed suite and Firefox;
- reusable material is represented as objects or explicitly declared inline;
- physical bytes equal P50 frames plus profile payload;
- chronological learning curves reported per F.

### GRZ gate

- exact current-TU frames;
- bounded retained history;
- deterministic retry;
- restart/history-reset correctness;
- no cross-F history reference.

### Farm gate

- 1/2/3/4/8/20-F scenarios;
- environment-cold stress;
- reconnect, eviction, fallback;
- compiler result correctness;
- complete directional ledgers.

Compression and speed targets are reported separately from protocol correctness. A codec may fail a compression target without invalidating the P50 implementation.

---

## 19. Parallel work allocation

### Product/protocol lane

- C++23 migration;
- cache types/frame/protocol;
- sidecar;
- client/daemon/scheduler integration;
- environment and fallback join;
- restart and eviction.

### Codec lane

- extract P29 into the profile interface;
- classify P29 reusable versus inline material;
- integrate GRZ current-TU profile;
- retain byte-identical standalone controls.

### Verification/measurement lane

- TLA+/PlusCal model;
- implementation trace checker;
- simulator adapters;
- farm launcher and reports;
- independent exact replay.

The lanes merge at the small vertical gates, not after three separate frameworks are complete.

---

## 20. Immediate commit sequence

```text
1. Add this plan and freeze the identity/restart/message contracts.

2. Move the whole build to C++23 and establish the compiler/CI floor.

3. Add cache_types, Key64, frame parser, protocol actions, and TLA+ skeleton.

4. Build the in-memory P29 transaction with independent C/F stores.

5. Add the sidecar and local PREPARE/ATTACH path; compile one real TU.

6. Add the dedicated remote cache endpoint and P50 Zstd profile.

7. Add real P29 cold/warm.

8. Add restart, environment join, eviction, and legacy fallback.

9. Add GRZ.

10. Run the static multi-F farm matrix and align simulator traces.
```

This sequence gets a real Protocol-50 compile running early, while every subsequent feature extends the same state machine and test harness.
