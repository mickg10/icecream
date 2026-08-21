# Protocol 50 M0/M1 executable boundary

This directory is the product-facing cache protocol boundary. M0 freezes identities, closure,
framing, the ten-message vocabulary, action traces, and the bounded formal model. M1 makes the
two-party transaction executable in memory with the real `p29::OnlineS1` and
`p29::BlockCatalogue`. There are no sockets, cache sidecars, daemon hooks, compiler pipes, or
on-disk storage in these milestones.

## Build baseline and cluster census

The repository baseline is C++23. Boost 1.66 is the minimum: configure exercises only the
future sidecar's required `boost::asio::io_context`, TCP socket, and `post` APIs. Boost.System
is linked only on Boost/toolchain combinations that require it; Boost 1.74 on the gate host is
header-only for this probe. There is deliberately no strand probe and no transport target in
B0/M0/M1. Protocol-50 digest code separately requires libxxhash 0.8.0 or newer and its
XXH3-128 one-shot and streaming APIs.

| artificial-cluster host | compiler | system Boost | system xxHash |
|---|---|---|---|
| `nas642` | GCC 11.4 | 1.55 headers (below baseline) | 0.8.1 runtime, development files absent |
| `tt-quietbox2` | GCC 11.4 | headers absent | 0.8.1 runtime, development files absent |

Both installations therefore need the current Boost and xxHash development packages before a
later product slice is deployed. M0/M1 were built against extracted Boost 1.74/xxHash 0.8.1
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
`SESSION_STATE` returns the selected version/profile and the smaller negotiated caps. V1
rejects a zero or greater-than-1-MiB frame cap and a FILL-record cap smaller than its fixed
record prefix.
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

## Four reconnect outcomes

| Resume observation | C action | F object result |
|---|---|---|
| exact F GUID, nonce, REL_SEQ, and state digest | replay the same pure `TxBegin` and streams | complete objects survive; F recomputes Need |
| F exactly one commit ahead with the retained complete `TxCommit` | accept the retained tuple once | objects and F cursor stay committed |
| changed F GUID or absent C namespace | abort pending OnlineS1, start a new nonce/REL_SEQ 0, re-encode the retained PreparedTU with P29's history-independent Root mode | repopulate from F-authoritative Need |
| same object store but missing/different route cursor | abort pending OnlineS1, reset route outside ACTIVE_TX, re-encode retained PreparedTU with P29's history-independent Root mode | retain all immutable objects |

A new session replaces the old session for that C namespace and clears only F's incomplete
transaction overlay. Session serial allocation never wraps: after allocating `UINT64_MAX`, all
new sessions fail until a new F-store incarnation resets the counter.
Every in-memory session handle also carries its `F_STORE_GUID`, so a coincident serial from a
different F or from a reset incarnation cannot complete old work. On a history reset, F installs
the new cursor before C re-encodes a retained `PreparedTU`; the observable action order is
`TX_ABORTED`, `HISTORY_RESET`, then the new C-side `TX_BEGIN`.
`HISTORY_RESET` carries the expected initial state digest, but F derives that digest from
`C_STORE_GUID` and the proposed nonce and rejects a mismatch; it never installs a caller-chosen
initial cursor.

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

## Artificial-cluster gate plan after M1

The current artificial product harness is `tests/test.sh`: one scheduler, one submit daemon,
two worker daemons, private loopback ports, compiler-output comparison, retained process logs,
and an `OTHERVERSIONPREFIX` path for mixed installed versions. The distribution simulator under
`capability/distribution/` is a separate research harness and is not changed by M0/M1.

Later product slices should enter the cluster in this order:

1. **Loopback cache pair:** framed endpoints only; replay every M1 stream/object boundary and
   compare reconstructed input bytes.
2. **C1F1:** add compiler pipes and one worker. Exercise cold/warm, lost final commit, route reset,
   F-store reset, and process replacement. Retain endpoint action traces and daemon logs.
3. **C1F2:** alternate and pin scheduling; partially transfer on F1, retry the same PreparedTU on
   F2, then return to F1. Check independent route cursors and F-authoritative Need sets.
4. **C1F20:** 200 slots per F, cold and repeated builds, pinning on and off. Retain per-route
   outgoing bytes, queue depth, reconnects, compiler results, and wall time.
5. **Mixed old/new:** use explicit protocol negotiation with `OTHERVERSIONPREFIX`; use Protocol 50
   only when both endpoints select it, otherwise keep the existing input path.

M1 is not deployed by this patch. Remote sidecar, socket ownership, asynchronous execution,
bounded worker pools, compiler integration, persistence, and eviction belong to later slices.
