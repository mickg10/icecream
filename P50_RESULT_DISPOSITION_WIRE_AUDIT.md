# Protocol-50 ResultDispositionMsg wire audit

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
No client or daemon integration is part of this wire-only lane.
