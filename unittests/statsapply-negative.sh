#!/bin/sh
# Exact negative control for the unserialized StatsMsg client-count overwrite.
set -eu

set +e
./statsapply_mutant >statsapply-mutant.log 2>&1
rc=$?
set -e
if [ "$rc" -eq 0 ] \
        || ! grep -q 'FAILED   - StatsMsg cannot overwrite the authoritative lifecycle client count' \
                   statsapply-mutant.log; then
    cat statsapply-mutant.log >&2
    echo "StatsMsg client-count mutant did not fail at its intended assertion" >&2
    exit 1
fi
rm -f statsapply-mutant.log
echo "PASS: unserialized StatsMsg client-count overwrite rejected"
