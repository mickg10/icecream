#!/bin/sh
set -eu

cxx=${ICECC_TEST_CXX:-c++}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++20}
out=${TMPDIR:-/tmp}/p50-daemon-sidecar-adapter-sanitize.$$
trap 'rm -f "$out" "$out.o"' EXIT HUP INT TERM

"$cxx" "$standard" -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"${srcdir:-$(dirname "$0")}/.." -I"${srcdir:-$(dirname "$0")}/../services" \
    -c "${srcdir:-$(dirname "$0")}/../cache/p50_daemon_sidecar_adapter.cpp" \
    -o "$out.o"
echo 'p50 daemon sidecar adapter sanitizer compile: ok'
