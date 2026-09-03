#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
top_build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
archive="$top_build/services/.libs/libicecc.a"
build=$(mktemp -d "${TMPDIR:-/tmp}/p50-claim-wire-mutants.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

test -f "$archive"

reset_tree() {
    rm -rf "$build/tree"
    mkdir -p "$build/tree/services" "$build/tree/unittests"
    cp "$src/services/comm.h" "$src/services/comm.cpp" \
        "$src/services/p50_cache_session_wire.h" \
        "$src/services/p50_cache_session_wire.cpp" "$build/tree/services/"
    cp "$src/unittests/p50_cache_session_wire_test.cpp" \
        "$build/tree/unittests/"
}

compile_mutant() {
    output=$1
    # shellcheck disable=SC2086
    "$cxx" "$standard" -O0 -g -Wall -Wextra -Wpedantic -Werror \
        ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_LIBCAP_NG_CFLAGS:-} \
        -I"$build/tree" -I"$build/tree/services" -I"$src" \
        -I"$src/services" -I"$top_build" \
        "$build/tree/unittests/p50_cache_session_wire_test.cpp" \
        "$build/tree/services/comm.cpp" \
        "$build/tree/services/p50_cache_session_wire.cpp" \
        "$archive" ${ICECC_TEST_LDFLAGS:-} \
        ${ICECC_TEST_LIBCAP_NG_LIBS:-} \
        ${ICECC_TEST_LIBS:--llzo2} \
        ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} \
        ${ICECC_TEST_XXHASH_LIBS:--lxxhash} -ldl -pthread \
        -o "$output"
}

require_red() {
    name=$1
    file=$2
    expression=$3
    reset_tree
    target="$build/tree/$file"
    before="$build/$name.before"
    cp "$target" "$before"
    sed "$expression" "$before" >"$target"
    if cmp -s "$before" "$target"; then
        echo "FAIL: $name mutation did not apply" >&2
        exit 1
    fi
    if ! compile_mutant "$build/$name" >"$build/$name.compile" 2>&1; then
        cat "$build/$name.compile" >&2
        echo "FAIL: $name did not compile; not a semantic red witness" >&2
        exit 1
    fi
    if "$build/$name" >"$build/$name.run" 2>&1; then
        echo "FAIL: semantic deletion mutant survived: $name" >&2
        exit 1
    fi
}

require_red entropy_short_read services/comm.cpp \
    's/result == static_cast<ssize_t>(capability.bytes.size())/result > 0/'
require_red entropy_distinctness services/comm.cpp \
    's/if (capability_1 != capability_2)/if (capability_1 == capability_2)/'
require_red selected_capability services/comm.cpp \
    's/claim->attempt.selected_capability != attempt_capability/false/'
require_red server_exact_echo services/comm.cpp \
    's/outcome->canonical_claim == ticket.canonical_claim_/true/'
require_red barrier_before_release services/comm.cpp \
    's/ticket.outcome_frame_sequence_ != 0 &&/true \&\&/'
require_red kernel_clean_boundary services/comm.cpp \
    's/if (result >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))/if (result < 0 \&\& false)/'
require_red client_claim_singularity services/comm.cpp \
    's/if (client_claim_pending ||/if ((client_claim_pending \&\& false) ||/'
require_red client_exact_echo services/comm.cpp \
    's/outcome->canonical_claim != p50_outbound_claim/false/'
require_red client_operation_sequence services/comm.cpp \
    's/outcome->operation.operation_sequence !=/0 ==/'
require_red adopted_launch_binding services/p50_cache_session_wire.cpp \
    's/f_sidecar_launch == claim->binding.f_control_identity()/true/'
require_red adopted_store_binding services/p50_cache_session_wire.cpp \
    's/f_store_guid == claim->binding.f_store_guid/true/'
require_red refused_exact_launch services/p50_cache_session_wire.cpp \
    's/f_sidecar_launch == P50WireLaunchIdentity{}/!f_sidecar_launch.valid()/'
require_red refused_exact_operation services/p50_cache_session_wire.cpp \
    's/operation == P50FSessionOperationId{}/!operation.valid()/'
require_red refusal_reason_range services/p50_cache_session_wire.cpp \
    's/refusal_reason_valid(\*refusal_reason)/(refusal_reason_valid(*refusal_reason) || true)/'
require_red claim_exact_protocol services/comm.h \
    '/class P50CacheSessionClaimMsg/,/class P50CacheSessionOutcomeMsg/ s/return negotiated_protocol == PROTOCOL_VERSION;/return negotiated_protocol >= PROTOCOL_VERSION;/'
require_red outcome_exact_protocol services/comm.h \
    '/class P50CacheSessionOutcomeMsg/,/struct P50SourceArmFields/ s/return negotiated_protocol == PROTOCOL_VERSION;/return negotiated_protocol >= PROTOCOL_VERSION;/'

echo 'ok - P50 claim/outcome foundation semantic mutants red'
