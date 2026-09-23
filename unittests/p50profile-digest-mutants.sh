#!/bin/sh
# Compiled witnesses for each retained profile's final raw-result digest gate.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
top_build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
libtool=${ICECC_TEST_LIBTOOL:-$top_build/libtool}
test_object=${ICECC_TEST_ENDPOINT_TEST_OBJECT:-$top_build/unittests/p50endpoint-p50_endpoint_test.o}
baseline=${ICECC_TEST_ENDPOINT_BINARY:-$top_build/unittests/p50endpoint}
reference_archive=${ICECC_TEST_REFERENCE_ARCHIVE:-$top_build/unittests/libp50reference.a}
endpoint_hooks=${ICECC_TEST_ENDPOINT_HOOKS_ARCHIVE:-$top_build/cache/libp50endpointtesthooks.a}
endpoint_archive=${ICECC_TEST_ENDPOINT_ARCHIVE:-$top_build/cache/libp50endpoint.a}
adopted_archive=${ICECC_TEST_ADOPTED_WRITER_ARCHIVE:-$top_build/cache/libp50adoptedoutcomewriter.a}
local_archive=${ICECC_TEST_LOCAL_TRANSPORT_ARCHIVE:-$top_build/cache/libp50localtransport.a}
protocol_archive=${ICECC_TEST_PROTOCOL50_ARCHIVE:-$top_build/cache/libprotocol50.a}
services_la=${ICECC_TEST_SERVICES_LA:-$top_build/services/libicecc.la}
work=$(mktemp -d "${TMPDIR:-/tmp}/p50-profile-digest-mutants.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

for required in "$libtool" "$test_object" "$baseline" "$reference_archive" \
    "$endpoint_hooks" \
    "$endpoint_archive" "$adopted_archive" "$local_archive" \
    "$protocol_archive" "$services_la"; do
    test -f "$required"
done

ICECC_P50_PROFILE_DIGEST_MUTANT_FOCUS=1 timeout 30s "$baseline" \
    >"$work/baseline.log" 2>&1

mutate() {
    name=$1
    output=$2
    case "$name" in
    p29v1)
        cp "$src/cache/p50_slice0.cpp" "$output"
        sed -i \
            's/digest128(result) != pending.begin.raw_digest/false/' \
            "$output"
        original="$src/cache/p50_slice0.cpp"
        ;;
    zstd-tu)
        cp "$src/cache/p50_zstd.cpp" "$output"
        sed -i \
            's/if (icecc::digest128(output) != begin.raw_digest)/if (false)/' \
            "$output"
        original="$src/cache/p50_zstd.cpp"
        ;;
    zstd-route)
        cp "$src/cache/p50_zstd.cpp" "$output"
        sed -i \
            's/if (icecc::digest128(result) != begin.raw_digest)/if (false)/' \
            "$output"
        original="$src/cache/p50_zstd.cpp"
        ;;
    *)
        echo "unknown profile digest mutant: $name" >&2
        exit 1
        ;;
    esac
    if cmp -s "$original" "$output"; then
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
        ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
        ${ICECC_TEST_LIBZSTD_CFLAGS:-} ${ICECC_TEST_XXHASH_CFLAGS:-} \
        -I"$top_build" -I"$src" -I"$src/cache" -I"$src/services" \
        -c "$source" -o "$object"
    # shellcheck disable=SC2086
    "$libtool" --tag=CXX --mode=link "$cxx" "$standard" -O0 -g \
        ${ICECC_TEST_LDFLAGS:-} ${ICECC_TEST_BOOST_LDFLAGS:-} \
        -pthread -o "$output" "$test_object" "$object" \
        "$reference_archive" "$endpoint_hooks" "$endpoint_archive" "$adopted_archive" \
        "$local_archive" "$protocol_archive" "$services_la" \
        ${ICECC_TEST_LIBZSTD_LIBS:-} ${ICECC_TEST_XXHASH_LIBS:-} \
        ${ICECC_TEST_LIBCAP_NG_LIBS:-} ${ICECC_TEST_BOOST_LIBS:-} \
        ${ICECC_TEST_LIBS:-} >/dev/null
}

count=0
for name in p29v1 zstd-tu zstd-route; do
    count=$((count + 1))
    source="$work/$name.cpp"
    object="$work/$name.o"
    binary="$work/$name"
    mutate "$name" "$source"
    if ! compile_mutant "$source" "$object" "$binary"; then
        echo "FAIL: $name did not compile; no digest witness exists" >&2
        exit 1
    fi
    if ICECC_P50_PROFILE_DIGEST_MUTANT_FOCUS=1 timeout 30s "$binary" \
        >"$work/$name.log" 2>&1; then
        echo "FAIL: profile raw-result digest mutant survived: $name" >&2
        tail -n 20 "$work/$name.log" >&2
        exit 1
    fi
    echo "ok - profile raw-result digest mutant red: $name"
done

test "$count" -eq 3
echo 'PASS: all 3 retained-profile raw-result digest mutants red'
