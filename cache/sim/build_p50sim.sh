#!/bin/sh
set -eu

sim_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$sim_dir/../.." && pwd)
build_root=${ICECC_P50_BUILD_ROOT:-$root}
dep_root=${ICECC_P50_DEP_ROOT:-/tanksmall/MICKG2/mickg/src/mickg10/icecream-deps/ubuntu22-p50-root/usr}
cxx=${CXX:-g++}
output="$sim_dir/.p50sim.bin"
temporary=$(mktemp "$sim_dir/.p50sim.bin.XXXXXX")
receipt="$sim_dir/.p50sim-build.json"
receipt_temporary=$(mktemp "$sim_dir/.p50sim-build.json.XXXXXX")
trap 'rm -f "$temporary" "$receipt_temporary"' EXIT HUP INT TERM

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

with_libbsc=0
if [ -n "$libbsc_libs" ]; then
    with_libbsc=1
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
git_root=$(git -C "$root" rev-parse --show-toplevel)
git_status=$(git -C "$git_root" status --porcelain --untracked-files=no)
if [ -n "$git_status" ]; then
    echo "p50sim: product source tree is tracked-dirty; refusing build receipt" >&2
    exit 1
fi
source_commit=$(git -C "$git_root" rev-parse HEAD)
source_tree=$(git -C "$git_root" rev-parse 'HEAD^{tree}')
sha256_file() { sha256sum "$1" | awk '{print $1}'; }
bytes_file() { wc -c < "$1" | tr -d ' '; }
artifact_json() {
    artifact=$1
    if [ -f "$artifact" ]; then
        printf '{"path":"%s","sha256":"%s","bytes":%s}' \
            "$artifact" "$(sha256_file "$artifact")" "$(bytes_file "$artifact")"
    else
        printf 'null'
    fi
}
libbsc_receipt=null
if [ "$with_libbsc" -eq 1 ]; then
    libbsc_root=/tanksmall/scratch/ictmp/libbsc-issue16
    libbsc_head=$(git -C "$libbsc_root" rev-parse HEAD)
    libbsc_tree=$(git -C "$libbsc_root" rev-parse 'HEAD^{tree}')
    libbsc_receipt=$(printf '{"source_root":"%s","head":"%s","tree":"%s","archive":{"sha256":"39edf31118aa546a0439a08e730a7bcab522f376fa9a715fc17a3add4c760ccf","materialized":false,"reproducible_from":"git archive --format=tar --prefix=libbsc-baffa62/ %s"},"archive_sha256":"39edf31118aa546a0439a08e730a7bcab522f376fa9a715fc17a3add4c760ccf","header":%s,"library":%s,"provenance":%s,"source_manifest":%s}' \
        "$libbsc_root" "$libbsc_head" "$libbsc_tree" \
        "$libbsc_head" \
        "$(artifact_json "$libbsc_root/libbsc/libbsc.h")" \
        "$(artifact_json "$libbsc_root/build-gcc2/libbsc.a")" \
        "$(artifact_json "$build_root/vendor/libbsc/PROVENANCE")" \
        "$(artifact_json "$build_root/vendor/libbsc/SOURCE-MANIFEST.sha256")")
fi
compiler_path=$(command -v "$cxx" || printf '%s' "$cxx")
compiler_version=$($cxx --version 2>/dev/null | head -n 1 | tr '\n' ' ')
printf '{"schema":"icecream-p50sim-build-v1","source":{"root":"%s","head":"%s","tree":"%s","tracked_clean":true},"binary":{"path":"%s","sha256":"%s","bytes":%s},"inputs":{"build_script":%s,"p50sim_source":%s,"config_h":%s,"cache_makefile":%s,"services_makefile":%s},"libbsc":%s,"configuration":{"with_libbsc":%s,"make_mode":"%s","dependency_root":"%s","compiler_path":"%s","compiler_version":"%s"}}\n' \
    "$git_root" "$source_commit" "$source_tree" "$output" "$(sha256_file "$output")" "$(bytes_file "$output")" \
    "$(artifact_json "$sim_dir/build_p50sim.sh")" "$(artifact_json "$sim_dir/p50sim.cpp")" \
    "$(artifact_json "$build_root/config.h")" "$(artifact_json "$build_root/cache/Makefile")" \
    "$(artifact_json "$build_root/services/Makefile")" "$libbsc_receipt" "$with_libbsc" \
    "$(if [ -f "$build_root/cache/Makefile" ] && [ -f "$build_root/services/Makefile" ]; then printf product_make; else printf direct_sources; fi)" \
    "$dep_root" "$compiler_path" "$compiler_version" > "$receipt_temporary"
mv "$receipt_temporary" "$receipt"
trap - EXIT HUP INT TERM
