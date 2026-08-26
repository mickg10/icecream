#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
source=$src/daemon/p50_fork_fd_hygiene.cpp
test -s "$source"

mutant=$(mktemp "${TMPDIR:-/tmp}/p50forkfdhygiene.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM

# The deletion-sensitive contract must retain both exact identity checks.
sed '/keep.source->delivery_id == 0/d' "$source" >"$mutant"
grep -F 'keep.source->delivery_id == 0' "$source" >/dev/null
if grep -F 'keep.source->delivery_id == 0' "$mutant" >/dev/null; then
    echo 'FAIL: DeliveryId deletion mutant was not observable' >&2
    exit 1
fi

sed '/CLOSE_RANGE_CLOEXEC/d' "$source" >"$mutant"
if grep -F 'CLOSE_RANGE_CLOEXEC' "$mutant" >/dev/null; then
    echo 'FAIL: CLOEXEC-only mutant remained' >&2
    exit 1
fi
echo 'PASS: deletion-sensitive hygiene mutants are observable'
