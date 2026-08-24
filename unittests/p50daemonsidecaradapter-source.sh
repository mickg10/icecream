#!/bin/sh
set -eu

top_src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
top_build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
unit_build=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
service=${ICECC_TEST_CACHE_SERVICE:?ICECC_TEST_CACHE_SERVICE is required}
cxx=${ICECC_TEST_CXX:-c++}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++20}
src="$top_src/cache/p50_daemon_sidecar_adapter.cpp"
header="$top_src/cache/p50_daemon_sidecar_adapter.h"
test_binary="$unit_build/p50daemonsidecaradapter"

test -x "$service" && test -x "$test_binary" || {
    echo 'FAIL: adapter source gate requires the built service and runtime test' >&2
    exit 1
}

grep -F 'connect_unix_until' "$src" >/dev/null
grep -F 'cumulative_post_ready_exits_' "$src" >/dev/null
grep -F 'prior_supervisor_post_ready_exits_' "$src" >/dev/null
grep -F 'runtime_nodes_valid()' "$src" >/dev/null
grep -F 'socket_info.st_dev == socket_device_' "$src" >/dev/null
grep -F 'socket_info.st_ino == socket_inode_' "$src" >/dev/null
grep -F 'directory_info.st_ino == attempt_directory_inode_' "$src" >/dev/null
grep -F 'append_update(update, controller_.observe' "$src" >/dev/null
grep -F 'max_restarts = 0' "$src" >/dev/null
grep -F 'observe_public_listener' "$header" >/dev/null

if grep -E 'daemon/main\.cpp|signal\(|sigaction\(|listen_unix\(' "$src" "$header" >/dev/null; then
    echo 'FAIL: adapter acquired daemon-main, signal-handler, or public-listener ownership' >&2
    exit 1
fi
if test "$(grep -Fc '(void)::unlink(socket_path_.c_str());' "$src")" -ne 1; then
    echo 'FAIL: exact inode-bound stale-socket cleanup is missing or duplicated' >&2
    exit 1
fi

tmp_root=$(mktemp -d "${TMPDIR:-/tmp}/p50daemonsidecaradapter-source.XXXXXX")
cleanup() {
    rm -rf -- "$tmp_root"
}
trap cleanup EXIT HUP INT TERM

# The production object must contain no test-only counter/overflow hook.
production_object="$tmp_root/production.o"
"$cxx" "$standard" -Wall -Wextra -Werror -pthread -DHAVE_CONFIG_H \
    -I"$top_build" -I"$top_src" -I"$top_src/cache" \
    -I"$top_src/client" -I"$top_src/services" \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
    -UICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS -c "$src" -o "$production_object"
if nm -C "$production_object" | grep -E 'test_force_attempt|test_force_counter_state' >/dev/null ||
   strings "$production_object" | grep -F 'ICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS' >/dev/null; then
    echo 'FAIL: production adapter contains a test-only hook' >&2
    exit 1
fi
echo 'ok - production adapter contains no test-only hook'

ICECC_TEST_CACHE_SERVICE="$service" "$test_binary"
echo 'ok - linked service/SCM_RIGHTS lifecycle baseline passes'

test_object="$unit_build/p50daemonsidecaradapter-p50_daemon_sidecar_adapter_test.o"
dispatch_object="$unit_build/p50daemonsidecaradapter-p50_daemon_cache_dispatch.o"
handoff_object="$unit_build/p50daemonsidecaradapter-p50_fd_handoff.o"
for object in "$test_object" "$dispatch_object" "$handoff_object"; do
    test -f "$object" || {
        echo "FAIL: missing adapter mutant dependency $object" >&2
        exit 1
    }
done

compile_and_expect_red() {
    label=$1
    mutant=$2
    object="$tmp_root/$label.o"
    binary="$tmp_root/$label"
    log="$tmp_root/$label.log"
    "$cxx" "$standard" -Wall -Wextra -Werror -pthread -DHAVE_CONFIG_H \
        -I"$top_build" -I"$top_src" -I"$top_src/cache" \
        -I"$top_src/client" -I"$top_src/services" \
        ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
        -c "$mutant" -o "$object"
    "$cxx" "$standard" -pthread ${ICECC_TEST_LDFLAGS:-} \
        "$test_object" "$dispatch_object" "$handoff_object" "$object" \
        "$top_build/cache/libp50readyadvertisement.a" \
        "$top_build/cache/libp50sidecarsupervisor.a" \
        "$top_build/cache/libp50localtransport.a" \
        "$top_build/services/.libs/libicecc.a" -llzo2 \
        ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} -o "$binary"
    set +e
    ICECC_TEST_CACHE_SERVICE="$service" timeout 60s "$binary" >"$log" 2>&1
    status=$?
    set -e
    if test "$status" -eq 0; then
        echo "FAIL: $label mutant survived the executable lifecycle gate" >&2
        cat "$log" >&2
        exit 1
    fi
    echo "ok - $label mutant is rejected (status $status)"
}

exact_mode_mutant="$tmp_root/exact-mode.cpp"
sed '0,/(info.st_mode & 07777) != 0700/s//(info.st_mode \& 0077) != 0/' \
    "$src" >"$exact_mode_mutant"
cmp -s "$src" "$exact_mode_mutant" && {
    echo 'FAIL: exact-mode mutant was not applied' >&2
    exit 1
}
compile_and_expect_red exact-mode "$exact_mode_mutant"

same_identity_mutant="$tmp_root/same-identity.cpp"
sed 's/config.expected_service_uid != config.expected_daemon_uid ||/false ||/' \
    "$src" >"$same_identity_mutant"
cmp -s "$src" "$same_identity_mutant" && {
    echo 'FAIL: same-identity mutant was not applied' >&2
    exit 1
}
compile_and_expect_red same-identity "$same_identity_mutant"

runtime_revalidation_mutant="$tmp_root/runtime-revalidation.cpp"
sed 's/if (!runtime_nodes_valid()) {/if (false) {/' \
    "$src" >"$runtime_revalidation_mutant"
cmp -s "$src" "$runtime_revalidation_mutant" && {
    echo 'FAIL: runtime-revalidation mutant was not applied' >&2
    exit 1
}
compile_and_expect_red runtime-revalidation "$runtime_revalidation_mutant"

inode_cleanup_mutant="$tmp_root/inode-cleanup.cpp"
sed 's/directory_info.st_ino == attempt_directory_inode_;/true;/' \
    "$src" >"$inode_cleanup_mutant"
cmp -s "$src" "$inode_cleanup_mutant" && {
    echo 'FAIL: inode-cleanup mutant was not applied' >&2
    exit 1
}
compile_and_expect_red inode-cleanup "$inode_cleanup_mutant"

echo 'PASS: daemon sidecar adapter production guards and executable mutants hold'
