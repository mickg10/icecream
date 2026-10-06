#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
source=$src/daemon/p50_fork_fd_hygiene.cpp
test=$src/unittests/p50_fork_fd_hygiene_test.cpp
header=$src/daemon/p50_fork_fd_hygiene.h
test -s "$source" -a -s "$test" -a -s "$header"

mutant=$(mktemp "${TMPDIR:-/tmp}/p50forkfdhygiene.XXXXXX")
identity_dir=
move_dir=
proof_dir=
mutable_dir=
mutant_dir=
alias_dir=
kcmp_dir=
alias_reject_dir=
slot_dir=
baseline_dir=
trap 'rm -f "$mutant"; rm -rf "$mutant_dir" "$identity_dir" "$move_dir" "$proof_dir" "$mutable_dir" "$alias_dir" "$kcmp_dir" "$alias_reject_dir" "$slot_dir" "$baseline_dir"' EXIT HUP INT TERM

# A killed mutant must FAIL, not skip: the test binary exits 77 when kcmp
# is unavailable (docker's default seccomp), and the plain kill checks
# below treat ANY nonzero exit as killed -- so under that profile every
# mutant passed VACUOUSLY.  Run the UNMUTATED baseline first:
#   77 -> the whole script skips (the only honest signal under seccomp),
#   nonzero -> the baseline itself is broken, a hard FAIL,
#   0 -> proceed, and every kill check demands a REAL failure (not 77).
baseline_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50forkfdhygiene-baseline.XXXXXX")
mkdir -p "$baseline_dir/daemon" "$baseline_dir/unittests"
cp "$source" "$baseline_dir/daemon/p50_fork_fd_hygiene.cpp"
cp "$header" "$baseline_dir/daemon/p50_fork_fd_hygiene.h"
cp "$test" "$baseline_dir/unittests/p50_fork_fd_hygiene_test.cpp"
cxx=${ICECC_TEST_CXX:-c++}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
"$cxx" "$standard" -Wall -Wextra -Werror \
    -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS -I"$baseline_dir" \
    "$baseline_dir/unittests/p50_fork_fd_hygiene_test.cpp" \
    "$baseline_dir/daemon/p50_fork_fd_hygiene.cpp" -o "$baseline_dir/test"
# Capture the baseline OUTPUT too: rc 0 with "kcmp-dependent case(s)
# skipped" notes means the kcmp-gated cases (owner mint, proof retirement,
# same-number replacement -- the observables most mutants below need) did
# NOT run, so their kills would be vacuous even though the binary exited 0.
# Under that shape the mutants that depend on the skipped cases cannot be
# observed; the honest outcome is a SKIP, same as the explicit 77.
"$baseline_dir/test" >"$baseline_dir/out.log" 2>&1
baseline_rc=$?
if [ "$baseline_rc" -eq 77 ]; then
    echo 'SKIP: kcmp unavailable (seccomp blocks SYS_kcmp); the mutants cannot be observed' >&2
    exit 77
fi
if [ "$baseline_rc" -ne 0 ]; then
    echo "FAIL: the UNMUTATED baseline exited $baseline_rc -- every mutant kill below would be vacuous" >&2
    exit 1
fi
if grep -q "kcmp-dependent case(s) skipped" "$baseline_dir/out.log" 2>/dev/null; then
    echo "SKIP: the baseline skipped its kcmp-dependent cases under this seccomp profile -- the owner-mint/proof-retirement mutants below cannot be observed (they would pass vacuously)" >&2
    exit 77
fi
rm -rf "$baseline_dir"
baseline_dir=

# A mutant is killed only by a REAL failure: 77 is the test's own skip,
# never a kill.
kill_or_fail() { # <test-binary> <mutant-name>
    kb=$1
    kn=$2
    if "$kb" >/dev/null 2>&1; then
        echo "FAIL: $kn mutant survived" >&2
        exit 1
    fi
    kb_rc=$?
    if [ "$kb_rc" -eq 77 ]; then
        echo "FAIL: $kn mutant exited 77 (skip) -- vacuous kill; the green baseline means this mutant must fail for a REAL reason" >&2
        exit 1
    fi
}

