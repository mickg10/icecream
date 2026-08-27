# Protocol 50 M0/M1 core and M2 loopback endpoint

This directory is the product-facing cache protocol boundary. M0 freezes identities, closure,
framing, the ten-message vocabulary, action traces, and the bounded formal model. M1 makes the
two-party transaction executable in memory with the real `p29::OnlineS1` and
`p29::BlockCatalogue`. M2 adds one real Boost.Asio TCP dialogue per C/F relationship and uses
the history-independent `P50_ZSTD_TU` profile as the first vertical path. It is still a
standalone loopback endpoint: there are no cache sidecars, daemon hooks, compiler pipes, or
on-disk storage in this milestone.

## Build baseline and cluster census

The repository baseline is C++23. M2 raises the mandatory Boost minimum to 1.74, the first
verified Boost release for the selected C++20 coroutine path. Configure compile-probes the
exact `boost::asio::awaitable`, `co_spawn`, `use_awaitable`, and asynchronous TCP
accept/connect/read/write APIs. Boost.System is linked only on Boost/toolchain combinations
that require it. Configure centrally selects Boost.Asio's standard-coroutine path; this also
handles Boost 1.74's older Clang detection without caller-provided compatibility flags. Boost
1.74 on the gate host is header-only for this probe. Protocol-50 digest code separately
requires libxxhash 0.8.0 or newer and its XXH3-128 one-shot and streaming APIs.

| artificial-cluster host | compiler | system Boost | system xxHash |
|---|---|---|---|
| `nas642` | GCC 11.4 | 1.55 headers (below baseline) | 0.8.1 runtime, development files absent |
| `tt-quietbox2` | GCC 11.4 | headers absent | 0.8.1 runtime, development files absent |

Both installations therefore need the current Boost and xxHash development packages before a
later product slice is deployed. M0/M1/M2 were built against extracted Boost 1.74/xxHash 0.8.1
development packages without changing either host installation.

## Canonical identities and allocation points

- `CStoreGuid` (`C_STORE_GUID`) and `FStoreGuid` (`F_STORE_GUID`) are independent 128-bit
  store-incarnation identities. An F object namespace is selected by `C_STORE_GUID`; equal
  Key64 numbers belonging to two C stores never share data.
- `Key64` uses `KeyLayoutV1`: 5 type bits, 10 generation bits, and 49 ordinal bits. Type zero
  and ordinal zero are invalid. Generation zero is valid. Advancing generation retains older
  objects; exhausting generation 1023 requires a new `C_STORE_GUID`, with no handshake or
  wrapped generation.
- `TU_SEQ` is allocated only after a `PreparedTU` has validated and becomes immutable. It is
  C publication order, not route order.
- `REL_SEQ` is allocated when one `CRoute` accepts a TU. The tests route TU_SEQ 1 at REL_SEQ 0,
  then TU_SEQ 0 at REL_SEQ 1.
- `HISTORY_NONCE` is an unsigned 64-bit relationship value. A reset selects a new value,
  returns REL_SEQ to zero, and leaves immutable objects in place.

Historical capability identifiers and structs remain unchanged. The one-way adapter is the C
preparation path: local dense Region IDs enter the real OnlineS1 matcher, and its Region/Block
root is translated to product Key64 objects. Dense IDs never become product identities.
`ProfileId` names negotiated codecs (`P29`, `ZSTD_TU`, and the reserved future `GRZ`); P29's
route-history versus history-independent Root choice is the separate `P29RootMode` field.

## Physical P29 ownership

| P29 component | Product representation | Lifetime |
|---|---|---|
| Line/Atom/Material/Path/Blob bytes | content-interned Key64 byte object | immutable reusable object under C_STORE_GUID |
| Region definition | content-interned Key64 child object | immutable reusable object |
| canonical Block children | content-interned Key64 Block object | immutable reusable object |
| shared `BlockCatalogue` and dense-to-Key64 tables | `CAuthority` lookup state | C-store lifetime |
| exact bytes, digest, Region root, dense IDs, TU_SEQ | immutable `PreparedTU` | C transaction preparation |
| `TuPlan` and `OnlineS1` undo journal | selected `CRoute` matcher | pending C transaction |
| root, exact manifest, DICT/BODY bytes, pure `TxBegin` | `CActiveTx` | one C ACTIVE_TX |
| copied `TxBegin`, exact DICT/BODY accumulation, parsed root/manifest, original and remaining Need sets, partial FILL record, exact materialized result | `FPending` | one F ACTIVE_TX |
| committed OnlineS1 occurrences/predecessors | C route matcher | ordered relationship state |
| F route cursor | nonce, next REL_SEQ, state digest, optional last `TxCommit` | ordered relationship state |

