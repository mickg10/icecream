# Protocol-50 reverse sealed-FD retry seam

This bounded slice is a transport/reducer primitive only.  It makes no claim
about daemon/main or `CompileFile` wiring.

`ReverseFdOwner::stage` materializes one Linux sealed memfd.  The owner keeps
the original absolute local `steady_clock` deadline and the nonzero
`DeliveryId`/token pair.  `send_attempt` computes a bounded remaining
millisecond duration at the last responsible moment, duplicates the master
with `FD_CLOEXEC`, and closes that duplicate after the attempt.  Repeated
attempts therefore never reuse an already-transferred descriptor and never
renew either identity or deadline.  A wire peer sees `remaining_ms`, never a
`steady_clock::time_point`.

`ReverseFdReceiverLedger` is service-incarnation state, not connection state.
It retains a high-water delivery id and exact accepted fingerprint.  The
first exact attempt transitions `WAITP50INPUT` to `TOCOMPILE` before an ACK
can be emitted.  An exact replay (including after a lost ACK) is ACKable but
closes its duplicate and does not increment transition/fork counters.  Lower
ids and same-id conflicts are rejected.  Cancel, expiry, close, and move
replacement release retained descriptors through RAII.

## Focused deletion/mutant matrix

Each row is a required invariant.  The behavioral mutant gate mutates the
corresponding operation and expects the focused test to fail.

| Row | Deletion/mutant | Expected red behavior |
|---:|---|---|
| 1 | remove `kMagic` check | malformed wire accepted |
| 2 | remove version check | old wire accepted |
| 3 | remove exact wire-size check | trailing bytes accepted |
| 4 | remove nonzero DeliveryId validation | zero identity accepted |
| 5 | remove nonzero token validation | zero token accepted |
| 6 | remove remaining-duration upper bound | unbounded wire accepted |
| 7 | serialize an absolute time point | cross-host codec is no longer portable |
| 8 | stage without `F_SEAL_WRITE` | mutable master accepted |
| 9 | duplicate with plain `dup` | attempt loses `FD_CLOEXEC` |
| 10 | reuse master in `send_attempt` | second attempt is not a fresh FD |
| 11 | compute retry deadline from a new caller deadline | retry renews budget |
| 12 | reset receiver high-water on reconnect | stale replay is admitted |
| 13 | accept same-id different token | conflicting duplicate is admitted |
| 14 | transition after ACK | lost-ACK replay can fork twice |
| 15 | return without closing duplicate on replay | replay leaks an FD |
| 16 | clear accepted fingerprint after ACK loss | exact replay is treated as new |
| 17 | accept lower id after high-water | stale delivery crosses fence |
| 18 | skip cancel/expiry RAII reset | retained master survives terminal state |

The test executable is intentionally standalone and sanitizer-friendly; the
module is not referenced by production daemon or compiler entry points.