# A real runtime mutant changes the accepted source DeliveryId while the
# owner still authorizes 77.  The mutant must fail before any sweep runs.
mutant_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50forkfdhygiene-mutant.XXXXXX")
trap 'rm -rf "$mutant_dir" "$mutant"' EXIT HUP INT TERM
mkdir -p "$mutant_dir/daemon" "$mutant_dir/unittests"
cp "$source" "$mutant_dir/daemon/p50_fork_fd_hygiene.cpp"
cp "$header" "$mutant_dir/daemon/p50_fork_fd_hygiene.h"
sed 's/constexpr uint64_t kAcceptedDeliveryId = 77;/constexpr uint64_t kAcceptedDeliveryId = 88;/' \
    "$test" >"$mutant_dir/unittests/p50_fork_fd_hygiene_test.cpp"
cxx=${ICECC_TEST_CXX:-c++}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
"$cxx" "$standard" -Wall -Wextra -Werror \
    -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS -I"$mutant_dir" \
    "$mutant_dir/unittests/p50_fork_fd_hygiene_test.cpp" \
    "$mutant_dir/daemon/p50_fork_fd_hygiene.cpp" -o "$mutant_dir/test"
kill_or_fail "$mutant_dir/test" "accepted DeliveryId 77->88"

# A true identity-deletion mutant must also be caught: replacing the immutable
# dev/ino/mode/size/seals comparison with unconditional success lets dup3()
# same-number replacement through mint.
identity_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50forkfdhygiene-identity-mutant.XXXXXX")
mkdir -p "$identity_dir/daemon" "$identity_dir/unittests"
sed 's/return source_identity_equal(actual, expected);/return expected.valid() || source_identity_equal(actual, expected);/' \
    "$source" >"$identity_dir/daemon/p50_fork_fd_hygiene.cpp"
cp "$header" "$identity_dir/daemon/p50_fork_fd_hygiene.h"
cp "$test" "$identity_dir/unittests/p50_fork_fd_hygiene_test.cpp"
"$cxx" "$standard" -Wall -Wextra -Werror \
    -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS -I"$identity_dir" \
    "$identity_dir/unittests/p50_fork_fd_hygiene_test.cpp" \
    "$identity_dir/daemon/p50_fork_fd_hygiene.cpp" -o "$identity_dir/test"
kill_or_fail "$identity_dir/test" "source identity deletion"
rm -rf "$identity_dir"

# A true ownership mutant closes the borrowed handoff while replacing a lease.
# The caller-owned descriptor must survive both move-assignment and lease
# destruction; deleting that distinction must redden the runtime regression.
move_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50forkfdhygiene-move-mutant.XXXXXX")
mkdir -p "$move_dir/daemon" "$move_dir/unittests"
sed 's/borrowed_handoff_fd_ = other.borrowed_handoff_fd_;/if (borrowed_handoff_fd_ >= 0) { (void)::close(borrowed_handoff_fd_); } borrowed_handoff_fd_ = other.borrowed_handoff_fd_;/' \
    "$source" >"$move_dir/daemon/p50_fork_fd_hygiene.cpp"
cp "$header" "$move_dir/daemon/p50_fork_fd_hygiene.h"
cp "$test" "$move_dir/unittests/p50_fork_fd_hygiene_test.cpp"
"$cxx" "$standard" -Wall -Wextra -Werror \
    -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS -I"$move_dir" \
    "$move_dir/unittests/p50_fork_fd_hygiene_test.cpp" \
    "$move_dir/daemon/p50_fork_fd_hygiene.cpp" -o "$move_dir/test"
kill_or_fail "$move_dir/test" "borrowed-handoff move-assignment"
rm -rf "$move_dir"

