# Protocol 50 M3 input attachment core

This slice is a pure C++ ownership and observation core. It composes the
existing `InputRecordStore`/`InputCursor` implementation and does not change
the endpoint, cache service, adopted-endpoint, FD-handoff, or compiler-input
transport seams.

## Invariants

* The retained cache identity is exactly `(C_STORE_GUID, TU_SEQ)`. `ATTEMPT_ID`
  appears only in `InputAttempt` ownership and request/reply observations.
* A first exact commit for an Open logical job creates one bounded pending
  `InputReady` event. Exact duplicate commits are idempotent. A job that was
  cancelled before its route commit uses the closed observation path and
  creates no ready event.
* A request includes `(C_STORE_GUID, TU_SEQ, logical job, ATTEMPT_ID,
  request ID)`. A reply echoes that identity and carries a stable event ID.
  Repeating an exact request returns the original reply. An ACK must echo the
  complete request and reply identity; an exact duplicate ACK is harmless.
* Replacement changes only the current attempt owner. It neither republishes
  input nor emits another ready event. Requests, ACKs, and attachments from
  the old attempt are stale and cannot affect the replacement.
* `attach()` delegates to `InputRecordStore::attach()`, yielding an independent
  byte-zero cursor. Closing/cancelling blocks new attachments but does not
  invalidate cursors already issued. Garbage collection requires both logical
  job closure and release of every cursor's shared immutable backing.
* The pending-ready table and replay observation table have explicit limits.
  Admission checks happen before publication, so table exhaustion cannot leave
  a partially retained record.

The API returns status replies for unknown and stale request observations and
throws on malformed local identities or illegal owner transitions. Networking,
compiler process lifetime, wire serialization, and scheduler policy remain
outside this module for a later integration slice.

## Focused gates

```sh
make check TESTS='p50inputattachment p50inputattachment-source.sh'
./unittests/p50_input_attachment_sanitize.sh
```

The focused test includes duplicate-ready, cross-attempt close, wrong
GUID/TU/owner, stale replay/ACK, table exhaustion, attach-after-close, cursor
lifetime/reclamation, closed-before-commit, replacement, and a perturbed reply.