F has no P29 occurrence stream, source positions, generic route-history delta, or copied
`PreparedTU`. The cache transaction contains no compiler-environment state. Compiler readiness
and result attachment are a separate join outside this state machine.

The C interner indexes actual canonical payloads and checks content after digest lookup. Feeding
the same independently-created raw Region vectors through `prepare_from_regions()` returns the
same Line and Region keys. The warm P29 gate then requests only a new Block object.

## Wire closure

Every frame starts with one big-endian u32. Its high 8 bits are the message type and its low
24 bits are payload length. M0 caps one payload at 1 MiB. The exact message vocabulary is:

1. `SESSION_HELLO`
2. `SESSION_STATE`
3. `HISTORY_RESET`
4. `ERROR`
5. `TX_BEGIN`
6. `DICT`
7. `BODY`
8. `NEED`
9. `FILL`
10. `TX_COMMIT`

`ERROR` terminates the current message session; no later frame is accepted on that session.
`SESSION_HELLO` is the reserved additive negotiation point: it carries a minimum/maximum
protocol version, supported-profile mask, offered frame cap, and offered FILL-record cap.
`SESSION_STATE` returns the selected version, profile-mask intersection, and the smaller
negotiated caps. V1 rejects a frame cap below the 152-byte mandatory-control minimum or above
1 MiB, and rejects a FILL-record cap smaller than its fixed record prefix.
There is no attempt identifier. One current session plus the `(HISTORY_NONCE, REL_SEQ, TU_SEQ)`
tuple identifies work. `TxBegin` is a pure value containing that tuple, profile, pre-state
digest, exact DICT/BODY descriptors, expected raw byte count/digest, and transaction digest.

DICT and BODY are independent append-only streams. Their descriptor byte counts and digests
must close exactly; zero-length streams are legal. Need's first payload declares total key count
and delta-stream bytes. The remaining bytes encode one sorted, unique Key64 set. F records that
exact original set and answers from its own object store. A Fill is a sequence of self-delimiting
object records and a record may span any number of frames. A complete requested object is applied
immediately. Repeating the exact object is a no-op; a later bad object does not roll back earlier
complete objects.

The closures are intentionally non-circular:

```text
TRANSACTION_DIGEST = XXH3-128(canonical semantic TX_BEGIN without its digest
                              || exact DICT || exact BODY || expected raw length/digest)

POST_STATE_DIGEST  = XXH3-128(pre-state || HISTORY_NONCE || REL_SEQ || TU_SEQ
                              || TRANSACTION_DIGEST)
```

Product content and closure digests all use `services/digest128.*`, the common C++ wrapper over
XXH3-128's established one-shot and streaming APIs. The original MD5 implementation remains
only for the existing client path. MD5 reached 0.588 GB/s locally but 0.465 GB/s on quietbox,
below the required 0.5 GB/s gate; the one permitted replacement candidate reached 8.382 GB/s
locally and 16.264 GB/s on quietbox. The product wrapper retains the 0.5 GB/s gate.
`TX_COMMIT` contains nonce, REL_SEQ, TU_SEQ, transaction digest, raw digest, and post-state
digest. C advances OnlineS1 only after the complete tuple matches its one ACTIVE_TX.

## M2 ZSTD_TU profile and endpoint ownership

M2 keeps codec mechanics and dialogue state as two blocks with a narrow join:

