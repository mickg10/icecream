#!/bin/sh
set -eu
src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
impl="$src/unittests/support/p50_source_ingress.cpp"
test="$src/unittests/p50_source_ingress_test.cpp"
comm="$src/services/comm.cpp"
comm_header="$src/services/comm.h"
identity="$src/services/p50_store_identity_wire.h"
libtool="$build/libtool"
for file in "$impl" "$test" "$comm" "$comm_header" "$identity" "$libtool" "$build/cache/libprotocol50.a" "$build/services/libicecc.la"; do test -f "$file"; done
grep -F 'P50SourceArmedMsg' "$impl" "$comm" >/dev/null
grep -F 'source_budget_msec' "$impl" "$comm" >/dev/null
grep -F 'f_store_generation' "$impl" "$comm" >/dev/null
grep -F 'P50SourceArmedFields' "$comm_header" "$impl" >/dev/null
grep -F 'store_identity_guid_valid_for_role' "$identity" "$comm_header" >/dev/null
grep -F 'store_identity_file_guid_matches_client' "$identity" "$comm_header" >/dev/null
grep -F 'reported_pid != lease_.cpp_pid' "$impl" >/dev/null
grep -F 'CacheWireState::CommitSent' "$impl" >/dev/null
grep -F 'witness_matches' "$impl" >/dev/null
grep -F 'FinalizeControl' "$impl" "$src/unittests/support/p50_source_ingress.h" >/dev/null

cxx=${CXX:-g++}
flags="-std=c++23 -Wall -Wextra ${ICECC_TEST_CPPFLAGS:-} -I$src -I$src/cache -I$src/services -I$build"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/p50sourceingress-successor.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
compile() { "$libtool" --mode=link "$cxx" $flags "$1" "$test" "$build/cache/libprotocol50.a" "$build/services/libicecc.la" -pthread -o "$2" >/dev/null; }
run_reject() { compile "$1" "$2"; if "$2" >/dev/null 2>&1; then echo "FAIL: mutant accepted: $3" >&2; exit 1; fi; }

sed 's/return owner == lease_;/(void)owner; return true;/' "$impl" >"$tmp/serial.cpp"
run_reject "$tmp/serial.cpp" "$tmp/serial" serial-owner-fence
sed 's/if (reported_pid != lease_.cpp_pid) return SourceIngressResult::WrongPid;/if (false) return SourceIngressResult::WrongPid;/' "$impl" >"$tmp/pid.cpp"
run_reject "$tmp/pid.cpp" "$tmp/pid" child-pid-fence
sed -e 's/if (!ack.valid_payload()) return SourceIngressResult::InvalidAck;/if (false) return SourceIngressResult::InvalidAck;/' \
    -e 's/if (!lease_.f_ack_valid()) {/if (false) {/' \
    "$impl" >"$tmp/ack.cpp"
run_reject "$tmp/ack.cpp" "$tmp/ack" full-ack-validation
sed 's/if (now < deadline_) return SourceIngressResult::NotReady;/if (false) return SourceIngressResult::NotReady;/' "$impl" >"$tmp/deadline.cpp"
run_reject "$tmp/deadline.cpp" "$tmp/deadline" deadline-sweep
sed 's/!prepared_ || finalize_taken_/finalize_taken_/' "$impl" >"$tmp/eof.cpp"
run_reject "$tmp/eof.cpp" "$tmp/eof" eof-latch
sed -e 's/    revoke_finalize();$/    (void)0;/' \
    -e 's/if (!wire_started_) revoke_finalize();/if (false) (void)0;/' \
    "$impl" >"$tmp/revoke.cpp"
run_reject "$tmp/revoke.cpp" "$tmp/revoke" finalize-revocation
sed 's/if (!finalized_ || !witness_matches(witness))/if (false)/' "$impl" >"$tmp/settle.cpp"
run_reject "$tmp/settle.cpp" "$tmp/settle" settlement-identity
sed 's/clear_prepared(); reconcile(); return SourceIngressResult::ReconcileRequired;/clear_prepared(); return SourceIngressResult::ReconcileRequired;/' "$impl" >"$tmp/reconcile.cpp"
run_reject "$tmp/reconcile.cpp" "$tmp/reconcile" reconcile-boundary
sed 's/if (!active_->cleanup_ready()) return SourceIngressResult::NotReady;/if (false) return SourceIngressResult::NotReady;/' "$impl" >"$tmp/cleanup.cpp"
run_reject "$tmp/cleanup.cpp" "$tmp/cleanup" cleanup-boundary
sed 's/wire_state_ = CacheWireState::CommitSent;/wire_state_ = CacheWireState::Body;/' "$impl" >"$tmp/commit.cpp"
run_reject "$tmp/commit.cpp" "$tmp/commit" cachewire-commit-boundary
"$src/unittests/p50storeidentitywire-mutants.sh" >/dev/null
echo 'ok - source ingress successor semantic deletion mutants'
