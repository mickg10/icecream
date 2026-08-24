# Protocol-50 committed-input lifecycle

`p50_input_lifecycle.{h,cpp}` is the F-side ownership reducer between a
committed cache input and compiler-attempt teardown. It is serialized on the
same `SidecarRuntime` executor as `P50ServerEndpoint`; it is not a second cache
store and never changes cache identity.

## Identities and actions

The cache key remains exactly `(C_STORE_GUID, TU_SEQ)`. Compiler ownership is
the independent tuple `(logical_job, assignment_epoch, assignment_nonce)`, and
the private transport is fenced by the current sidecar `(generation, attempt)`.
The local operation ID is scoped to that sidecar incarnation and exists only
for exact lost-ACK replay.

`CancelAttempt` revokes the current compiler attempt but keeps the logical-job
input lease open for one fresh assignment owner. `CloseAcceptedJob` and
`CancelJob` are terminal: they block new attachment, preserve a descriptor that
was already authorized, and collect the retained record after the last
endpoint cursor is released. A stale owner can mutate neither a replacement
attempt nor a replacement sidecar.

The reducer uses the endpoint's single `max_retained_input_records` bound and
reserves lifecycle capacity in the endpoint's
`input_job_state` callback before publication. A terminal command that precedes
route completion leaves a bounded tombstone; the late exact route commit is
validated through `observe_closed_job_commit`, exposes no attachable record,
and retires the tombstone. A failed pre-publication transaction releases its
reservation.

## Private control wire

The existing 32-byte CacheSession envelope remains byte-for-byte version 1.
The former 56-byte version-1 InputFdAttachment shape is still decoded only so a
mixed-version peer fails explicitly: it has no owner and therefore cannot
authorize a compiler. New owner-bearing InputFdAttachment and InputLifecycle
frames are exact 88-byte version-2 envelopes:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 2 | local operation version (`2`) |
| 2 | 2 | operation kind |
| 4 | 4 | exact size (`88`) |
| 8 | 8 | sidecar generation |
| 16 | 8 | sidecar attempt |
| 24 | 8 | request/operation ID |
| 32 | 16 | `C_STORE_GUID` |
| 48 | 8 | `TU_SEQ` |
| 56 | 8 | logical job |
| 64 | 8 | assignment epoch |
| 72 | 8 | assignment nonce |
| 80 | 2 | lifecycle action (`0` for attachment) |
| 82 | 2 | result+1 in a lifecycle reply; zero in requests |
| 84 | 4 | reserved zero |

The sidecar rejects a lifecycle request containing a result or trailing queued
data before mutation. Every syntactically valid lifecycle result is returned in
an identity-complete fixed reply. The client consumes that reply and sends one
empty, identity-bound local `Goodbye` ACK before the sidecar closes the stream;
this prevents a complete reply from racing a terminal `POLLHUP`. A lost final
ACK does not undo the reducer result. Exact replay of an applied operation yields
`AlreadyApplied`; reuse of the operation ID with changed identity/action yields
`ConflictingReplay`.

The daemon adapter retains transport failures in a bounded retry queue. If a
command cannot be retained (capacity/allocation failure), or an authenticated
reply violates the lifecycle protocol, it synchronously withdraws and destroys
that exact sidecar/store incarnation. Store destruction is the fail-closed
reclaim witness; an unretained terminal command can therefore never leave its
record attachable, and pending commands are never rebound to a replacement.
The withdrawal edge is retained until the daemon consumes it; a same-poll
recovery therefore publishes ordered `absent -> replacement-present` snapshots
instead of silently projecting a new store under the old advertisement.

## Three-bucket audit

- **Bound:** local incarnation, full cache key, full assignment owner,
  operation ID, action, and result are encoded and byte-exactly echoed. The
  owner table, retired-owner history, replay table, and daemon retry/completion
  tables are bounded.
- **Wire-placeholder/derived:** local operation IDs and sidecar process
  identity are daemon-derived observations and are not cache identity. The
  terminal action is supplied by the result-disposition owner; it is never
  inferred from generic attempt teardown.
- **Model-unrepresented:** this is private AF_UNIX control, not an ordinary
  Icecream wire-message change. The separate P50-only ordinary-link
  result-disposition message and child/parent completion record now supply the
  terminal action; they do not add a dimension to cache identity or to this
  private control wire.

## Result-to-lifecycle bridge

The worker's pre-P50 status pipe remains exactly eight native `uint32_t` words
(32 bytes), written and closed before output transfer. A P50 compiler child
instead sends `CompileResultMsg`, completes every object/DWO output stream,
waits under a 30-second bound for one exact submitter disposition, then writes
one 144-byte canonical big-endian `P50CompletionRecord` and immediately closes
the pipe. Worker exceptions write an identity-bound `AttemptCancelOnly` record
when possible. The canonical record binds statistics/exit status, result state,
disposition, job, assignment epoch/nonce, and the full compiler-input identity;
attempt and request IDs must both equal the assignment nonce.

The daemon parent reads this pipe through a retained nonblocking reader. A
short record, trailing byte, malformed state, identity mismatch, or disconnect
is attempt-only. An exact record is additionally compared with the retained
`InputFdRequest`, including its key, logical owner, and request ID. Only then
does `Accepted` drive `CloseAcceptedJob` or `DefinitiveCancel` drive `CancelJob`.
The following `handle_end()` call cannot double-settle because terminal
settlement consumes the active lease; all nonterminal paths retain the existing
`CancelAttempt` behavior.
