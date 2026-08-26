# Protocol-50 C source ingress/finalization successor

This is an event-loop-owned reducer seam, not a production CompileFile bridge.
The C owner allocates one exact full source arm, monotonic lease serial, child
PID, and bounded nonrenewable deadline. F admission consumes the complete
`P50_SOURCE_ARMED` message, including F control generation/attempt, F
StoreIdentity generation/GUID/version, observation, and bounded source budget.
The wire validator rejects role-tagged C/F GUIDs sharing the same 127-bit root.

Wrapper EOF and successful exact-child `waitpid` are independent latches. A
move-only `SourceFinalize` capability is revocable by cancellation, expiry,
settlement, cleanup, or replacement. CacheWire starts only after F ACK and
finalization, binds an exact `InputRecordKey`, cache-session attempt,
operation ID, raw bytes, and digest, and emits only BEGIN/BODY/COMMIT.

Settlement is not an enum acknowledgement. The endpoint must supply a lossless
witness containing the exact live lease, operation/session binding, raw bytes
and digest, and either the canonical `committed_input` key or an explicit
proved-pre-durable disposition. Late settlement is allowed only for that exact
live witness; unknown, stale, copied, or future values fail closed.

Production CompileFile/private-transport/C-F sender and real SOURCE_FINALIZE/
CacheWire callsites remain HOLD. The `CacheWireStart` and settlement witness
types expose all canonical material needed for a later root integration.
