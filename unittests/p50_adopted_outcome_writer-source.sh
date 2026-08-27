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
    "$header|AbsoluteMonotonicDeadline" \
    "$header|send_nonblocking" \
    "$header|DontWait =" \
    "$header|NoSignal =" \
    "$impl|P5coWriterState::FailedAfterDetach" \
    "$impl|P5coFailure::TerminalEvent" \
    "$impl|deadline_valid(now_ns, clock)" \
    "$impl|lease_->revalidate(operation_identity_)" \
    "$impl|P5coSendFlag::DontWait | P5coSendFlag::NoSignal" \
    "$impl|take_for_endpoint"; do
    file=${pair%%|*}; pattern=${pair#*|}
    require "$file" "$pattern"
done

# Deletion-sensitive checks: the pre-syscall linearization needs both an
# immediate deadline check and a second exact-owner observation.  The flags
# must remain per-call, not a shared O_NONBLOCK mutation.
[ "$(grep -c 'lease_->revalidate(operation_identity_)' "$impl")" -ge 2 ]
[ "$(grep -c 'deadline_valid(now_ns, clock)' "$impl")" -ge 1 ]
if grep -E 'fcntl|F_SETFL|O_NONBLOCK|[^_]write\(' "$impl" >/dev/null; then
    echo 'FAIL: reducer owns or mutates a raw/shared fd mode' >&2
    exit 1
fi

# Compile and run semantic deletion mutants against the executable fixture.
# A mutant is expected to fail the focused rows; surviving is a gate failure.
run_rejected_mutant() {
    name=$1
    mutant=$2
    exe="$mutant_dir/$name"
    if ! "$cxx" -std=c++20 -Wall -Wextra -Werror \
        -I"$src/cache" -I"$src/services" -I"$src" \
        "$mutant" "$src/unittests/p50_adopted_outcome_writer_test.cpp" \
        -o "$exe"; then
        return 0
    fi
    if "$exe" >/dev/null 2>&1; then
        echo "FAIL: deletion mutant survived: $name" >&2
        exit 1
    fi
}

sed 's/P5coSendFlag::DontWait | P5coSendFlag::NoSignal/P5coSendFlag::DontWait/' \
    "$impl" >"$mutant_dir/drop-nosignal.cpp"
run_rejected_mutant drop-nosignal "$mutant_dir/drop-nosignal.cpp"

sed 's/return deadline_.matches_clock(clock) && now_ns < deadline_.expires_at_ns;/return true;/' \
    "$impl" >"$mutant_dir/renew-deadline.cpp"
run_rejected_mutant renew-deadline "$mutant_dir/renew-deadline.cpp"

# Remove only the revalidation guarded by the pre-syscall linearization
# comment, leaving the earlier per-turn observation intact.
awk '
    /Linearization point:/ { print; skip = 1; next }
    skip && /    if \(!lease_->revalidate\(operation_identity_\)\)/ { drop = 1; next }
    drop && /    }/ { drop = 0; next }
    skip && /    const P5coWriteResult result/ { skip = 0 }
    drop { next }
    { print }
' "$impl" >"$mutant_dir/drop-prewrite-owner.cpp"
run_rejected_mutant drop-prewrite-owner "$mutant_dir/drop-prewrite-owner.cpp"

echo 'ok - P5CO adopted outcome reducer source/deletion gates hold'
