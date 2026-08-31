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
test_source="$top_src/unittests/p50_daemon_sidecar_adapter_test.cpp"

test -x "$service" || {
    echo 'FAIL: adapter source gate requires the built service' >&2
    exit 1
}
grep -F 'outer_current_ready_lease()' "$src" >/dev/null
grep -F 'outer_prepare_attempt_retirement' "$src" >/dev/null
grep -F 'launch_identities' "$src" >/dev/null
grep -F 'runtime_directory' "$src" >/dev/null
grep -F 'cumulative_post_ready_exits_' "$src" >/dev/null
grep -F 'observation.cumulative_post_ready_exits' "$src" >/dev/null
grep -F 'runtime_nodes_valid()' "$src" >/dev/null
grep -F 'socket_info.st_dev == socket_device_' "$src" >/dev/null
grep -F 'socket_info.st_ino == socket_inode_' "$src" >/dev/null
grep -F 'directory_info.st_ino != attempt_directory_inode_' "$src" >/dev/null
grep -F 'controller_.observe(observation)' "$src" >/dev/null
grep -F 'max_restarts = 0' "$src" >/dev/null
grep -F 'observe_public_listener' "$header" >/dev/null
grep -F 'outer_begin_turn' "$header" >/dev/null
grep -F 'outer_advance_turn' "$header" >/dev/null
grep -F 'outer_append_pollfds' "$header" >/dev/null
grep -F 'outer_immediate_turn_required' "$header" >/dev/null
grep -F 'outer_prepare_attempt_retirement' "$header" >/dev/null
grep -F 'outer_commit_attempt_replacement' "$header" >/dev/null
grep -F 'outer_close_logical_input_lease' "$header" >/dev/null
grep -F 'AttemptLeafRetirementJoin' "$header" >/dev/null

if grep -E 'daemon/main\.cpp|signal\(|sigaction\(|listen_unix\(' "$src" "$header" >/dev/null; then
    echo 'FAIL: adapter acquired daemon-main, signal-handler, or public-listener ownership' >&2
    exit 1
fi
if grep -F 'unlink(socket_path_.c_str())' "$src" >/dev/null \
        || grep -F 'supervisor_->' "$src" >/dev/null \
        || grep -E '(^|[^[:alnum:]_])waitpid[[:space:]]*\(' "$src" >/dev/null; then
    echo 'FAIL: adapter retains a synchronous supervisor/reap path' >&2
    exit 1
fi

tmp_root=$(mktemp -d "${TMPDIR:-/tmp}/p50daemonsidecaradapter-source.XXXXXX")
runtime_roots=
cleanup() {
    for runtime_root in $runtime_roots; do
        rm -rf -- "$runtime_root"
    done
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
if nm -C "$production_object" | grep -E 'test_force_attempt|test_force_counter_state|test_force_input_lifecycle_operation' >/dev/null ||
   strings "$production_object" | grep -F 'ICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS' >/dev/null; then
    echo 'FAIL: production adapter contains a test-only hook' >&2
    exit 1
fi
echo 'ok - production adapter contains no test-only hook'

test_object="$tmp_root/test.o"
dispatch_object="$tmp_root/dispatch.o"
handoff_object="$tmp_root/handoff.o"
attachment_object="$tmp_root/input_fd_attachment.o"
lifecycle_object="$tmp_root/p50_input_lifecycle.o"
for source_and_object in \
    "$top_src/cache/p50_daemon_cache_dispatch.cpp:$dispatch_object" \
    "$top_src/cache/p50_fd_handoff.cpp:$handoff_object" \
    "$top_src/cache/p50_input_fd_attachment.cpp:$attachment_object" \
    "$top_src/cache/p50_input_lifecycle.cpp:$lifecycle_object"; do
    source=${source_and_object%%:*}
    object=${source_and_object#*:}
    "$cxx" "$standard" -Wall -Wextra -Werror -pthread -DHAVE_CONFIG_H \
        -I"$top_build" -I"$top_src" -I"$top_src/cache" \
        -I"$top_src/client" -I"$top_src/services" \
        ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
        -c "$source" -o "$object"
done
"$cxx" "$standard" -Wall -Wextra -Werror -pthread -DHAVE_CONFIG_H \
    -DICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS \
    -I"$top_build" -I"$top_src" -I"$top_src/cache" \
    -I"$top_src/client" -I"$top_src/services" \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
    -c "$test_source" -o "$test_object"

link_binary() {
    adapter_object=$1
    binary=$2
    "$cxx" "$standard" -pthread ${ICECC_TEST_LDFLAGS:-} \
        "$test_object" "$dispatch_object" "$handoff_object" \
        "$attachment_object" "$lifecycle_object" "$adapter_object" \
        "$top_build/cache/libp50readyadvertisement.a" \
        "$top_build/cache/libp50sidecarlifecycle.a" \
        "$top_build/cache/libp50sidecarsupervisor.a" \
        "$top_build/cache/libp50localtransport.a" \
        "$top_build/cache/libprotocol50.a" \
        "$top_build/services/.libs/libicecc.a" \
        ${ICECC_TEST_LIBCAP_NG_LIBS:-} -llzo2 \
        ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} \
        ${ICECC_TEST_XXHASH_LIBS:--lxxhash} -o "$binary"
}

mutant_sidecar_pids() {
    runtime_root=$1
    for process_root in /proc/[0-9]*; do
        process_exe=$(readlink "$process_root/exe" 2>/dev/null) || continue
        test "$process_exe" = "$service" || continue
        process_command=$(tr '\000' ' ' <"$process_root/cmdline" 2>/dev/null) || continue
        case "$process_command" in
            *" --socket $runtime_root/"*)
                printf '%s\n' "${process_root##*/}"
                ;;
        esac
    done
}

