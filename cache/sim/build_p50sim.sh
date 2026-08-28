#!/bin/sh
set -eu

sim_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$sim_dir/../.." && pwd)
dep_root=${ICECC_P50_DEP_ROOT:-/tanksmall/MICKG2/mickg/src/mickg10/icecream-deps/ubuntu22-p50-root/usr}
cxx=${CXX:-g++}
output="$sim_dir/.p50sim.bin"
temporary=$(mktemp "$sim_dir/.p50sim.bin.XXXXXX")
trap 'rm -f "$temporary"' EXIT HUP INT TERM

if [ -f "$root/cache/Makefile" ] && [ -f "$root/services/Makefile" ]; then
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
else
    "$cxx" -std=c++23 -O2 -DBOOST_ERROR_CODE_HEADER_ONLY \
        -I"$root" -I"$dep_root/include" \
        "$sim_dir/p50sim.cpp" "$root/cache/protocol50.cpp" \
        "$root/cache/p50_actions.cpp" "$root/cache/p50_zstd.cpp" \
        "$root/cache/p50_endpoint.cpp" "$root/cache/p50_input_record.cpp" \
        "$root/cache/p50_slice0.cpp" "$root/cache/p50_profile.cpp" \
        "$root/cache/p50_endpoint_run_cancel.cpp" \
        "$root/cache/p50_adopted_outcome_writer.cpp" \
        "$root/services/digest128.cpp" "$root/services/p50_cache_session_wire.cpp" \
        -L"$dep_root/lib/x86_64-linux-gnu" \
        -lzstd -lxxhash -pthread -o "$temporary"
fi
mv "$temporary" "$output"
trap - EXIT HUP INT TERM
