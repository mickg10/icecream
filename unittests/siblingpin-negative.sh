#!/bin/sh
# Exact negative control for the historical exact-worker-platform rewrite.
set -eu

set +e
./siblingpin_mutant >siblingpin-mutant.log 2>&1
rc=$?
set -e
if [ "$rc" -eq 0 ] \
        || ! grep -q 'FAILED   - compatible platform, not worker platform, pins the first sibling' \
                   siblingpin-mutant.log; then
    cat siblingpin-mutant.log >&2
    echo "exact-worker-platform mutant did not fail at its intended assertion" >&2
    exit 1
fi
rm -f siblingpin-mutant.log
echo "PASS: exact-worker-platform sibling rewrite rejected"
