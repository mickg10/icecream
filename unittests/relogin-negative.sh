#!/bin/sh
# Exact negative control for the successful-relogin false return.
set -eu

set +e
./relogin_mutant >relogin-mutant.log 2>&1
rc=$?
set -e
if [ "$rc" -eq 0 ] \
        || ! grep -q 'FAILED   - successful relogin keeps the daemon connection in the drain loop' \
                   relogin-mutant.log; then
    cat relogin-mutant.log >&2
    echo "successful-relogin false-return mutant did not fail at its intended assertion" >&2
    exit 1
fi
rm -f relogin-mutant.log
echo "PASS: successful-relogin false return rejected"
