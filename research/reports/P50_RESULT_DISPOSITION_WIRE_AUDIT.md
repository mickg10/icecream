# Protocol-50 result-disposition wire contract

This is a maintained description of `services/comm` and the result/lifecycle
bridge, not a TLA+ model or evidence of a new farm run. See also
[committed-input lifecycle](../../cache/P50_PROTOCOL.md#compiler-input-and-result-lifecycle).

`ResultDispositionMsg` is an ordinary framed `services/comm` message and is
admitted only when the negotiated protocol is exactly 50.  The message value
is `0x50f00002`: the enum already uses `0x50f00000` for `CACHE_SESSION`, while
`0x50f00001` is the raw `CACHE_SESSION_READY_MAGIC` transition witness and is
not available as an ordinary message value.

The frame is the existing Icecream shape: a four-byte network-order payload
length followed by the four-byte network-order `Msg::Value`.  After the type,
the payload is exactly 23 words (92 bytes), in this order:

```
JOB_ID,
ASSIGNMENT_EPOCH_HI, ASSIGNMENT_EPOCH_LO,
ASSIGNMENT_NONCE_HI, ASSIGNMENT_NONCE_LO,
COMPILE_INPUT_PROFILE,
C_STORE_GUID (four words), TU_SEQ (two words), RAW_BYTES (two words),
RAW_DIGEST (four words), ATTEMPT_ID (two words), REQUEST_ID (two words),
DISPOSITION
```

The assignment is required to be complete and nonzero.  A terminal disposition
requires a present `CompileInputIdentity`; absent and partial selectors are
refused.  Both `ATTEMPT_ID` and `REQUEST_ID` must equal the exact assignment
nonce.  TU sequence zero, raw byte count zero, and an all-zero raw digest remain
valid payload values; the C-store GUID and nonce-bound attempt/request fields
are the required presence and assignment markers.

The decoder rejects a body whose length is not exactly the fixed shape,
unknown disposition values, and any trailing bytes.  The sender applies the
same payload checks before composing the frame, so pre-P50, P49, and any
hypothetical later protocol emit no bytes.  Duplicate frames are valid and
decode to equal messages; this stateless wire layer does not invent a sequence
number or silently deduplicate a result owner’s event.

The focused test covers exact type/length, present and absent-selector refusal,
mutations of every identity field, duplicate semantics, malformed frames,
strict protocol gates, and unchanged P43/P48 `CompileResultMsg` behavior.

## Production ownership and ordering

The submitter retains the exact `CompileJob` assignment/input identity after
`CompileResultMsg`. It attempts at most one terminal frame. `Accepted` is sent
only after every successful object/DWO stream has reached `EndMsg` and the
temporary file has been closed and renamed. OOM, caret-workaround, and
missing-file fallback send `DefinitiveCancel`; a successful output that will be
discarded is still drained so the worker can reach its output-complete barrier.
A generic post-result exception attempts `DefinitiveCancel`, while a pre-result
failure emits no disposition.

The worker waits at most 30 seconds for the first exact disposition only after
all output frames have been sent. Missing, malformed, mismatched, unexpected,
or disconnected input is `AttemptCancelOnly`; it cannot be promoted to a
logical-job terminal action. The child then sends one canonical completion
record and closes its private parent pipe. The parent accepts that record only
after a nonblocking, EOF-confirmed exact read, compares it with both the job and
the retained sidecar lease, and maps only `Accepted -> CloseAcceptedJob` or
`DefinitiveCancel -> CancelJob`. Ordinary teardown remains `CancelAttempt`.

The production source gate is deletion-sensitive for output/disposition/record
ordering, exact retained-lease comparison, both terminal mappings, the
attempt-only paths, nonblocking EOF validation, and the unchanged legacy child
pipe path.
