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
trap 'rm -f "$mutant"; rm -rf "$mutant_dir" "$identity_dir" "$move_dir" "$proof_dir" "$mutable_dir" "$alias_dir" "$kcmp_dir" "$alias_reject_dir" "$slot_dir"' EXIT HUP INT TERM

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
if "$mutant_dir/test" >/dev/null 2>&1; then
    echo 'FAIL: accepted DeliveryId 77->88 mutant survived' >&2
    exit 1
fi

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
if "$identity_dir/test" >/dev/null 2>&1; then
    echo 'FAIL: source identity deletion mutant survived' >&2
    exit 1
fi
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
if "$move_dir/test" >/dev/null 2>&1; then
    echo 'FAIL: borrowed-handoff move-assignment mutant survived' >&2
    exit 1
fi
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
if "$proof_dir/test" >/dev/null 2>&1; then
    echo 'FAIL: proof-FD identity-retirement mutant survived' >&2
    exit 1
fi
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
if "$kcmp_dir/test" >/dev/null 2>&1; then
    echo 'FAIL: kcmp unavailable/denied retirement mutant survived' >&2
    exit 1
fi
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
if "$alias_reject_dir/test" >/dev/null 2>&1; then
    echo 'FAIL: rejected-alias disarm mutant survived' >&2
    exit 1
fi
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
if "$slot_dir/test" >/dev/null 2>&1; then
    echo 'FAIL: move-assignment proof-slot overwrite mutant survived' >&2
    exit 1
fi
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
if "$alias_dir/test" >/dev/null 2>&1; then
    echo 'FAIL: owned-proof/handoff alias mutant survived' >&2
    exit 1
fi
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
if "$mutable_dir/test" >/dev/null 2>&1; then
    echo 'FAIL: unsealed-mutable-source mutant survived' >&2
    exit 1
fi
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
