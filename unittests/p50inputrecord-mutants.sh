#!/bin/sh
# Compiled behavioral mutants for two-phase retained-input publication.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
top_build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
libtool=${ICECC_TEST_LIBTOOL:-$top_build/libtool}
protocol_archive=${ICECC_TEST_PROTOCOL50_ARCHIVE:-$top_build/cache/libprotocol50.a}
services_la=${ICECC_TEST_SERVICES_LA:-$top_build/services/libicecc.la}
work=$(mktemp -d "${TMPDIR:-/tmp}/p50-input-record-mutants.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

for required in "$libtool" "$protocol_archive" "$services_la"; do
    test -f "$required"
done

mutate() {
    name=$1
    output=$2
    cp "$src/cache/p50_input_record.cpp" "$output"
    case "$name" in
    no-bucket-reservation)
        sed -i '/    records_\.reserve(max_records_);/d' "$output"
        ;;
    allocating-owner-insert)
        perl -0pi -e \
            's/    auto insertion = records_\.insert\(std::move\(prepared\.state_->node\)\);/    auto insertion = records_.emplace(key, candidate);/; s/    if \(!insertion\.inserted\)/    if (!insertion.second)/' \
            "$output"
        ;;
    record-cap-bypass)
        sed -i \
            's/if (records_\.size() >= max_records_)/if (false \&\& records_.size() >= max_records_)/' \
            "$output"
        ;;
    closed-job-reopen)
        perl -0pi -e \
            's/    if \(existing == records_\.end\(\)\)\n        return InputPublishResult::NotRetainedJobClosed;/    if (existing == records_.end())\n        return commit_prepared(std::move(prepared));/' \
            "$output"
        ;;
    *)
        echo "unknown InputRecord mutant: $name" >&2
        exit 1
        ;;
    esac
    if cmp -s "$src/cache/p50_input_record.cpp" "$output"; then
        echo "FAIL: $name mutation did not apply" >&2
        exit 1
    fi
}

compile_mutant() {
    source=$1
    output=$2
    # Compile the test and mutant source together so the allocation probe wraps
    # the exact owner insertion used by this binary.
    # shellcheck disable=SC2086
    "$libtool" --tag=CXX --mode=link "$cxx" "$standard" -O1 -g \
        -Wall -Wextra -Wpedantic -DHAVE_CONFIG_H \
        ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_LIBZSTD_CFLAGS:-} \
        ${ICECC_TEST_XXHASH_CFLAGS:-} \
        -I"$top_build" -I"$src" -I"$src/cache" -I"$src/services" \
        "$src/unittests/p50_input_record_test.cpp" "$source" \
        "$protocol_archive" "$services_la" \
        ${ICECC_TEST_LDFLAGS:-} ${ICECC_TEST_LIBZSTD_LIBS:-} \
        ${ICECC_TEST_XXHASH_LIBS:-} ${ICECC_TEST_LIBS:-} \
        -pthread -o "$output" >/dev/null
}

count=0
for name in no-bucket-reservation allocating-owner-insert record-cap-bypass \
    closed-job-reopen; do
    count=$((count + 1))
    source="$work/$name.cpp"
    binary="$work/$name"
    mutate "$name" "$source"
    if ! compile_mutant "$source" "$binary"; then
        echo "FAIL: $name did not compile; no semantic witness exists" >&2
        exit 1
    fi
    if timeout 30s "$binary" >"$work/$name.log" 2>&1; then
        echo "FAIL: InputRecord semantic mutant survived: $name" >&2
        tail -n 20 "$work/$name.log" >&2
        exit 1
    fi
    echo "ok - InputRecord semantic mutant red: $name"
done

test "$count" -eq 4
echo 'PASS: all 4 compiled InputRecord semantic mutants red'