# A true retirement-ownership mutant exposes the private proof handle as the
# caller-reusable numeric slot.  The executable owner/lease proof-reuse rows
# must catch the resulting close of a caller replacement.
proof_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50forkfdhygiene-proof-mutant.XXXXXX")
mkdir -p "$proof_dir/daemon" "$proof_dir/unittests"
sed -e 's/return owner\.proof_slot_fd_;/return owner.owned_proof_fd_;/' \
    -e 's/return lease\.proof_slot_fd_;/return lease.owned_proof_fd_;/' \
    "$source" >"$proof_dir/daemon/p50_fork_fd_hygiene.cpp"
cp "$header" "$proof_dir/daemon/p50_fork_fd_hygiene.h"
cp "$test" "$proof_dir/unittests/p50_fork_fd_hygiene_test.cpp"
"$cxx" "$standard" -Wall -Wextra -Werror \
    -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS -I"$proof_dir" \
    "$proof_dir/unittests/p50_fork_fd_hygiene_test.cpp" \
    "$proof_dir/daemon/p50_fork_fd_hygiene.cpp" -o "$proof_dir/test"
kill_or_fail "$proof_dir/test" "proof-FD identity-retirement"
rm -rf "$proof_dir"

# A true error-mode mutant restores the rejected behavior: a failed kcmp
# suppresses proof retirement.  The ENOSYS/EPERM/EINTR executable rows must
# observe the resulting private-handle leak.
kcmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50forkfdhygiene-kcmp-mutant.XXXXXX")
mkdir -p "$kcmp_dir/daemon" "$kcmp_dir/unittests"
sed 's/const bool proof_closed = proof < 0 || close_exact(proof);/const bool proof_closed = proof < 0 || (proved \&\& close_exact(proof));/' \
    "$source" >"$kcmp_dir/daemon/p50_fork_fd_hygiene.cpp"
cp "$header" "$kcmp_dir/daemon/p50_fork_fd_hygiene.h"
cp "$test" "$kcmp_dir/unittests/p50_fork_fd_hygiene_test.cpp"
"$cxx" "$standard" -Wall -Wextra -Werror \
    -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS -I"$kcmp_dir" \
    "$kcmp_dir/unittests/p50_fork_fd_hygiene_test.cpp" \
    "$kcmp_dir/daemon/p50_fork_fd_hygiene.cpp" -o "$kcmp_dir/test"
kill_or_fail "$kcmp_dir/test" "kcmp unavailable/denied retirement"
rm -rf "$kcmp_dir"

# A true rejected-alias mutant skips the production disarm after mint refuses
# an owner.  The executable's natural optional reset must catch the destructor
# closing the caller's handoff without a test-only disarm.
alias_reject_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50forkfdhygiene-alias-reject-mutant.XXXXXX")
mkdir -p "$alias_reject_dir/daemon" "$alias_reject_dir/unittests"
sed 's/owner\.disarm_rejected_alias(fd);/(void)fd;/' \
    "$source" >"$alias_reject_dir/daemon/p50_fork_fd_hygiene.cpp"
cp "$header" "$alias_reject_dir/daemon/p50_fork_fd_hygiene.h"
cp "$test" "$alias_reject_dir/unittests/p50_fork_fd_hygiene_test.cpp"
"$cxx" "$standard" -Wall -Wextra -Werror \
    -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS -I"$alias_reject_dir" \
    "$alias_reject_dir/unittests/p50_fork_fd_hygiene_test.cpp" \
    "$alias_reject_dir/daemon/p50_fork_fd_hygiene.cpp" -o "$alias_reject_dir/test"
kill_or_fail "$alias_reject_dir/test" "rejected-alias disarm"
rm -rf "$alias_reject_dir"

# A true move-assignment mutant restores the slot overwrite.  The executable
# checks both caller-owned observation slots and the final descriptor count.
slot_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50forkfdhygiene-slot-mutant.XXXXXX")
mkdir -p "$slot_dir/daemon" "$slot_dir/unittests"
sed '/        identity_ = other\.identity_;/i\        proof_slot_fd_ = other.proof_slot_fd_;' \
    "$source" >"$slot_dir/daemon/p50_fork_fd_hygiene.cpp"