| State | Owner | Lifetime |
|---|---|---|
| reusable `ZSTD_CCtx`/`ZSTD_DCtx`, local level 1, profile descriptor checks | `ZstdTuCodec` | one endpoint/worker |
| compressed BODY, canonical empty DICT descriptor, raw count/digest, TU_SEQ | immutable `PreparedZstdTU` | prepared C work item |
| request-key index, TU allocator, reusable encoder, immutable prepared entries and reference counts | `P50PreparationAuthority` | one C store |
| C store GUID, observed F GUID, nonce/cursor, queued work, one active `TxBegin` | `P50ClientEndpoint` | C/F relationship |
| F store GUID and map keyed by C store GUID | `P50ServerEndpoint` | F-store incarnation |
| F route cursor, retained last commit, interrupted-begin identity | one F namespace | C-store/F-store relationship |
| exact component accumulation and copied `TxBegin` | F pending overlay | one active dialogue transaction |
| immutable exact bytes keyed by `(C_STORE_GUID, TU_SEQ)`, job-open state, and independent cursors | `InputRecordStore` | cache commit through logical-job close and final cursor release |
| socket, negotiated profile mask/limits, current non-reused session serial | client/server coroutine | one connection |

`p50_zstd.*` has no store GUID, session, route, reconnect, or commit logic. It prepares,
validates, compresses, and exactly materializes one TU. `p50_endpoint.*` owns the common outer
state which will host the P29 adapter later; it does not create another P29 matcher or object
store. M2 advertises only `ZSTD_TU`, while `SESSION_STATE` returns the full profile-mask
intersection and each `TX_BEGIN` must choose one member. The negotiated frame cap is at least
152 bytes so it can carry every mandatory V1 control frame. A local cap is checked against the
four-byte frame header before payload allocation, against `TxBegin` before component
accumulation, and again before decoded-output allocation. Terminal `ERROR` detail is truncated
to the negotiated peer cap.

The first vertical dialogue is deliberately small:

```text
C                                                        F
SESSION_HELLO ------------------------------------------->
              <----------------------------- SESSION_STATE
[cold/different route only]
HISTORY_RESET ------------------------------------------>
              <----------------------------- SESSION_STATE  (reset acknowledged)

TX_BEGIN ----------------------------------------------->
BODY (one Zstd frame over one or more BODY messages) --->
              <------------------------------- TX_COMMIT
C accepts the exact tuple; C then closes; F observes EOF.
```

ZSTD_TU sends no DICT, NEED, or FILL frames. Its DICT descriptor is canonical empty and its
logical empty Need set is closed when F accepts `TX_BEGIN`. Even an empty TU is represented by
one nonempty Zstd frame in BODY. The exact decoder accepts one frame only: trailing bytes and
concatenated empty or nonempty frames are rejected. The BODY is split into as many Protocol-50
BODY messages as the negotiated frame cap requires. Zstd contexts are reused; compression
level 1 is local configuration and is not a wire field.

Every asynchronous accept, connect, header read, payload read, write fragment, and final EOF
wait records an operation kind, C/F actor, non-reused session serial, and every identity known
at initiation. Before peer identity is learned, its GUID is explicitly zero/previously
observed; before `TX_BEGIN`, transaction fields are explicitly unbound. Once known, the stamp
contains both store GUIDs, nonce, REL_SEQ, TU_SEQ, and transaction digest. It is checked after
every await and before state changes. A store-incarnation reset invalidates all old session
serials without rewinding the allocator. An uncertain disconnect clears only F's
connection-local pending overlay and retains C's exact active identity. `ERROR` is terminal for
that message session and the socket is closed after it.

`SESSION_HELLO` is decoded, intrinsically validated, and fully negotiated before F binds or
mutates the named C namespace. Thus an incompatible reconnect cannot replace a live pending
dialogue. Initial contact with an absent namespace is cold. Once C has an acknowledged route,
the same F GUID reporting that namespace absent is terminal and does not erase C's active
identity. A changed F GUID is the separate verified-incarnation transition.

One C-wide `P50PreparationAuthority` owns preparation. Callers receive opaque handles which
are accepted only by their issuing authority. Preparation is idempotent over `(producer
session, request token)` bound to the exact input bytes. An exact retry decodes and compares the
retained immutable body, returns the same handle and `TU_SEQ`, and does not create another
owner. Only explicit `retain()` creates another owner; `release()` removes the entry and bytes
exactly when that count reaches zero. A different input under the same request key is rejected,
including a same-length change. Zero request-key fields are reserved. Live entry count and
retained encoded bytes are bounded, and a failed preparation or cap check consumes neither
`TU_SEQ` nor a handle ID. F retains a monotonic `HISTORY_NONCE` high-water per C namespace and
rejects equal or lower resets.

