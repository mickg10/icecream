#!/bin/sh
# Focused standalone ASan/UBSan/LSan gate.  It deliberately compiles only
# protocol, digest, InputRecordStore, and the attachment core.
set -eu

root=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=$(mktemp -d "${TMPDIR:-/tmp}/icecc-p50-input-attachment-sanitize.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

cxx=${CXX:-${ICECC_TEST_CXX:-g++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
cppflags=${ICECC_TEST_CPPFLAGS:-}
ldflags=${ICECC_TEST_LDFLAGS:-}
xxhash_cflags=${XXHASH_CFLAGS:-${ICECC_TEST_XXHASH_CFLAGS:-}}
xxhash_libs=${XXHASH_LIBS:-${ICECC_TEST_XXHASH_LIBS:--lxxhash}}
zstd_libs=${ICECC_TEST_LIBZSTD_LIBS:--lzstd}
"$cxx" "$standard" -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined,leak -DP50_ATTACHMENT_TEST_SEAMS \
    $cppflags $xxhash_cflags -I"$root" -I"$root/services" \
    "$root/unittests/p50_input_attachment_test.cpp" \
    "$root/cache/p50_input_attachment.cpp" \
    "$root/cache/p50_input_record.cpp" \
    "$root/cache/protocol50.cpp" "$root/services/digest128.cpp" \
    $ldflags -o "$build/p50_input_attachment_test" $zstd_libs $xxhash_libs
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1} \
    "$build/p50_input_attachment_test"
echo 'PASS: p50 input attachment ASan/UBSan/LSan gate passed'
