#!/bin/sh
set -eu

sim_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$sim_dir/../.." && pwd)
deps=/tanksmall/MICKG2/mickg/miniconda3/envs/cppdeps/include
arrow=/tanksmall/MICKG2/mickg/miniconda3/envs/mta/lib/python3.11/site-packages/pyarrow/include

exec g++ -std=c++23 -O2 -DBOOST_ERROR_CODE_HEADER_ONLY \
    -I"$root" -I"$deps" -I"$arrow" -I"$arrow/arrow/vendored" \
    "$sim_dir/p50sim.cpp" "$root/cache/protocol50.cpp" \
    "$root/cache/p50_actions.cpp" "$root/cache/p50_zstd.cpp" \
    "$root/cache/p50_endpoint.cpp" "$root/cache/p50_input_record.cpp" \
    "$root/cache/p50_slice0.cpp" "$root/services/digest128.cpp" \
    -lzstd -Wl,-l:libxxhash.so.0 -pthread -o "$sim_dir/.p50sim.bin"