cp "$header" "$slot_dir/daemon/p50_fork_fd_hygiene.h"
cp "$test" "$slot_dir/unittests/p50_fork_fd_hygiene_test.cpp"
"$cxx" "$standard" -Wall -Wextra -Werror \
    -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS -I"$slot_dir" \
    "$slot_dir/unittests/p50_fork_fd_hygiene_test.cpp" \
    "$slot_dir/daemon/p50_fork_fd_hygiene.cpp" -o "$slot_dir/test"
kill_or_fail "$slot_dir/test" "move-assignment proof-slot overwrite"
rm -rf "$slot_dir"

# The mint boundary must reject an owner proof whose integer slot aliases the
# borrowed handoff.  Removing that explicit check transfers the handoff into
# the lease and its destructor closes the caller-owned descriptor; the alias
# regression makes this true mutant fail at runtime.
alias_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50forkfdhygiene-alias-mutant.XXXXXX")
mkdir -p "$alias_dir/daemon" "$alias_dir/unittests"
sed -e 's/owner\.owned_proof_fd_ == fd/false/' \
    -e 's/proof_handoff != OpenFileComparison::Different/static_cast<int>(proof_handoff) + ::getpid() >= 0/' \
    -e 's/control_handoff != OpenFileComparison::Different/static_cast<int>(control_handoff) + ::getpid() >= 0/' \
    "$source" >"$alias_dir/daemon/p50_fork_fd_hygiene.cpp"
cp "$header" "$alias_dir/daemon/p50_fork_fd_hygiene.h"
cp "$test" "$alias_dir/unittests/p50_fork_fd_hygiene_test.cpp"
"$cxx" "$standard" -Wall -Wextra -Werror \
    -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS -I"$alias_dir" \
    "$alias_dir/unittests/p50_fork_fd_hygiene_test.cpp" \
    "$alias_dir/daemon/p50_fork_fd_hygiene.cpp" -o "$alias_dir/test"
kill_or_fail "$alias_dir/test" "owned-proof/handoff alias"
rm -rf "$alias_dir"

# A true immutability mutant accepts an unsealed regular file by treating a
# failed F_GET_SEALS as if all seals were present.  The mint-time rejection
# regression must redden before such a source can reach sweep.
mutable_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50forkfdhygiene-mutable-mutant.XXXXXX")
mkdir -p "$mutable_dir/daemon" "$mutable_dir/unittests"
sed 's/if (seals < 0)/if (false \&\& seals < 0)/' \
    "$source" >"$mutable_dir/daemon/p50_fork_fd_hygiene.cpp"
cp "$header" "$mutable_dir/daemon/p50_fork_fd_hygiene.h"
cp "$test" "$mutable_dir/unittests/p50_fork_fd_hygiene_test.cpp"
"$cxx" "$standard" -Wall -Wextra -Werror \
    -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS -I"$mutable_dir" \
    "$mutable_dir/unittests/p50_fork_fd_hygiene_test.cpp" \
    "$mutable_dir/daemon/p50_fork_fd_hygiene.cpp" -o "$mutable_dir/test"
kill_or_fail "$mutable_dir/test" "unsealed-mutable-source"
rm -rf "$mutable_dir"

# The identity guard and its move-only owner mint are deletion-sensitive.
grep -F 'owner.expected_delivery_id_' "$source" >/dev/null
grep -F 'source->valid()' "$source" >/dev/null
grep -F 'source_identity_equal' "$source" >/dev/null
grep -F 'retire_owned_proof' "$source" >/dev/null
grep -F 'borrowed_handoff_fd_' "$source" >/dev/null
grep -F 'dup3' "$test" >/dev/null

sed '/CLOSE_RANGE_CLOEXEC/d' "$source" >"$mutant"
if grep -F 'CLOSE_RANGE_CLOEXEC' "$mutant" >/dev/null; then
    echo 'FAIL: CLOEXEC-only mutant remained' >&2
    exit 1
fi
echo 'PASS: deletion-sensitive hygiene mutants are observable'
