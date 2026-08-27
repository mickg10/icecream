#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
cxx=${ICECC_TEST_CXX:-c++}
mutant_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50-adopted-writer-source.XXXXXX")
trap 'rm -rf "$mutant_dir"' EXIT HUP INT TERM

require() { grep -F "$2" "$1" >/dev/null; }
impl="$src/cache/p50_adopted_outcome_writer.cpp"
header="$src/cache/p50_adopted_outcome_writer.h"
test_source="$src/unittests/p50_adopted_outcome_writer_test.cpp"

for pair in \
    "$header|class P5coAdoptedSocketLease" \
    "$header|class P5coEndpointHandoff" \
    "$header|std::optional<P5coEndpointHandoff>" \
    "$header|P50CacheSessionOutcome" \
    "$header|MonotonicObservationSource" \
    "$header|CLOCK_MONOTONIC" \
    "$header|send_nonblocking" \
    "$header|DontWait =" \
    "$header|NoSignal =" \
    "$impl|outcome_.valid()" \
    "$impl|outcome_.kind == daemon::P50CacheSessionOutcomeKind::Adopted" \
    "$impl|canonical_p5co_ = daemon::encode_cache_session_outcome(outcome_)" \
    "$impl|observations_->observe()" \
    "$impl|P5coFailure::TerminalEvent" \
    "$impl|P5coFailure::EndpointStartExpired" \
    "$impl|deadline_.matches_clock" \
    "$impl|P5coSendFlag::DontWait | P5coSendFlag::NoSignal" \
    "$impl|fence_owned(); // INVALID_CONSTRUCTION_FENCE" \
    "$impl|take_for_endpoint"; do
    file=${pair%%|*}
    pattern=${pair#*|}
    require "$file" "$pattern"
done

# Structural checks complement executable mutants.  These counts ensure that
# the initial/final send checks and the endpoint-transfer check are all present.
[ "$(grep -c 'observations_->observe()' "$impl")" -ge 3 ]
[ "$(grep -c 'lease_->revalidate(outcome_, deadline_)' "$impl")" -ge 3 ]
[ "$(grep -c 'fence_owned();' "$impl")" -ge 3 ]
if grep -E 'fcntl|F_SETFL|O_NONBLOCK|[^_]write\(' "$impl" >/dev/null; then
    echo 'FAIL: reducer owns or mutates a raw/shared fd mode' >&2
    exit 1
fi
if grep -E 'local::Identity operation_identity|std::vector<uint8_t> canonical_p5co,' \
    "$header" >/dev/null; then
    echo 'FAIL: arbitrary frame/bare identity constructor seam remains' >&2
    exit 1
fi

# Every mutant must compile.  A compile failure is itself a gate failure; a
# compiled mutant must then be rejected by the executable semantic rows.
run_rejected_mutant() {
    name=$1
    mutant=$2
    exe="$mutant_dir/$name"
    if ! "$cxx" -std=c++20 -Wall -Wextra -Werror \
        -I"$src/cache" -I"$src/services" -I"$src" \
        "$mutant" "$src/services/p50_cache_session_wire.cpp" \
        "$test_source" -o "$exe" >"$mutant_dir/$name.compile.log" 2>&1; then
        cat "$mutant_dir/$name.compile.log" >&2
        echo "FAIL: deletion mutant did not compile: $name" >&2
        exit 1
    fi
    if "$exe" >"$mutant_dir/$name.run.log" 2>&1; then
        cat "$mutant_dir/$name.run.log" >&2
        echo "FAIL: deletion mutant survived: $name" >&2
        exit 1
    fi
}

# Drop MSG_NOSIGNAL while preserving the uint8_t call type.
sed 's/P5coSendFlag::DontWait | P5coSendFlag::NoSignal/static_cast<uint8_t>(P5coSendFlag::DontWait)/' \
    "$impl" >"$mutant_dir/drop-nosignal.cpp"
run_rejected_mutant drop-nosignal "$mutant_dir/drop-nosignal.cpp"

# Reuse the initial pre-expiry observation for the final syscall check.
sed 's/const std::optional<MonotonicObservation> final = observations_->observe();/const std::optional<MonotonicObservation> final = initial;/' \
    "$impl" >"$mutant_dir/renew-prewrite-time.cpp"
run_rejected_mutant renew-prewrite-time "$mutant_dir/renew-prewrite-time.cpp"

# Remove only the exact-owner check following the second observation.
awk '
    /Linearization point:/ { in_final = 1; print; next }
    in_final && /if \(!lease_->revalidate\(outcome_, deadline_\)\)/ {
        skip = 4
        next
    }
    skip > 0 { --skip; next }
    in_final && /const P5coWriteResult result/ { in_final = 0 }
    { print }
' "$impl" >"$mutant_dir/drop-second-identity.cpp"
run_rejected_mutant drop-second-identity "$mutant_dir/drop-second-identity.cpp"

# Delete the writer destructor's fence.
sed 's/AdoptedOutcomeWriter::~AdoptedOutcomeWriter() noexcept { fence_owned(); }/AdoptedOutcomeWriter::~AdoptedOutcomeWriter() noexcept {}/' \
    "$impl" >"$mutant_dir/drop-destructor-fence.cpp"
run_rejected_mutant drop-destructor-fence "$mutant_dir/drop-destructor-fence.cpp"

# Delete the synchronous construction fence; invalid-construction rows fail
# before the writer destructor can account for the lease.
sed 's/fence_owned(); \/\/ INVALID_CONSTRUCTION_FENCE/(void)0; \/\/ INVALID_CONSTRUCTION_FENCE/' \
    "$impl" >"$mutant_dir/drop-construction-fence.cpp"
run_rejected_mutant drop-construction-fence "$mutant_dir/drop-construction-fence.cpp"

# Restore the old Ready->Writing mutation on EAGAIN/EINTR.
awk '
    /if \(\(result.kind == P5coWriteKind::WouldBlock/ { in_eagain = 1 }
    in_eagain && /return state_;/ {
        print "        return state_ = P5coWriterState::Writing;"
        in_eagain = 0
        next
    }
    { print }
' "$impl" >"$mutant_dir/eagain-mutates-state.cpp"
run_rejected_mutant eagain-mutates-state "$mutant_dir/eagain-mutates-state.cpp"

# Remove terminal-first classification while retaining compilable control flow.
sed '0,/state_ != P5coWriterState::FailedAfterDetach/s//false/' \
    "$impl" >"$mutant_dir/drop-terminal-first.cpp"
run_rejected_mutant drop-terminal-first "$mutant_dir/drop-terminal-first.cpp"

# Suppress exact outcome/deadline identity validation.  The typed mismatch row
# and prewrite owner row must reject the resulting unsafe send.
sed 's/if (!lease_->revalidate(outcome_, deadline_))/if (false)/g' \
    "$impl" >"$mutant_dir/drop-outcome-mismatch.cpp"
run_rejected_mutant drop-outcome-mismatch "$mutant_dir/drop-outcome-mismatch.cpp"

# Allow endpoint transfer after the original absolute deadline has expired.
sed 's/if (observation->now_ns >= deadline_.expires_at_ns)/if (false)/' \
    "$impl" >"$mutant_dir/drop-endpoint-deadline.cpp"
run_rejected_mutant drop-endpoint-deadline "$mutant_dir/drop-endpoint-deadline.cpp"

# Revalidate ownership before the fresh endpoint observation.  The ordered
# event-log row must reject this TOCTOU-prone transfer order.
awk '
    /const std::optional<MonotonicObservation> observation = observations_->observe\(\);/ {
        print "    if (!lease_->revalidate(outcome_, deadline_)) {"
        print "        fail(P5coFailure::OwnershipLost);"
        print "        return {};"
        print "    }"
    }
    { print }
' "$impl" >"$mutant_dir/endpoint-owner-before-observation.cpp"
run_rejected_mutant endpoint-owner-before-observation \
    "$mutant_dir/endpoint-owner-before-observation.cpp"

# Return a handoff with empty outcome metadata.  The endpoint bundle must
# retain the exact canonical claim/launch/store/operation DTO.
sed 's/P5coEndpointHandoff handoff(std::move(lease_), std::move(outcome_), deadline_);/P5coEndpointHandoff handoff(std::move(lease_), daemon::P50CacheSessionOutcome{}, deadline_);/' \
    "$impl" >"$mutant_dir/drop-handoff-outcome.cpp"
run_rejected_mutant drop-handoff-outcome "$mutant_dir/drop-handoff-outcome.cpp"

echo 'ok - P5CO adopted outcome reducer source/deletion gates hold (11 compiled mutants rejected)'
