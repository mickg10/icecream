#!/bin/sh
set -eu

root=${ICECC_TEST_TOP_SRCDIR:?}
build=${ICECC_TEST_TOP_BUILDDIR:?}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
cppflags=${ICECC_TEST_CPPFLAGS:-}
ldflags=${ICECC_TEST_LDFLAGS:-}
work=$(mktemp -d "${TMPDIR:-/tmp}/p50sourcearm-wait-mutants.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

common="-O1 -g -Wall -Wextra -Werror $cppflags ${ICECC_TEST_LIBZSTD_CFLAGS:-} ${ICECC_TEST_XXHASH_CFLAGS:-} -I$root -I$root/cache -I$root/services"
libs="$ldflags -L$build/services/.libs -licecc ${ICECC_TEST_LIBS:-} ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} ${ICECC_TEST_XXHASH_LIBS:--lxxhash} -llzo2 -ldl -pthread"

compile_lease() {
    "$cxx" $standard $common \
        "$root/unittests/p50_source_arm_wait_lease_test.cpp" \
        $libs -o "$work/lease"
}

cp "$root/daemon/p50_source_arm_wait_lease.h" "$work/p50_source_arm_wait_lease.h"
sed -i 's/left.store_generation == right.store_generation/true \/\* frozen-generation mutant *\//' \
    "$work/p50_source_arm_wait_lease.h"
compile_lease_mutant() {
    "$cxx" $standard $common -I"$work" -I"$root/daemon" \
        "$root/unittests/p50_source_arm_wait_lease_test.cpp" \
        $libs -o "$work/lease-mutant"
}
compile_lease_mutant
if "$work/lease-mutant"; then
    echo 'FAIL: frozen F-store-generation comparison mutant survived' >&2
    exit 1
fi
echo 'ok - frozen F-store-generation mutant reddened'

for field in c_control_generation c_control_attempt; do
    cp "$root/cache/p50_input_wait.cpp" "$work/p50_input_wait-$field.cpp"
    sed -i "s/left\.${field} == right\.${field}/true \/\* dropped-${field} mutant *\//" \
        "$work/p50_input_wait-$field.cpp"
    "$cxx" $standard $common \
        "$root/unittests/p50_source_arm_wait_test.cpp" \
        "$root/cache/p50_source_identity.cpp" "$work/p50_input_wait-$field.cpp" \
        $libs -o "$work/arm-mutant-$field"
    if "$work/arm-mutant-$field"; then
        echo "FAIL: dropped canonical $field mutant survived" >&2
        exit 1
    fi
    echo "ok - dropped canonical $field mutant reddened"
done
