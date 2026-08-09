#!/bin/sh
# Gates for the dispatch-credit / stall-report scheduler change; see
# schedcredit.cpp.  No preload instrument: runs on every platform.
dir=$(dirname "$0")
for mode in credit report retention noreader clientstall mixedrole clamp relisten; do
    out=$("$dir/schedcredit" "$dir/../scheduler/icecc-scheduler" "$mode") || { echo "$out"; exit 1; }
    echo "$out"
    # Runner-level sentinel: a mode must print its executed-assertion count.
    # An empty branch (or a splice that shadows the real one) prints PASS
    # with no assertions -- the binary now fails that itself, and this
    # double-checks it from outside the process.
    echo "$out" | grep -q "^# $mode: [1-9][0-9]* assertions executed" || {
        echo "schedcredit-run: mode '$mode' executed no assertions" >&2
        exit 1
    }
done
# Meta-invariant: an unknown mode is itself a test -- it must exit nonzero
# and say so, never fall through to a PASS path.
if "$dir/schedcredit" "$dir/../scheduler/icecc-scheduler" no-such-mode >/dev/null 2>&1; then
    echo "schedcredit-run: unknown mode exited zero" >&2
    exit 1
fi
exit 0
