# P50 client ZSTD_TU source sender

`p50_zstd_sender.*` is the client-side ownership seam for one complete
preprocessed source. It accepts either an owned regular-file descriptor or a
copied byte span, admits one `PrepareRequestKey`, uses one
`P50PreparationAuthority` and one `P50ClientEndpoint`, and permits exactly one
replay of the same prepared handle after a disconnected run. It has no
FileChunk path.

The sender requires a caller-owned nonzero `CStoreGuid` and request identity;
it does not invent product identity. A successful endpoint action trace is
checked for exactly one C-side `COMMIT_ACCEPTED` or `LOST_COMMIT_ACCEPTED` with
the expected raw digest, yielding the exact `(C_GUID,TU_SEQ)` key.

## Bounded HOLD

The current public endpoint API has no absolute-deadline cancellation or
completion callback that returns the committed `TxCommit`. The sender enforces
an absolute deadline at source admission, before each attempt, and after each
endpoint run; it cannot interrupt an endpoint coroutine already blocked in
Asio. The exact upstream API needed to close this HOLD is:

1. `P50ClientEndpoint::run(..., absolute_deadline)` must cancel its owned
   socket and complete with a typed deadline result; and
2. `ClientRunResult` (or a new public accessor) must carry the validated
   `TxCommit` identity for the committed attempt.

Until that cache API change is reviewed, the sender fails closed on a stale
deadline, missing commit witness, terminal result, or retry exhaustion and
does not claim full production deadline compliance.
