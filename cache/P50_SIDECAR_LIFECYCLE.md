# Protocol-50 outer-loop sidecar lifecycle

`p50_sidecar_lifecycle` is the nonblocking integration seam for issue 16.
`SidecarLifecycle::begin`, `advance`, and `observe_child_reaped` are reducer
operations only: they do not fork, perform I/O, poll, sleep, wait, or signal.
The daemon outer loop owns those effects and executes at most the one returned
action per turn. Reap delivery crosses the registry/lifecycle boundary as a
bounded value event carrying a stable `(owner_id, generation, PID)` key; the
registry never stores a lifecycle pointer.

The reducer has explicit `Stopped`, `LaunchPrepared`,
`ForkedAwaitExecAndReady`, `Ready`, `TerminatingGrace`, `TerminatingKill`,
`ReapAndGroupCheck`, `RetryEligible`, `DegradedLegacy`, and terminal
`FailedClosed` states. A central `CentralChildReaperRegistry` gives each PID
exactly one owner and forwards one reap observation. ECHILD or leader reaping
is only leader knowledge; retry and replacement require the exact expected
PGID to be proven ESRCH, bound to a non-reusable external `KillDomainLease`,
and the exact private path to be absent. Numeric PGID/ESRCH alone is rejected
because zombie helpers and PGID reuse make it ambiguous.

Each attempt consumes a fresh `LaunchIdentityAllocator` attempt and store root,
with fresh C/F role GUIDs and a fresh private path.  The control generation is
immutable, while `store_generation` is an explicitly separate field and may
remain 1 across a fresh store namespace. READY is published only after the
complete lease validates against this identity, and publication is rejected
after any waitable/reap, identity-loss, replacement, or teardown observation.
The lease captures the listener's device/inode; teardown compares that exact
old node, so a new socket at the same pathname cannot satisfy old-incarnation
absence. Teardown clears the current lease before returning the Withdraw action,
so advertisement, C eligibility, and control operations are withdrawn before
TERM/KILL. One absolute deadline covers TERM, KILL, group absence, reap, and
path proof; residue at the deadline becomes `FailedClosed` with capacity
withheld. `request_legacy` uses the same proof-driven teardown before entering
`DegradedLegacy`; without an externally supplied non-reusable kill-domain lease
it also fails closed and never claims a replacement-safe teardown.

This is an integration seam, not a claim that `iceccd` has been converted to
the event-loop reducer.  Positive CACHE_SESSION/private-FD settlement,
compiler-attempt retirement/cursor, and first-fork descriptor hygiene remain
separate follow-up work.
