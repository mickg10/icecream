# P50 client ZSTD_TU source sender

`p50_zstd_sender.*` is the client-side ownership seam for one complete
preprocessed source. It accepts either an owned regular-file descriptor or a
copied byte span, admits one `PrepareRequestKey`, uses one
`P50PreparationAuthority` and one `P50ClientEndpoint`, and permits exactly one
replay of the same prepared handle after a disconnected run. It has no
FileChunk path.

The sender requires a caller-owned nonzero `CStoreGuid` and request identity;
it does not invent product identity. A successful endpoint result must carry
the directly validated `TxCommit` and exact `(C_GUID,TU_SEQ)` key. The sender
checks their TU sequence, C namespace, and raw digest without treating the
diagnostic action trace as authority.

Production callers may supply a connected-fd factory. It is invoked once per
bounded attempt.  The production factory sends the ordinary `CACHE_SESSION`,
retains the socket, waits under the sender's unchanged absolute deadline for
the exact raw sidecar-ownership witness `50 f0 00 01`, and only then returns
the descriptor to CacheWire.  Wrong, partial, missing, late, or read-ahead
READY fails closed.  The second attempt therefore obtains a fresh ordinary
connection while replaying the same immutable prepared handle; a partially
consumed or detached descriptor is never reused.

The absolute deadline is passed unchanged into the endpoint, whose owned timer
cancels a blocked connect/read/write and returns typed `DeadlineExceeded`.
After a validated commit, that witness wins a same-boundary timer race; before
commit, timeout retains endpoint reconciliation state and the sender fails
closed. Missing witnesses, terminal errors, and exhausted retry likewise never
fall back to FileChunk inside this transaction.
