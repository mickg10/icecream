#!/bin/sh
# Focused standalone ASan/UBSan/LSan gate.  It deliberately compiles only
# protocol, digest, InputRecordStore, and the attachment core.
set -eu

root=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${TMPDIR:-/tmp}/icecc-p50-input-attachment-sanitize
rm -rf "$build"
mkdir -p "$build"

: "${CXX:=g++}"
flags="-std=c++23 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined,leak"
XXHASH_CFLAGS=${XXHASH_CFLAGS:-}
XXHASH_LIBS=${XXHASH_LIBS:--lxxhash}
libs="-lzstd $XXHASH_LIBS"
"$CXX" $flags -I"$root" -I"$root/services" \
    $XXHASH_CFLAGS \
    "$root/unittests/p50_input_attachment_test.cpp" \
    "$root/cache/p50_input_attachment.cpp" \
    "$root/cache/p50_input_record.cpp" \
    "$root/cache/protocol50.cpp" "$root/services/digest128.cpp" \
    -o "$build/p50_input_attachment_test" $libs
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1} \
    "$build/p50_input_attachment_test"
echo 'PASS: p50 input attachment ASan/UBSan/LSan gate passed'
