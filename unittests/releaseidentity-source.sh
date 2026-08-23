#!/bin/sh
# Deletion-sensitive scope gate for the development release identity (S1b).
# Source-only -- see releaseidentity-artifact.sh for the built icecc
# --version identity gate, kept as a separate test per the
# bigoracle/local-oracle HOLD on 97ef314f (both findings below).
#
# 97ef314f's original test anchored on the exact target strings
# (m4_define([icecream_version_minor],[5]) etc.) and additionally forbade
# only the prior 1.4.92 values. Both oracles found the same gap: a LATER
# conflicting m4_define for the same macro (e.g. appending
# m4_define([icecream_version_minor],[6]) after the real one) still leaves
# the original anchor's count at 1 and isn't one of the two forbidden old
# values, so the test would false-PASS even though m4's own
# last-definition-wins semantics makes the effective version 1.6.90.
#
# The fix is generic in two steps: (1) count how many m4_define calls exist
# for each macro NAME at all, independent of value -- exactly one, or fail;
# (2) only then extract that one definition's numeric value, with a strict
# 'm4_define([name],[N])' shape check, and require the composed
# major.minor.micro to equal exactly 1.5.90. A second active definition of
# any value fails step 1 before step 2 ever runs.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}

count_definition() {
    # How many m4_define calls exist for this exact macro name, regardless
    # of the value each one assigns. Anchored so the m4_ifnblank branch a
    # few lines below (which mentions these names only as text inside
    # m4_define([icecream_version],[...]), not as a definition of them)
    # cannot match: that line does not start with "m4_define(" after
    # whitespace, it starts with "[m4_define(...".
    name=$1
    grep -c -E "^[[:space:]]*m4_define[[:space:]]*\([[:space:]]*\[${name}\][[:space:]]*," \
        "$src/configure.ac" || true
}

extract_value() {
    # The single definition's numeric value, only if the whole line
    # (whitespace stripped) has the exact m4_define([name],[N]) shape.
    # Prints nothing and exits 1 if that shape does not hold, so a
    # malformed or non-numeric definition cannot silently compare equal to
    # anything downstream.
    name=$1
    awk -v name="$name" '
        {
            line = $0
            gsub(/[ \t]/, "", line)
            if (line ~ ("^m4_define\\(\\[" name "\\],\\[[0-9]+\\]\\)$")) {
                value = line
                sub(/^[^,]*,\[/, "", value)
                sub(/\]\)$/, "", value)
                print value
                found = 1
            }
        }
        END { exit(found ? 0 : 1) }
    ' "$src/configure.ac"
}

require_one_definition() {
    name=$1
    label=$2
    actual=$(count_definition "$name")
    if [ "$actual" -ne 1 ]; then
        echo "FAIL: $label (expected exactly 1 active m4_define for $name, found $actual)" >&2
        exit 1
    fi
    echo "ok - $label"
}

require_one_definition icecream_version_major \
    'configure.ac has exactly one active major definition'
require_one_definition icecream_version_minor \
    'configure.ac has exactly one active minor definition'
require_one_definition icecream_version_micro \
    'configure.ac has exactly one active micro definition'

major=$(extract_value icecream_version_major) || {
    echo "FAIL: major definition is not the exact m4_define([name],[N]) shape" >&2
    exit 1
}
minor=$(extract_value icecream_version_minor) || {
    echo "FAIL: minor definition is not the exact m4_define([name],[N]) shape" >&2
    exit 1
}
micro=$(extract_value icecream_version_micro) || {
    echo "FAIL: micro definition is not the exact m4_define([name],[N]) shape" >&2
    exit 1
}
echo "ok - extracted major.minor.micro = $major.$minor.$micro"

composed="$major.$minor.$micro"
if [ "$composed" != "1.5.90" ]; then
    echo "FAIL: configure.ac composes to $composed, expected exactly 1.5.90" >&2
    exit 1
fi
echo "ok - configure.ac composes to exactly 1.5.90"

echo 'PASS: development release identity is 1.5.90 (source only)'
