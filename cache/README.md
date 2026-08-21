# Protocol 50 M0 wire and formal core

This directory freezes the product-facing Protocol 50 identities, framing, negotiation,
ten-message vocabulary, action traces, key-layout census, and bounded formal model. It does
not add sockets, daemon hooks, compiler pipes, an executable P29 transaction, persistence,
pinning, eviction, or result arbitration.

## Build baseline and cluster census

The repository baseline is C++23 and Boost 1.66. Protocol 50 digest code separately requires
libxxhash 0.8.0 or newer and its XXH3-128 one-shot and streaming APIs. The artificial-cluster
hosts require development packages before a later product slice is deployed: `nas642` has
Boost 1.55 headers and xxHash 0.8.1 runtime files, while `tt-quietbox2` has no Boost headers
and xxHash 0.8.1 runtime files. M0 was built against extracted Boost 1.74 and xxHash 0.8.1
development packages without changing either host installation.

## Canonical identities

- `CStoreGuid` (`C_STORE_GUID`) and `FStoreGuid` (`F_STORE_GUID`) are independent 128-bit
  store-incarnation identities. An F object namespace is selected by `C_STORE_GUID`.
- `Key64` uses `KeyLayoutV1`: 5 type bits, 10 generation bits, and 49 ordinal bits. Type zero
  and ordinal zero are invalid; generation zero is valid. Exhausting generation 1023 requires
  a new `C_STORE_GUID`.
- `TU_SEQ` is C publication order. `REL_SEQ` is allocated by one ordered relationship.
- `HISTORY_NONCE` is an unsigned 64-bit relationship value. Resetting it returns `REL_SEQ` to
  zero without changing immutable object identities.
- `ProfileId` has additive identifiers for `P29`, `ZSTD_TU`, and reserved future `GRZ`.

## Wire closure

Every frame starts with one big-endian u32. Its high 8 bits are the message type and its low
24 bits are payload length. M0 caps one payload at 1 MiB. The exact vocabulary is:

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

`ERROR` terminates the current message session. `SESSION_HELLO` carries a minimum/maximum
protocol version, an additive supported-profile mask, an offered frame cap, and an offered
FILL-record cap. `SESSION_STATE` selects one offered version/profile and caps no larger than
the offer. The public client-side state validator checks those relationships in addition to
the intrinsic wire validation.

There is no attempt identifier. One current session plus the `(HISTORY_NONCE, REL_SEQ, TU_SEQ)`
tuple identifies work. `TxBegin` is a pure value containing that tuple, selected profile,
pre-state digest, exact DICT/BODY descriptors, expected raw byte count/digest, and transaction
digest. Generic descriptor decoding carries future encoding IDs for later profile adapters.

DICT and BODY are independent append-only streams. Their byte counts and digests must close
exactly; zero-length streams are legal. Need declares the exact sorted, unique Key64 set.
Fill is a sequence of self-delimiting object records, and records may span frames.

```text
TRANSACTION_DIGEST = XXH3-128(canonical semantic TX_BEGIN without its digest
                              || exact DICT || exact BODY || expected raw length/digest)

POST_STATE_DIGEST  = XXH3-128(pre-state || HISTORY_NONCE || REL_SEQ || TU_SEQ
                              || TRANSACTION_DIGEST)
```

Product content and closure digests use `services/digest128.*`, the common C++ wrapper around
XXH3-128 one-shot and streaming APIs. The original MD5 implementation remains owned by the
existing client path.

## Action trace and bounded model

`p50_actions.*` emits JSONL with explicit C/F actors and keys checker state by
`(C_STORE_GUID, F_STORE_GUID)`. C-active and F-pending overlays are distinct. F emits
`INPUT_COMMITTED` at its durable transition; C later emits `COMMIT_ACCEPTED`, or
`LOST_COMMIT_ACCEPTED` after reconnect. The checker retains the exact original Need keys and
each applied object's content digest.

The TLC model has one C relationship, two F stores, two TUs, two immutable key/content values,
and two session tokens per F. It covers token fencing, stale callback rejection, canonical
immutable content, and the durable-F/unacknowledged-C window. Compression, timing,
environment, scheduler choice, parser internals, pinning, eviction, and result arbitration
remain outside M0.