After exact materialization, the F state owner synchronously selects whether the logical job is
still open or already closed, before advancing the route. An open job publishes one immutable
`InputRecord`; a closed job validates the same commit without creating a compiler-visible key.
Each attachment gets an independent byte-zero cursor. Closing the job blocks new attachments,
while already-authorized cursors survive until release; collection then removes the record.
Replacing the F-store incarnation drops store-owned records and route state without invalidating
an already-authorized cursor. The endpoint no longer exposes a copied raw-input vector.

Mutable preparation, C relationship, F namespace, InputRecord, attach/close/collect, and reset
operations are owned by one executor thread. Multiple C namespaces may interleave on that
thread, but cross-thread calls are rejected. F admission applies aggregate bounds to live
sessions, namespaces, simultaneous encoded bytes, raw bytes, decoder-window budgets, retained
InputRecord count, and retained exact bytes. Reservations are acquired at `TX_BEGIN` and released
on commit, disconnect, replacement, or store reset. Completion/action diagnostics are separately
bounded; loss of diagnostic capacity marks that evidence incomplete without changing the
protocol result.

### Endpoint mutation to canonical action mapping

| Endpoint transition | Canonical action |
|---|---|
| F binds a new/replacing session to a C namespace | `SESSION_OPENED` / `SESSION_REPLACED` |
| current F session ends | `SESSION_DISCONNECTED` |
| F derives and installs the new route cursor | `HISTORY_RESET` |
| C publishes an exact active tuple | C `TX_BEGIN` |
| F accepts its first/equal interrupted tuple | F `TX_BEGIN` / `ACTIVE_REPLAYED` |
| F closes the canonical empty DICT and records its empty set | `DICT_COMPLETE`, `NEED_RECORDED` |
| F closes BODY | `BODY_COMPLETE` |
| profile materializes exact bytes | `INPUT_MATERIALIZED` |
| F advances the route and retains the complete tuple | `INPUT_COMMITTED` |
| C accepts the ordinary/retained final tuple | `COMMIT_ACCEPTED` / `LOST_COMMIT_ACCEPTED` |
| an idle same-F route mismatch installs a fresh route | `HISTORY_RESET`, then a new `TX_BEGIN` |
| a same-F route mismatch while C retains active work | terminal result; no route action and the exact active tuple remains |
| C observes a changed F GUID while retaining prepared work | `F_STORE_INCAR_REPLACED`, then a cold `HISTORY_RESET` and new `TX_BEGIN` |

A changed F GUID is handled as an explicit incarnation-replacement observation and a whole new
attempt; it is not emitted as an ordinary abort on the new relationship. A stale asynchronous
completion is not a product mutation and therefore emits no alternate action: its complete
operation stamp is rejected before state changes.

## Four reconnect outcomes

| Resume observation | C action | F result |
|---|---|---|
| exact F GUID, nonce, REL_SEQ, and state digest | replay the same pure `TxBegin` and streams | complete P29 objects survive and F recomputes Need; ZSTD_TU repeats its exact body |
| F exactly one commit ahead with the retained complete `TxCommit` | accept the retained tuple once | route cursor and exact committed result stay advanced |
| first contact and absent C namespace | start nonce/REL_SEQ 0 after the reset acknowledgement | the initial F namespace is populated from the selected profile |
| established relationship and changed F GUID | emit `F_STORE_INCAR_REPLACED`, preserve prepared work, and create a whole new history-independent attempt | the replacement incarnation gets a new namespace |
| established relationship, same F GUID, absent C namespace | terminal inconsistency; preserve active/prepared C work | no cold reset is authorized |
| same object store, idle C route, but missing/different F route cursor | reset outside ACTIVE_TX and create a new history-independent attempt | P29 retains immutable objects; ZSTD_TU has no object set |
| same object store and unresolved C active tuple, but mismatched F route cursor | terminal inconsistency; retain the exact C tuple for reconciliation | do not reset the reported F route |

