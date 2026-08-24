#!/bin/sh
set -eu

root=${ICECC_TEST_TOP_SRCDIR:?}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
work=$(mktemp -d "${TMPDIR:-/tmp}/p50-input-fd-sanitize.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

"$cxx" "$standard" -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread \
    -fsanitize=address,undefined,leak -fno-omit-frame-pointer \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_LIBZSTD_CFLAGS:-} \
    -I"$root" -I"$root/cache" -I"$root/services" \
    -c "$root/cache/p50_input_fd_attachment.cpp" -o "$work/attachment.o"

echo 'PASS: p50 input FD attachment ASan/UBSan/LSan compile gate'
