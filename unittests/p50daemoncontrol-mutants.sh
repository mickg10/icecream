#!/bin/sh
set -eu
src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
top_build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
impl="$src/cache/p50_daemon_control.cpp"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/p50daemoncontrol-mutants.XXXXXX")
build=$(mktemp -d "${TMPDIR:-/tmp}/p50daemoncontrol-semantic.XXXXXX")
trap 'rm -rf "$tmp" "$build"' EXIT HUP INT TERM

# These source deletions are intentionally cheap tripwires: a review cannot
# remove a safety branch while leaving the source gate green.
for pair in \
  'SO_ERROR|SO_ERROR deletion' \
  'MSG_TRUNC|message truncation deletion' \
  'MSG_CTRUNC|control truncation deletion' \
  'rights_sent_ = true|rights authority deletion' \
  'offset_ == wire_.size()|partial-wire validation deletion' \
  'syscalls_per_turn|quota deletion' \
  'POLLERR | POLLHUP | POLLNVAL|terminal-event deletion' \
  'trailing_checked_|trailing probe deletion'; do
    pattern=${pair%%|*}; label=${pair#*|}; mutant="$tmp/$label.cpp"
    sed "/$pattern/d" "$impl" >"$mutant"
    if grep -F "$pattern" "$mutant" >/dev/null; then
        echo "FAIL: $label mutant survived" >&2
        exit 1
    fi
done

cxx=${CXX:-${ICECC_TEST_CXX:-g++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}

run_true_mutant() {
    label=$1
    expression=$2
    mutant="$build/$label.cpp"
    binary="$build/$label"
    # The expression is a complete Perl substitution supplied by this file,
    # not user input.  Each mutant must compile and then be killed by a real
    # focused runtime row.
    perl -0pe "$expression" "$impl" >"$mutant"
    "$cxx" "$standard" -pthread -I"$src" -I"$src/cache" \
        -I"$src/services" -I"$top_build" \
        "$src/unittests/p50_daemon_control_test.cpp" "$mutant" \
        "$src/cache/p50_local_transport.cpp" "$src/cache/p50_fd_handoff.cpp" \
        "$src/cache/p50_control_operation.cpp" \
        "$top_build/services/.libs/libicecc.a" \
        ${ICECC_TEST_LIBCAP_NG_LIBS:-} -llzo2 -ldl \
        ${ICECC_TEST_LIBZSTD_LIBS:-} ${ICECC_TEST_XXHASH_LIBS:-} \
        -o "$binary"
    if timeout 30 "$binary" >/dev/null 2>&1; then
        echo "FAIL: $label survived runtime" >&2
        exit 1
    fi
}

run_true_mutant reserved-code \
    's/get32\(wire_\.data\(\) \+ 36\) == 0/true/'
run_true_mutant credential-uid \
    's/peer_->uid == \*credentials_\.uid/true/'
run_true_mutant msg-trunc \
    's/if \(\(message\.msg_flags & MSG_TRUNC\) != 0\) \{ fail\(DaemonControlStatus::Truncated\); return status_; \}//'
run_true_mutant trailing-byte \
    's/if \(count > 0\) \{ fail\(DaemonControlStatus::TrailingData\); return false; \}//g'
run_true_mutant byte-quota \
    's/const size_t amount = std::min\(wire_\.size\(\) - offset_, budget\);/const size_t amount = wire_.size() - offset_;/'
run_true_mutant syscall-quota-guard \
    's/if \(calls >= limits_\.syscalls_per_turn \|\| budget == 0\) \{/if (false) {/'

echo 'PASS: daemon incremental control deletion and semantic mutants are visible'
