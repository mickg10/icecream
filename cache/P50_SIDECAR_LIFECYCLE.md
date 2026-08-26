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
because zombie helpers and PGID reuse make it ambiguous. The reducer never
accepts caller fields as kill authority: `KillDomainLease` is a private
capability issued by an injected `KillDomainVerifier`, whose production
default rejects every capture and absence proof. A Linux cgroup-v2 adapter may
opt in only after creating a private per-attempt leaf and proving
`cgroup.kill` plus `cgroup.events` exhaustion without reusable numeric PGID
authority.

Each attempt consumes one complete fresh `LaunchIncarnation` from the shared
`LaunchIdentityAllocator`: control attempt, F-store generation, store root,
C/F role GUIDs, and private path all advance together while the daemon control
generation remains immutable. `SidecarLifecycleConfig` deliberately has no
second caller-selected store-generation field; replacement therefore cannot
retain or forge an old F-store fence. READY uses the same canonical
`F_STORE_GENERATION` schema as the production supervisor and is published only
after the complete lease validates against this identity, and publication is rejected
after any waitable/reap, identity-loss, replacement, or teardown observation.
The lease captures the listener's device/inode; READY publication itself
performs `lstat()` and requires a current AF_UNIX node with the exact
device/inode, and teardown re-lstats the exact socket pathname. A new socket at
the same pathname therefore cannot satisfy old-incarnation absence. Teardown
clears the current lease before returning the Withdraw action,
so advertisement, C eligibility, and control operations are withdrawn before
TERM/KILL. One absolute deadline covers TERM, KILL, group absence, reap, and
path proof; residue at the deadline becomes `FailedClosed` with capacity
withheld. `request_legacy` uses the same proof-driven teardown before entering
`DegradedLegacy`; without an externally supplied non-reusable kill-domain lease
it also fails closed and never claims a replacement-safe teardown. The central
registry has a fixed total owner capacity and its RAII registration token
retires a row without allocation; the old compatibility overload that
discarded this token does not exist.

This is an integration seam, not a claim that `iceccd` has been converted to
the event-loop reducer.  Positive CACHE_SESSION/private-FD settlement,
compiler-attempt retirement/cursor, and first-fork descriptor hygiene remain
separate follow-up work.
