#!/bin/sh
# Compiled behavioral mutants for the typed post-P5CO endpoint seam.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
top_build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
libtool=${ICECC_TEST_LIBTOOL:-$top_build/libtool}
test_object=${ICECC_TEST_ENDPOINT_TEST_OBJECT:-$top_build/unittests/p50endpoint-p50_endpoint_test.o}
baseline=${ICECC_TEST_ENDPOINT_BINARY:-$top_build/unittests/p50endpoint}
reference_archive=${ICECC_TEST_REFERENCE_ARCHIVE:-$top_build/unittests/libp50reference.a}
adopted_archive=${ICECC_TEST_ADOPTED_WRITER_ARCHIVE:-$top_build/cache/libp50adoptedoutcomewriter.a}
local_archive=${ICECC_TEST_LOCAL_TRANSPORT_ARCHIVE:-$top_build/cache/libp50localtransport.a}
protocol_archive=${ICECC_TEST_PROTOCOL50_ARCHIVE:-$top_build/cache/libprotocol50.a}
input_record_object=${ICECC_TEST_INPUT_RECORD_OBJECT:-}
services_la=${ICECC_TEST_SERVICES_LA:-$top_build/services/libicecc.la}
work=$(mktemp -d "${TMPDIR:-/tmp}/p50-endpoint-mutants.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
run_cancel_object="$work/p50_endpoint_run_cancel.o"

for required in "$libtool" "$test_object" "$baseline" "$reference_archive" \
    "$adopted_archive" \
    "$local_archive" "$protocol_archive" "$services_la"; do
    test -f "$required"
done

ICECC_P50_ENDPOINT_MUTANT_FOCUS=1 timeout 30s "$baseline" \
    >"$work/baseline.log" 2>&1

# The endpoint delegates run cancellation to a separate production
# translation unit. Mutants replace only p50_endpoint.cpp, so compile the
# unchanged delegate once and link it into every witness.
# shellcheck disable=SC2086
"$cxx" "$standard" -O0 -g -Wall -Wextra -Wpedantic \
    -Wno-mismatched-new-delete -DHAVE_CONFIG_H \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
    -I"$top_build" -I"$src" -I"$src/cache" -I"$src/services" \
    -c "$src/cache/p50_endpoint_run_cancel.cpp" -o "$run_cancel_object"

mutate() {
    name=$1
    output=$2
    cp "$src/cache/p50_endpoint.cpp" "$output"
    case "$name" in
    typed-operation-binding)
        sed -i \
            's/impl_->allocate_session(exact_outcome.operation, exact_deadline)/impl_->allocate_session()/' \
            "$output"
        ;;
    claimed-c-binding)
        sed -i \
            's/if (expected_c_store_guid.has_value() &&/if (false \&\&/' \
            "$output"
        ;;
    live-operation-fence)
        perl -0pi -e \
            's/    if \(expected\.cache_session_operation != live\.cache_session_operation\)\n        throw StaleCompletion\(\);\n//' \
            "$output"
        ;;
    absolute-timer-arm)
        sed -i \
            's/if (!arm_absolute_deadline_timer(\*io, \*deadline, timer_error))/if (false)/' \
            "$output"
        ;;
    deadline-worker-wakeup)
        perl -0pi -e \
            's/                if \(auto materialization = io->materialization\.lock\(\)\) \{\n                    cancel_materialization_notification\(materialization\);\n                \}\n//' \
            "$output"
        ;;
    owner-thread-codec)
        sed -i \
            's/asio::post(codec_pool.executor(),/asio::post(owner_executor,/' \
            "$output"
        ;;
    worker-completion-wakeup)
        perl -0pi -e \
            's/(                       state->completion\.emplace\(std::move\(completion\)\);\n                   \}\n)                   signal_notification_fd\(state->worker_notification\.get\(\)\);\n/$1/' \
            "$output"
        ;;
    codec-queue-bound)
        sed -i \
            's/if (!codec_pool.try_acquire())/if (false)/' \
            "$output"
        ;;
    codec-slot-release)
        sed -i \
            's/EndpointCodecSlotGuard slot{codec_pool_owner};/(void)codec_pool_owner;/' \
            "$output"
        ;;
    post-selector-deadline)
        perl -0pi -e \
            's/(            impl_->select_materialized_job_state\(session, materialized\);\n        \/\/ The selector is a product callback and may run for arbitrarily long\.\n        \/\/ Sample the original absolute deadline again after it returns and\n        \/\/ immediately before the allocation-free owner publication seam\.\n)        require_operation\(\);\n/$1/' \
            "$output"
        ;;
    post-transfer-fence)
        sed -i \
            's/} lease_failure_fence{lease.get()};/} lease_failure_fence{nullptr};/' \
            "$output"
        ;;
    client-completion-deadline)
        perl -0pi -e \
            's/(        require_live_completion\(expected, live\);\n)        require_deadline\(\);\n/$1/' \
            "$output"
        ;;
    server-completion-final-recheck)
        perl -0pi -e \
            's/(        require_live_completion\(expected, live\);\n        \/\/ Both completion hooks are arbitrary owner-affine product callbacks\.\n        \/\/ They may cross the absolute deadline or synchronously request\n        \/\/ cancellation, so their identity observation is not the final\n        \/\/ operation\/deadline observation for this completion\.\n)        require_operation\(\);\n/$1/' \
            "$output"
        ;;
    *)
        echo "unknown endpoint mutant: $name" >&2
        exit 1
        ;;
    esac
    if cmp -s "$src/cache/p50_endpoint.cpp" "$output"; then
        echo "FAIL: $name mutation did not apply" >&2
        exit 1
    fi
}

