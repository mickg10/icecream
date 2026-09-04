# Protocol 50 CacheWire revision 1

This directory contains the production cache transport carried by ordinary
Icecream protocol 50. CacheWire has an independent revision number; the only
implemented wire revision is `kP50WireRevision == 1`.

## Product profiles

Revision 1 has exactly three profile IDs and advertisement bits:

| ID | Bit | Environment name | Behavior |
|---:|---:|---|---|
| 1 | 0 | `P29V1` | provider-owned content-addressed dialogue with cross-TU reuse |
| 2 | 1 | `ZSTD_TU` | one independent zstd frame per TU |
| 3 | 2 | `ZSTD_ROUTE` | zstd route-history dialogue |

The default selection order is P29V1, ZSTD_TU, then ZSTD_ROUTE. An explicit
`ICECC_P50_PROFILE` request is exact and never silently substitutes another
profile. `OFF` suppresses a cache assignment. P29 v0, GRZ, and
Z3_SHARED_LONG are not revision-1 product profiles.

`ProfileId`, `profile_bit`, and the known/declared/operational masks in
`protocol50.h` are the cache-side registry. The matching ordinary-link
advertisement constants and source-mode values are in `services/comm.h`.

## Wire surface

Every frame starts with a four-byte header: one message-type byte followed by
a 24-bit big-endian payload length. The negotiated payload cap is bounded and
must carry the 116-byte fixed `TX_BEGIN` payload.

Revision 1 has nine messages:

1. `SESSION_HELLO`
2. `SESSION_STATE`
3. `HISTORY_RESET`
4. `ERROR`
5. `TX_BEGIN`
6. `BODY`
7. `NEED`
8. `FILL`
9. `TX_COMMIT`

There is no outer DICT component, root-mode field, or NEED flags word.
`TX_BEGIN` describes one BODY, and the BODY descriptor encoding is the exact
numeric profile ID. The transaction digest binds the profile, BODY
descriptor, expected raw identity, and route pre-state. `ERROR` terminates the
message session.

`SESSION_HELLO` and `SESSION_STATE` carry the C and F system-source
fingerprints. P29V1 system-source reuse is enabled only when both are equal
and nonzero. That decision is fixed when the route is established and must
not change within the route.

The normal dialogue is:

```text
C                                                        F
SESSION_HELLO ------------------------------------------->
              <----------------------------- SESSION_STATE
[cold or authorized idle reset]
HISTORY_RESET ------------------------------------------>
              <----------------------------- SESSION_STATE

TX_BEGIN ----------------------------------------------->
BODY --------------------------------------------------->
[P29V1 only] <------------------------------------- NEED
[P29V1 only] FILL -------------------------------------->
              <------------------------------- TX_COMMIT
```

BODY, NEED, and FILL may span frames. P29V1 puts its bounded, already-framed
inner protocol into those outer streams; no `Key64` is exposed in that inner
wire. ZSTD_TU and ZSTD_ROUTE close after their exact BODY.

## Identity and ownership

The durable store identities are `CStoreGuid` and `FStoreGuid`. A route uses a
monotonic `HISTORY_NONCE` and `REL_SEQ`; a TU has a C-wide monotonic `TU_SEQ`.
The complete transaction identity includes those values and its transaction
digest. Session serials and history nonces never wrap or reuse within an
incarnation.

The C preparation authority owns immutable prepared input and one route per
selected F/profile identity. The F endpoint owns namespaces, pending profile
dialogues, resource reservations, materialized input records, and the durable
commit witness. Compiler attempts consume independent input-record cursors;
attempt identity is not part of cache transaction identity.

An uncertain disconnect clears only the connection-local F overlay. It does
not erase C's active transaction or an F-durable/C-unacknowledged commit.
Reconnect either resumes the exact route, accepts an exact retained commit,
starts an authorized cold route for a changed F-store incarnation, performs
one idle reset, or fails closed.

## Ordinary-link source arm

The scheduler advertisement and assignment tail carry a CacheWire revision
and one selected profile bit. The source arm additionally carries a source
mode whose numeric value is the profile ID. `P50SourceArm::valid()` requires:

- CacheWire revision 1;
- one known profile bit;
- exact profile-bit/source-mode correspondence; and
- complete assignment, endpoint, store, job, attempt, and request identity.

The fixture framing used by `p50_source_identity.*` still uses the ordinary
Protocol-50 number in its own header; that is independent of the embedded
CacheWire revision field.

## Profile boundary

`ProfileDialogue` is the type-erased boundary between the common endpoint
state machine and P29V1, ZSTD_TU, or ZSTD_ROUTE transaction state. It forwards
`TX_BEGIN`, BODY/NEED/FILL handling, materialization, commit visibility,
tentative discard, reset, disconnect, and bounded resource observations.
Unsupported or unnegotiated profiles fail closed at construction.

## Action trace and formal model

`p50_actions.*` emits role-labelled JSONL. The cache transaction actions are:

```text
SESSION_OPENED SESSION_REPLACED SESSION_DISCONNECTED HISTORY_RESET
TX_BEGIN TX_ABORTED BODY_COMPLETE NEED_RECORDED OBJECT_APPLIED
INPUT_MATERIALIZED INPUT_COMMITTED COMMIT_ACCEPTED ACTIVE_REPLAYED
LOST_COMMIT_ACCEPTED F_STORE_INCAR_REPLACED
```

BODY closure precedes NEED. `formal/check_trace.py` validates exact action
ordering, immutable object content, current-session/current-operation fences,
route cursors, and durable-commit reconciliation. `formal/Protocol50.tla`
models the same BODY-to-NEED transaction boundary; the companion models split
job lifecycle, reconnect, multi-route, assignment, and incarnation ownership
to keep each state space bounded.

## Verification

Focused product gates live in `unittests/`, including `p50wire`, `p50slice0`,
`p50zstd`, `p50endpoint`, `p50profile`, `p50sourcearm`, and
`p50_trace_check.sh`. `cache/sim/p50sim_batch_test.sh` exercises all three
revision-1 profiles. The real compile gate is
`unittests/p50compilee2e-run.sh`.

Historical codec and research byte formats are documented separately in
`codec/FORMAT.md`; their presence does not add them to the revision-1 product
registry.
