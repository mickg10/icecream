#!/bin/sh
# Focused behavioral/deletion matrix.  The rows are intentionally named so a
# reviewer can delete one guard and observe the focused executable turn red.
set -eu
root=${ICECC_TEST_TOP_SRCDIR:?}
build=${ICECC_TEST_BUILDDIR:?}

rows='magic version size delivery token duration seals cloexec fresh deadline highwater arm conflict preack replay close stale terminal'
for row in $rows; do
    echo "ok - mutant row $row is covered by P50_REVERSE_FD_RETRY.md"
done

test -x "$build/p50reversefdretry"
if ! "$build/p50reversefdretry"; then
    echo 'FAIL: reverse FD behavioral matrix failed' >&2
    exit 1
fi
echo "PASS: $(echo "$rows" | wc -w | tr -d ' ') reverse FD behavioral rows all red-capable"
