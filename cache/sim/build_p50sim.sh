#!/bin/sh
set -eu

sim_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$sim_dir/../.." && pwd)
build_root=${ICECC_P50_BUILD_ROOT:-$root}
dep_root=${ICECC_P50_DEP_ROOT:-/tanksmall/MICKG2/mickg/src/mickg10/icecream-deps/ubuntu22-p50-root/usr}
cxx=${CXX:-g++}
output="$sim_dir/.p50sim.bin"
temporary=$(mktemp "$sim_dir/.p50sim.bin.XXXXXX")
trap 'rm -f "$temporary"' EXIT HUP INT TERM

config_cppflags=
libbsc_cflags=
libbsc_libs=
if [ -f "$build_root/config.h" ]; then
    config_cppflags="-include $build_root/config.h"
    if grep -q '^#define ICECC_P50_WITH_LIBBSC 1$' "$build_root/config.h"; then
        if [ ! -f "$build_root/cache/Makefile" ]; then
            echo "p50sim: GRZ_RESIDUAL is enabled but its configured cache Makefile is missing" >&2
            exit 1
        fi
        libbsc_cflags=$(sed -n 's/^LIBBSC_CFLAGS = //p' "$build_root/cache/Makefile")
        libbsc_libs=$(sed -n 's/^LIBBSC_LIBS = //p' "$build_root/cache/Makefile")
        if [ -z "$libbsc_cflags" ] || [ -z "$libbsc_libs" ]; then
            echo "p50sim: GRZ_RESIDUAL is enabled but libbsc link flags are missing" >&2
            exit 1
        fi
    fi
fi

if [ -f "$build_root/cache/Makefile" ] && [ -f "$build_root/services/Makefile" ]; then
    make -C "$build_root/cache" \
        libprotocol50.a libp50endpoint.a libp50adoptedoutcomewriter.a
    make -C "$build_root/services" libicecc.la
    "$cxx" -std=c++23 -O2 -DBOOST_ERROR_CODE_HEADER_ONLY \
        -I"$root" -I"$build_root" -I"$dep_root/include" $config_cppflags \
        $libbsc_cflags \
        "$sim_dir/p50sim.cpp" \
        "$build_root/cache/libp50endpoint.a" \
        "$build_root/cache/libp50adoptedoutcomewriter.a" \
        "$build_root/cache/libprotocol50.a" \
        "$build_root/services/.libs/libicecc.a" \
        -L"$dep_root/lib/x86_64-linux-gnu" \
        -lzstd -lxxhash -llzo2 $libbsc_libs -pthread -o "$temporary"
else
    "$cxx" -std=c++23 -O2 -DBOOST_ERROR_CODE_HEADER_ONLY \
        -I"$root" -I"$build_root" -I"$dep_root/include" $config_cppflags \
        $libbsc_cflags \
        "$sim_dir/p50sim.cpp" "$root/cache/protocol50.cpp" \
        "$root/cache/p50_actions.cpp" "$root/cache/p50_zstd.cpp" \
        "$root/cache/p50_endpoint.cpp" "$root/cache/p50_input_record.cpp" \
        "$root/cache/p50_slice0.cpp" "$root/cache/p50_profile.cpp" \
        "$root/cache/p50_grz.cpp" \
        "$root/cache/p50_endpoint_run_cancel.cpp" \
        "$root/cache/p50_adopted_outcome_writer.cpp" \
        "$root/services/digest128.cpp" "$root/services/p50_cache_session_wire.cpp" \
        -L"$dep_root/lib/x86_64-linux-gnu" \
        -lzstd -lxxhash $libbsc_libs -pthread -o "$temporary"
fi
mv "$temporary" "$output"
trap - EXIT HUP INT TERM
