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
# Run 3 -- stallevict (BP-1): at the --dispatch-stall-timeout floor (10s), a
# submitter whose dispatched jobs never reach JobBegin is evicted AT the
# bound -- not before it, and not never -- while the healthy submitter is
# served straight through the eviction.
dir=$(dirname "$0")
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 10 5 promotion 1 || exit 1
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 10 5 heterogeneous 1 || exit 1
exec "$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 50 5 stallevict