A new session replaces the old session for that C namespace and clears only F's incomplete
transaction overlay. Endpoint session serial allocation never wraps or rewinds: after allocating
`UINT64_MAX`, all new sessions fail for that endpoint object, including after an F-store reset.
Every in-memory session handle also carries its `F_STORE_GUID`, so an identity from a different F
or from a reset incarnation cannot complete old work. An idle history reset installs the new F
cursor before C encodes queued work and emits `HISTORY_RESET`, then the new C-side `TX_BEGIN`.
`HISTORY_RESET` carries the expected initial state digest, but F derives that digest from
`C_STORE_GUID` and the proposed nonce and rejects a mismatch; it never installs a caller-chosen
initial cursor. C installs no candidate reset state until the acknowledgement exactly repeats
the selected protocol, complete negotiated profile mask, both negotiated limits, F identity,
namespace/route presence, nonce, REL_SEQ, state digest, and absent last commit.

A peer `ERROR` or a definitive client-side dialogue failure returns the one bounded
`ClientRunResult::TerminalError` form and leaves its settlement unresolved. It does not erase queued or
active Protocol-50 reconciliation identity. Sending the bounded diagnostic to the peer is best
effort and cannot relabel an already-known terminal result as an uncertain disconnect. An
uncertain disconnect instead returns `Disconnected` and remains eligible for exact
same-transaction replay; it is never relabeled as
a terminal whole-attempt disposition. A later non-Protocol-50 path, when added, must therefore
start as a separate logical compile attempt rather than append bytes to this transaction.

## Canonical action trace and formal lane

`p50_actions.*` emits JSONL with an explicit C/F actor and keys checker state by
`(C_STORE_GUID, F_STORE_GUID)`. C-active and F-pending overlays remain distinct across session
replacement. F emits `INPUT_COMMITTED` at its durable transition; C later emits
`COMMIT_ACCEPTED`, or `LOST_COMMIT_ACCEPTED` after reconnect. Both the C++ checker and
`formal/check_trace.py` retain the exact original Need keys rather than only a counter, and
record each applied object's content digest so a Key64 cannot silently change content in a
replay trace.

The small TLC model has one C relationship, two F stores, two TUs, two immutable key/content
values, and two session tokens per F. It covers token fencing, stale callback rejection,
canonical immutable content, and the explicit durable-F/unacknowledged-C window. Pin/evict and
cache-versus-existing-path result arbitration are staged with the M5 eviction/legacy-retry
product actions; M0/M1 do not claim those unavailable transitions. Compression, timing,
environment, scheduler choice, and parser internals stay outside the model.

M2 adds `F_STORE_INCAR_REPLACED` to the product action vocabulary and C++ checker. The parallel
formal-correction branch owns the corresponding Python/TLA action update, so this branch does not
claim that a trace containing the new M2 action is accepted by `formal/check_trace.py` yet.

## Artificial-cluster gate plan after M2

The current artificial product harness is `tests/test.sh`: one scheduler, one submit daemon,
two worker daemons, private loopback ports, compiler-output comparison, retained process logs,
and an `OTHERVERSIONPREFIX` path for mixed installed versions. The distribution simulator under
`capability/distribution/` is a separate research harness and is not changed by M0/M1/M2.

Later product slices should enter the cluster in this order:

1. **Loopback cache pair (implemented in M2):** real asynchronous framed endpoints; fragment at
   every representative header/BODY boundary, disconnect at every dialogue message boundary,
   and compare reconstructed input bytes.
2. **C1F1:** add compiler pipes and one worker. Exercise cold/warm, lost final commit, route reset,
   F-store reset, and process replacement. Retain endpoint action traces and daemon logs.
3. **C1F2:** alternate and pin scheduling; partially transfer on F1, retry the same PreparedTU on
   F2, then return to F1. Check independent route cursors and F-authoritative Need sets.
4. **C1F20:** 200 slots per F, cold and repeated builds, pinning on and off. Retain per-route
   outgoing bytes, queue depth, reconnects, compiler results, and wall time.
5. **Mixed old/new:** use explicit protocol negotiation with `OTHERVERSIONPREFIX`; use Protocol 50
   only when both endpoints select it, otherwise keep the existing input path.

M2 is not deployed by this patch. Daemon attachment, sidecar process ownership, multi-dialogue
scheduling, compiler integration, persistence, and eviction belong to later slices.
