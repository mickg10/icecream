#!/bin/sh
# Exact negative control for the historical legacy-name off-by-one calculation.
set -eu

set +e
./legacyannouncement_mutant >legacyannouncement-mutant.log 2>&1
rc=$?
set -e
if [ "$rc" -eq 0 ] \
        || ! grep -q 'FAILED   - ordinary legacy announcement preserves the complete netname' \
                   legacyannouncement-mutant.log; then
    cat legacyannouncement-mutant.log >&2
    echo "legacy announcement off-by-one mutant did not fail at its intended assertion" >&2
    exit 1
fi
rm -f legacyannouncement-mutant.log
echo "PASS: legacy announcement off-by-one rejected"
