#!/bin/sh
# Deletion-sensitive scope gate for the development release identity (S1b).
#
# Asserts the version triplet in configure.ac composes to exactly 1.5.90,
# with no leftover conflicting definition from the prior 1.4.92 identity,
# and -- when a built client is present -- that `icecc --version` reports
# the same identity.  A stale minor/micro pair, a duplicated definition, or
# a client built from a different tree must fail this test.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}

require_count() {
    expected=$1
    pattern=$2
    file=$3
    label=$4
    actual=$(grep -F -c "$pattern" "$src/$file" || true)
    if [ "$actual" -ne "$expected" ]; then
        echo "FAIL: $label (expected $expected anchor(s), found $actual)" >&2
        exit 1
    fi
    echo "ok - $label"
}

require_absent() {
    pattern=$1
    shift
    label=$1
    shift
    if grep -E -n "$pattern" "$@" >/dev/null 2>&1; then
        echo "FAIL: $label" >&2
        grep -E -n "$pattern" "$@" >&2 || true
        exit 1
    fi
    echo "ok - $label"
}

require_count 1 'm4_define([icecream_version_major],[1])' configure.ac \
    'configure.ac declares major version 1'
require_count 1 'm4_define([icecream_version_minor],[5])' configure.ac \
    'configure.ac declares minor version 5'
require_count 1 'm4_define([icecream_version_micro],[90])' configure.ac \
    'configure.ac declares micro version 90'

require_absent 'm4_define\(\[icecream_version_minor\],\[4\]\)' \
    'no leftover 1.4.x minor definition remains' "$src/configure.ac"
require_absent 'm4_define\(\[icecream_version_micro\],\[92\]\)' \
    'no leftover .92 micro definition remains' "$src/configure.ac"

# If a build tree is present alongside this test run, the released client
# identity string must agree with the configure.ac triplet above.
build_dir=${ICECC_TEST_BUILDDIR:-$(dirname -- "$0")}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$build_dir/..}
client_bin="$top_build_dir/client/icecc"

if [ -x "$client_bin" ]; then
    out=$("$client_bin" --version)
    case "$out" in
        *1.5.90*)
            echo "ok - built icecc --version reports 1.5.90"
            ;;
        *)
            echo "FAIL: built icecc --version does not report 1.5.90 (got: $out)" >&2
            exit 1
            ;;
    esac
else
    echo "ok - skipped built icecc --version check ($client_bin not present)"
fi

echo 'PASS: development release identity is 1.5.90'