retire_mutant_sidecars() {
    runtime_root=$1
    sidecar_pids=$(mutant_sidecar_pids "$runtime_root")
    for sidecar_pid in $sidecar_pids; do
        kill -TERM "$sidecar_pid" 2>/dev/null
    done
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sidecar_pids=$(mutant_sidecar_pids "$runtime_root")
        test -z "$sidecar_pids" && return 0
        sleep 0.05
    done
    for sidecar_pid in $sidecar_pids; do
        kill -KILL "$sidecar_pid" 2>/dev/null
    done
    sidecar_pids=$(mutant_sidecar_pids "$runtime_root")
    test -z "$sidecar_pids"
}

baseline="$tmp_root/baseline"
link_binary "$production_object" "$baseline"
ICECC_TEST_CACHE_SERVICE="$service" timeout 60s "$baseline"
echo 'ok - current-source linked service/SCM_RIGHTS lifecycle baseline passes'

compile_and_expect_red() {
    label=$1
    mutant=$2
    expected_status=$3
    object="$tmp_root/$label.o"
    binary="$tmp_root/$label"
    log="$tmp_root/$label.log"
    # Keep the socket path short enough for sockaddr_un while retaining an
    # exact, test-owned prefix for detached-child cleanup.
    runtime_root=$(mktemp -d /tmp/p5m.XXXXXX)
    runtime_roots="$runtime_roots $runtime_root"
    "$cxx" "$standard" -Wall -Wextra -Werror -pthread -DHAVE_CONFIG_H \
        -I"$top_build" -I"$top_src" -I"$top_src/cache" \
        -I"$top_src/client" -I"$top_src/services" \
        ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
        -c "$mutant" -o "$object"
    link_binary "$object" "$binary"
    # A rejected mutant can exit before its detached cache-service child.
    # Retire only the service tied to this mutant's unique runtime root.
    set +e
    TMPDIR="$runtime_root" ICECC_TEST_CACHE_SERVICE="$service" \
        timeout 60s "$binary" >"$log" 2>&1
    status=$?
    retire_mutant_sidecars "$runtime_root"
    cleanup_status=$?
    set -e
    if test "$cleanup_status" -ne 0; then
        echo "FAIL: $label mutant left a cache-service child" >&2
        exit 1
    fi
    if test "$status" -ne "$expected_status"; then
        echo "FAIL: $label mutant returned $status, expected $expected_status" >&2
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
compile_and_expect_red exact-mode "$exact_mode_mutant" 7

same_identity_mutant="$tmp_root/same-identity.cpp"
sed 's/config.expected_service_uid != config.expected_daemon_uid ||/false ||/' \
    "$src" >"$same_identity_mutant"
cmp -s "$src" "$same_identity_mutant" && {
    echo 'FAIL: same-identity mutant was not applied' >&2
    exit 1
}
compile_and_expect_red same-identity "$same_identity_mutant" 6

runtime_revalidation_mutant="$tmp_root/runtime-revalidation.cpp"
sed '/bool DaemonSidecarAdapter::runtime_nodes_valid()/! s/runtime_nodes_valid()/true/g' \
    "$src" >"$runtime_revalidation_mutant"
cmp -s "$src" "$runtime_revalidation_mutant" && {
    echo 'FAIL: runtime-revalidation mutant was not applied' >&2
    exit 1
}
compile_and_expect_red runtime-revalidation "$runtime_revalidation_mutant" 14

inode_cleanup_mutant="$tmp_root/inode-cleanup.cpp"
sed '0,/info.st_ino != expected_inode/s//true/' \
    "$src" >"$inode_cleanup_mutant"
cmp -s "$src" "$inode_cleanup_mutant" && {
    echo 'FAIL: inode-cleanup mutant was not applied' >&2
    exit 1
}
compile_and_expect_red inode-cleanup "$inode_cleanup_mutant" 11

echo 'PASS: daemon sidecar adapter production guards and executable mutants hold'
