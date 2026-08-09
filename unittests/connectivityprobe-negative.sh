#!/bin/sh
# Exact negative controls for the inbound-connectivity state machine.
# The test passes only when the production build is sanitizer-clean and both
# historical mutations fail for their intended transition.
set -eu

srcdir_abs=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$srcdir_abs/.." && pwd)
cxx=${CXX:-c++}
tmp=${TMPDIR:-/tmp}/icecc-connectivityprobe-negative-$$
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

common="-std=c++11 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -I$root"

# Sanitizers are a required part of this negative control, but unsupported
# toolchains skip rather than silently weaken the gate.
printf 'int main(){return 0;}\n' > "$tmp/probe.cpp"
if ! $cxx $common "$tmp/probe.cpp" -o "$tmp/compiler-probe" >/dev/null 2>&1; then
    echo "SKIP: C++ address/undefined sanitizers unavailable" >&2
    exit 77
fi

build()
{
    name=$1
    shift
    # shellcheck disable=SC2086
    $cxx $common "$@" \
        "$srcdir_abs/connectivityprobetest.cpp" \
        "$root/scheduler/connectivityprobe.cpp" \
        -o "$tmp/$name"
}

build green
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
    "$tmp/green" >"$tmp/green.log" 2>&1

build immediate -DICECC_TEST_CONNPROBE_MUTANT_IMMEDIATE_FAILURE
set +e
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
    "$tmp/immediate" >"$tmp/immediate.log" 2>&1
immediate_rc=$?
set -e
if [ "$immediate_rc" -eq 0 ] \
        || ! grep -q 'FAILED   - connect(2) immediate success is classified as success' \
                    "$tmp/immediate.log"; then
    cat "$tmp/immediate.log" >&2
    echo "immediate-success mutant did not fail at its intended assertion" >&2
    exit 1
fi

build bytecount -DICECC_TEST_CONNPROBE_MUTANT_BYTECOUNT
set +e
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
    "$tmp/bytecount" >"$tmp/bytecount.log" 2>&1
bytecount_rc=$?
set -e
if [ "$bytecount_rc" -eq 0 ] \
        || ! grep -Eq "runtime error: index [0-9]+ out of bounds|AddressSanitizer:.*buffer-overflow" \
                    "$tmp/bytecount.log"; then
    cat "$tmp/bytecount.log" >&2
    echo "byte-count mutant did not fail at the retry-table boundary" >&2
    exit 1
fi

echo "PASS: normal probe sanitizer-clean; immediate-success and byte-count mutants rejected"