compile_mutant() {
    source=$1
    object=$2
    output=$3
    # shellcheck disable=SC2086
    "$cxx" "$standard" -O0 -g -Wall -Wextra -Wpedantic \
        -Wno-mismatched-new-delete -DHAVE_CONFIG_H \
        -DICECC_P50_ENDPOINT_TEST_HOOKS \
        ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
        ${ICECC_TEST_LIBZSTD_CFLAGS:-} \
        ${ICECC_TEST_XXHASH_CFLAGS:-} \
        -I"$top_build" -I"$src" -I"$src/cache" -I"$src/services" \
        -c "$source" -o "$object"
    # shellcheck disable=SC2086
    "$libtool" --tag=CXX --mode=link "$cxx" "$standard" -O0 -g \
        ${ICECC_TEST_LDFLAGS:-} ${ICECC_TEST_BOOST_LDFLAGS:-} \
        -pthread -o "$output" "$test_object" "$object" \
        "$run_cancel_object" $input_record_object "$reference_archive" \
        "$adopted_archive" "$local_archive" \
        "$protocol_archive" "$services_la" \
        ${ICECC_TEST_LIBZSTD_LIBS:-} \
        ${ICECC_TEST_XXHASH_LIBS:-} ${ICECC_TEST_LIBCAP_NG_LIBS:-} \
        ${ICECC_TEST_BOOST_LIBS:-} ${ICECC_TEST_LIBS:-} >/dev/null
}

count=0
for name in typed-operation-binding claimed-c-binding live-operation-fence \
    absolute-timer-arm deadline-worker-wakeup owner-thread-codec \
    worker-completion-wakeup codec-queue-bound codec-slot-release \
    post-selector-deadline post-transfer-fence client-completion-deadline \
    server-completion-final-recheck; do
    count=$((count + 1))
    source="$work/$name.cpp"
    object="$work/$name.o"
    binary="$work/$name"
    mutate "$name" "$source"
    if ! compile_mutant "$source" "$object" "$binary"; then
        echo "FAIL: $name did not compile; no semantic witness exists" >&2
        exit 1
    fi
    if ICECC_P50_ENDPOINT_MUTANT_FOCUS=1 timeout 20s "$binary" \
        >"$work/$name.log" 2>&1; then
        echo "FAIL: endpoint semantic mutant survived: $name" >&2
        tail -n 20 "$work/$name.log" >&2
        exit 1
    fi
    echo "ok - endpoint semantic mutant red: $name"
done

test "$count" -eq 13
echo 'PASS: all 13 compiled typed-endpoint semantic mutants red'
