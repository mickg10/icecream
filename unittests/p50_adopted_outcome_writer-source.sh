#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
impl="$src/cache/p50_adopted_outcome_writer.cpp"
header="$src/cache/p50_adopted_outcome_writer.h"
cxx=${ICECC_TEST_CXX:-c++}
mutant_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50-adopted-writer-source.XXXXXX")
trap 'rm -rf "$mutant_dir"' EXIT HUP INT TERM

require() { grep -F "$2" "$1" >/dev/null; }
for pair in \
    "$header|class P5coAdoptedSocketLease" \
    "$header|std::unique_ptr<P5coAdoptedSocketLease>" \
    "$header|P50CacheSessionOutcome" \
    "$header|MonotonicObservationSource" \
    "$header|CLOCK_MONOTONIC" \
    "$header|send_nonblocking" \
    "$header|DontWait =" \
    "$header|NoSignal =" \
    "$impl|outcome_.valid()" \
    "$impl|outcome_.kind == daemon::P50CacheSessionOutcomeKind::Adopted" \
    "$impl|encode_cache_session_outcome" \
    "$impl|canonical_p5co_ = daemon::encode_cache_session_outcome(outcome_)" \
    "$impl|observations_->observe()" \
    "$impl|P5coFailure::TerminalEvent" \
    "$impl|deadline_valid(*final)" \
    "$impl|lease_->revalidate(outcome_, deadline_)" \
    "$impl|P5coSendFlag::DontWait | P5coSendFlag::NoSignal" \
    "$impl|fence_owned()" \
    "$impl|take_for_endpoint"; do
    file=${pair%%|*}; pattern=${pair#*|}
    require "$file" "$pattern"
done

# These are source-level deletion guards in addition to executable mutants.
[ "$(grep -c 'observations_->observe()' "$impl")" -ge 3 ]
[ "$(grep -c 'lease_->revalidate(outcome_, deadline_)' "$impl")" -ge 3 ]
[ "$(grep -c 'fence_owned();' "$impl")" -ge 3 ]
if grep -E 'fcntl|F_SETFL|O_NONBLOCK|[^_]write\(' "$impl" >/dev/null; then
    echo 'FAIL: reducer owns or mutates a raw/shared fd mode' >&2
    exit 1
fi
if grep -E 'local::Identity operation_identity|std::vector<uint8_t> canonical_p5co,' "$header" >/dev/null; then
    echo 'FAIL: arbitrary frame/bare identity constructor seam remains' >&2
    exit 1
fi

# Every mutant must compile.  A compile failure is itself a gate failure.
run_rejected_mutant() {
    name=$1
    mutant=$2
    exe="$mutant_dir/$name"
    if ! "$cxx" -std=c++20 -Wall -Wextra -Werror \
        -I"$src/cache" -I"$src/services" -I"$src" \
        "$mutant" "$src/services/p50_cache_session_wire.cpp" \
        "$src/unittests/p50_adopted_outcome_writer_test.cpp" \
        -o "$exe" >"$mutant_dir/$name.compile.log" 2>&1; then
        cat "$mutant_dir/$name.compile.log" >&2
        echo "FAIL: deletion mutant did not compile: $name" >&2
        exit 1
    fi
    if "$exe" >/dev/null 2>&1; then
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
    /Linearization point:/ { print; final = 1; next }
    final && /    if \(!lease_->revalidate\(outcome_, deadline_\)\)/ { drop = 1; next }
    drop && /    }/ { drop = 0; next }
    final && /    const P5coWriteResult result/ { final = 0 }
    drop { next }
    { print }
' "$impl" >"$mutant_dir/drop-second-identity.cpp"
run_rejected_mutant drop-second-identity "$mutant_dir/drop-second-identity.cpp"

sed 's/AdoptedOutcomeWriter::~AdoptedOutcomeWriter() noexcept { fence_owned(); }/AdoptedOutcomeWriter::~AdoptedOutcomeWriter() noexcept {}/' \
    "$impl" >"$mutant_dir/drop-destructor-fence.cpp"
run_rejected_mutant drop-destructor-fence "$mutant_dir/drop-destructor-fence.cpp"

# Remove outcome/deadline matching in the fake lease's revalidation call path
# by suppressing both writer checks; the exact-mismatch row must fail.
sed 's/if (!lease_->revalidate(outcome_, deadline_))/if (false)/g' \
    "$impl" >"$mutant_dir/drop-outcome-mismatch.cpp"
run_rejected_mutant drop-outcome-mismatch "$mutant_dir/drop-outcome-mismatch.cpp"

# Endpoint transfer must still observe the original deadline.
sed '/if (!deadline_valid(\*observation))/,/        return {/ s/if (!deadline_valid(\*observation))/if (false)/' \
    "$impl" >"$mutant_dir/drop-endpoint-deadline.cpp"
run_rejected_mutant drop-endpoint-deadline "$mutant_dir/drop-endpoint-deadline.cpp"

echo 'ok - P5CO adopted outcome reducer source/deletion gates hold'
