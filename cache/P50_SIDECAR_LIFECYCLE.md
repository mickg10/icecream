# Protocol-50 outer-loop sidecar lifecycle

`p50_sidecar_lifecycle` is the nonblocking integration seam for issue 16.
`SidecarLifecycle::begin`, `advance`, and `observe_child_reaped` are reducer
operations only: they do not fork, perform I/O, poll, sleep, wait, or signal.
The daemon outer loop owns those effects and executes at most the one returned
action per turn.

The reducer has explicit `Stopped`, `LaunchPrepared`,
`ForkedAwaitExecAndReady`, `Ready`, `TerminatingGrace`, `TerminatingKill`,
`ReapAndGroupCheck`, `RetryEligible`, and `DegradedLegacy` states.  A central
`CentralChildReaperRegistry` gives each PID exactly one owner and forwards one
reap observation.  ECHILD or leader reaping is only leader knowledge; retry and
replacement require the exact expected PGID to be proven ESRCH and the exact
private path to be absent.

Each attempt consumes a fresh `LaunchIdentityAllocator` attempt and store root,
with fresh C/F role GUIDs and a fresh private path.  The control generation is
immutable, while `store_generation` is an explicitly separate field and may
remain 1 across a fresh store namespace.  READY is published only after the
complete lease validates against this identity.  Teardown clears the current
lease before returning the Withdraw action, so advertisement, C eligibility,
and control operations are withdrawn before TERM/KILL.

This is an integration seam, not a claim that `iceccd` has been converted to
the event-loop reducer.  Positive CACHE_SESSION/private-FD settlement,
compiler-attempt retirement/cursor, and first-fork descriptor hygiene remain
separate follow-up work.
