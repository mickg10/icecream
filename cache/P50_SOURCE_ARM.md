# Protocol-50 source arm / input-ready seam

This bounded S2 slice defines the two-phase identity contract needed by the
normal `CompileFile`-before-preprocessor order:

1. `P50SourceArm` is known before preprocessing. It binds the complete
   assignment claim, selected F ordinary/cache endpoint, logical job and
   attempt, C-store generation/GUID, nonzero source request ID, and source
   mode/profile.
2. F acknowledges the exact arm. Only that ACK permits the C-side CacheWire
   transfer (`P50SourceArmGate`).
3. `P50InputReady` is produced only after exact bytes are committed. It echoes
   the arm and adds TU sequence, byte count/digest, F-store generation/GUID,
   attachment request ID, and ready-event ID.
4. F enters `WAITP50INPUT` after arm admission. Only an exact ready envelope
   with the matching arm and source request ID plus a valid sealed FD makes the
   state forkable. `take_for_fork()` is one-shot; closure rejects late input
   and closes a staged FD.

The fixture codec is Protocol-50 version 50, not a new protocol version. It is
a bounded source fixture and reducer seam; legacy peers do not enter it and
the existing ordinary FileChunk fallback remains unchanged outside the armed
path.

## Deliberate boundary

This slice does not claim the production C/F adapter. A follow-on must wire the
reducer to the real `CompileFileMsg` arm ACK and endpoint-owner executor, then
deliver a sealed FD from the canonical F attachment owner. It must also
implement the current binding rulings: create a fresh private relationship on
demand after `CACHE_SESSION` decode under one absolute deadline; use
nonblocking exact-peer `connect_unix_until` with `SO_ERROR` and child-PID
credentials; and advertise only a supervised READY lease. That lease needs a
unique per-incarnation private path/dir, generation/attempt, child PID,
F-store GUID, path digest, and listener device/inode, with identity-safe
cleanup after proven process-group death. This slice intentionally stores none
of the superseded cached/static relationship shape.
