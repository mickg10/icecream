#!/bin/sh
# Behavioral mutation gate for lifecycle cases that source greps cannot prove.
set -eu

root=${ICECC_TEST_TOP_SRCDIR:?}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
cppflags=${ICECC_TEST_CPPFLAGS:-}
ldflags=${ICECC_TEST_LDFLAGS:-}
xxhash_cflags=${ICECC_TEST_XXHASH_CFLAGS:-}
xxhash_libs=${ICECC_TEST_XXHASH_LIBS:--lxxhash}
zstd_libs=${ICECC_TEST_LIBZSTD_LIBS:--lzstd}
work=$(mktemp -d "${TMPDIR:-/tmp}/p50-attachment-mutants.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

reset_sources() {
    cp "$root/unittests/support/p50_input_attachment.cpp" "$work/p50_input_attachment.cpp"
    cp "$root/cache/p50_input_record.cpp" "$work/p50_input_record.cpp"
}

compile_mutant() {
    "$cxx" "$standard" -O1 -g -Wall -Wextra -Wpedantic -Werror \
        -DP50_ATTACHMENT_TEST_SEAMS $cppflags $xxhash_cflags \
        -I"$root" -I"$root/cache" -I"$root/services" \
        "$root/unittests/p50_input_attachment_test.cpp" \
        "$work/p50_input_attachment.cpp" "$work/p50_input_record.cpp" \
        "$root/cache/protocol50.cpp" "$root/services/digest128.cpp" \
        $ldflags -o "$work/test" $zstd_libs $xxhash_libs
}

expect_red() {
    label=$1
    compile_mutant
    if "$work/test" >"$work/$label.log" 2>&1; then
        echo "FAIL: $label mutant survived" >&2
        exit 1
    fi
    echo "ok - $label mutant reddened the attachment matrix"
}

reset_sources
test "$(grep -F -c 'state.ready_pending = true;' "$work/p50_input_attachment.cpp")" -eq 1
sed -i 's/state.ready_pending = true;/state.ready_pending = false;/' \
    "$work/p50_input_attachment.cpp"
expect_red precommit_ready

reset_sources
test "$(grep -F -c 'existing_owner_for(key, owner)' "$work/p50_input_attachment.cpp")" -eq 2
sed -i 's/existing_owner_for(key, owner)/owner_for(key, owner)/g' \
    "$work/p50_input_attachment.cpp"
expect_red inserting_closed_callback

reset_sources
test "$(grep -F -c 'position.second.reply.event_id == reply.event_id' "$work/p50_input_attachment.cpp")" -eq 1
sed -i 's/position.second.reply.event_id == reply.event_id/false \&\& position.second.reply.event_id == reply.event_id/' \
    "$work/p50_input_attachment.cpp"
expect_red replay_ack_propagation

reset_sources
test "$(grep -F -c 'entry.begin != begin || entry.commit != commit ||' "$work/p50_input_record.cpp")" -eq 1
sed -i 's/entry.begin != begin || entry.commit != commit ||/((void)commit, false) ||/' \
    "$work/p50_input_record.cpp"
expect_red canonical_transaction_metadata

echo 'PASS: p50 attachment behavioral mutants all red'
