#!/bin/sh
set -eu
src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
impl="$src/cache/p50_daemon_control.cpp"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/p50daemoncontrol-mutants.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
for pair in \
  'SO_ERROR|SO_ERROR deletion' \
  'MSG_CTRUNC|control truncation deletion' \
  'rights_sent_ = true|rights authority deletion' \
  'offset_ == wire_.size()|partial-wire validation deletion' \
  'syscalls_per_turn|quota deletion' \
  'POLLERR | POLLHUP | POLLNVAL|terminal-event deletion'; do
    pattern=${pair%%|*}; label=${pair#*|}; mutant="$tmp/$label.cpp"
    sed "/$pattern/d" "$impl" >"$mutant"
    if grep -F "$pattern" "$mutant" >/dev/null; then
        echo "FAIL: $label mutant survived" >&2
        exit 1
    fi
done
build=$(mktemp -d "${TMPDIR:-/tmp}/p50daemoncontrol-semantic.XXXXXX")
trap 'rm -rf "$tmp" "$build"' EXIT HUP INT TERM
cxx=${CXX:-${ICECC_TEST_CXX:-g++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
for pair in \
  'if (fd_count_ > 1)|extra-fd runtime mutant' \
  'if (attach_rights) rights_sent_ = true;|rights-sent runtime mutant'; do
    pattern=${pair%%|*}; label=${pair#*|}; mutant="$build/$label.cpp"
    binary="$build/$label"
    sed "/$pattern/d" "$impl" >"$mutant"
    "$cxx" "$standard" -pthread -I"$src" -I"$src/cache" \
        "$src/unittests/p50_daemon_control_test.cpp" "$mutant" \
        "$src/cache/p50_local_transport.cpp" -o "$binary"
    if "$binary" >/dev/null 2>&1; then
        echo "FAIL: $label survived runtime" >&2
        exit 1
    fi
done
echo 'PASS: daemon incremental control deletion mutants are visible'
