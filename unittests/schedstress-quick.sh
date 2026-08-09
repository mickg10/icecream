#!/bin/sh
# Deterministic scheduler stress gates (~2min).  Split from schedbp-quick.sh
# because the promotion case runs at the PRODUCTION 60-second bound on
# purpose -- a shortened test-only bound would gate a rule production does
# not run.
#
# Run 1 -- promotion (SCH-1): warm two estimate keys (100ms and 90s), hold a
# one-slot farm busy while a cheap request ages past the bound, then submit
# a fresh request whose score is ~2x higher.  When the slot frees, numeric
# selection would pick the fresh request; only the hard promotion rule
# dispatches the overdue one first -- asserted against a continuous stream
# of younger competitors from the healthy submitter.
#
# Run 2 -- heterogeneous (SCH-3): the scored head is an x86_64 request whose
# only capable host is fully held (slot + preload window); a younger aarch64
# request must still dispatch to the second host, the head must neither
# bounce nor block, and it must dispatch once its host has capacity.
#
# Run 3 -- stallcredit (BP-1): at the --dispatch-stall-report-after floor
# (10s), a submitter whose dispatched jobs never reach JobBegin is bounded
# by its dispatch CREDIT and reported -- not removed: it drains its replies,
# so it is a responsive connection whose wrappers stalled, not a dead
# daemon.  Its healthy peer is served throughout.
#
# Run 4 -- clientstall (client isolation): a submitting daemon proxies every
# compiler wrapper on its host.  One wrapper frozen after its assignment
# holds a dispatch debit that JobBegin never credits; the scheduler used to
# answer that by removing the whole daemon, voiding every healthy sibling's
# work.  Only the stale assignment may be expired.
#
# Run 5 -- retention: the proof that Stage-A retention is correct.  A
# quiet healthy daemon crossing the report threshold is not removed, a
# long-running sibling survives it and its completion is consumed, dispatch
# continues while a wrapper is stuck, the stall is reported exactly once,
# and a LATE THAW's Begin/Done still reconcile the retained assignment
# exactly once with no accounting failure.
#
# Run 6 -- noreader: the honest stopped-daemon model.  A submitter whose
# process stops consuming its socket -- but whose single credit-1 reply fit
# in the kernel buffers, so no deferred-output episode ever arms -- is
# reported and capped, NOT removed; its assignment and worker reservation
# persist (the documented Stage-A behaviour) and everyone else progresses.
#
# Run 7 -- leastbusy (SCH-6): with -a least_busy and every host at maxJobs,
# work must still be assigned (the bucketed picker selected an empty set and
# stalled); with unequal occupancies the emptier host must win the exact
# fraction comparison; an even fill spreads evenly.
#
# Run 8 -- teardown: a started job survives its submitter's disconnect
# (detach/terminal-authority; issue #4 item 1).
#
# Run 9 -- internalsuaf: the bounded-async internals fan-out finalizes over
# a target whose CompileServer was freed mid-transaction without a
# use-after-free (issue #4 correction A.1; ASan red/green).
#
# Run 10 -- duplocal: a duplicate local-job Begin is idempotent (one
# allocation, one monitor Begin, one terminal) and a duplicate/unknown
# local Done never emits a terminal for global id 0 (issue #4 correction
# B.2a; red/green).
#
# Run 11 -- exhaust: a batch exceeding the wire-id domain fails fatally --
# generation closed, no staged survivors, unrelated peers live -- instead
# of parking and retrying forever (issue #4 correction B.2b; red/green,
# tiny ICECC_TEST_JOB_ID_DOMAIN).
dir=$(dirname "$0")
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 10 5 promotion 1 || exit 1
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 10 5 heterogeneous 1 || exit 1
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 50 5 stallcredit || exit 1
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 10 5 clientstall || exit 1
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 10 5 teardown || exit 1
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 10 5 internalsuaf || exit 1
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 10 5 duplocal || exit 1
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 10 5 exhaust || exit 1
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 10 5 retention || exit 1
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 10 5 noreader || exit 1
exec "$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 10 5 leastbusy 2
