#!/bin/sh
set -eu

sim_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$sim_dir/../.." && pwd)
dep_root=${ICECC_P50_DEP_ROOT:-/tanksmall/MICKG2/mickg/src/mickg10/icecream-deps/ubuntu22-p50-root/usr}
cxx=${CXX:-g++}
output="$sim_dir/.p50sim.bin"
temporary=$(mktemp "$sim_dir/.p50sim.bin.XXXXXX")
trap 'rm -f "$temporary"' EXIT HUP INT TERM

make -C "$root/cache" \
    libprotocol50.a libp50endpoint.a libp50adoptedoutcomewriter.a
make -C "$root/services" libicecc.la

"$cxx" -std=c++23 -O2 -DBOOST_ERROR_CODE_HEADER_ONLY \
    -I"$root" -I"$dep_root/include" \
    "$sim_dir/p50sim.cpp" \
    "$root/cache/libp50endpoint.a" \
    "$root/cache/libp50adoptedoutcomewriter.a" \
    "$root/cache/libprotocol50.a" \
    "$root/services/.libs/libicecc.a" \
    -L"$dep_root/lib/x86_64-linux-gnu" \
    -lzstd -lxxhash -llzo2 -pthread -o "$temporary"
mv "$temporary" "$output"
trap - EXIT HUP INT TERM
