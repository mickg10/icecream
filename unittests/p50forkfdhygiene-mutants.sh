#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
source=$src/daemon/p50_fork_fd_hygiene.cpp
test=$src/unittests/p50_fork_fd_hygiene_test.cpp
header=$src/daemon/p50_fork_fd_hygiene.h
test -s "$source" -a -s "$test" -a -s "$header"

mutant=$(mktemp "${TMPDIR:-/tmp}/p50forkfdhygiene.XXXXXX")
identity_dir=
mutant_dir=
trap 'rm -f "$mutant"; rm -rf "$mutant_dir" "$identity_dir"' EXIT HUP INT TERM

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

# The identity guard and its move-only owner mint are deletion-sensitive.
grep -F 'owner.expected_delivery_id_' "$source" >/dev/null
grep -F 'source->valid()' "$source" >/dev/null
grep -F 'source_identity_equal' "$source" >/dev/null
grep -F 'dup3' "$test" >/dev/null

sed '/CLOSE_RANGE_CLOEXEC/d' "$source" >"$mutant"
if grep -F 'CLOSE_RANGE_CLOEXEC' "$mutant" >/dev/null; then
    echo 'FAIL: CLOEXEC-only mutant remained' >&2
    exit 1
fi
echo 'PASS: deletion-sensitive hygiene mutants are observable'
