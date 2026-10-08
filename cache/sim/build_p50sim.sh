#!/bin/sh
set -eu

sim_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$sim_dir/../.." && pwd)
build_root=${ICECC_P50_BUILD_ROOT:-$root}
dep_root=${ICECC_P50_DEP_ROOT:-/tanksmall/MICKG2/mickg/src/mickg10/icecream-deps/ubuntu22-p50-root/usr}
cxx=${CXX:-g++}
cxxflags=${CXXFLAGS:-}
output="$sim_dir/.p50sim.bin"
temporary=$(mktemp "$sim_dir/.p50sim.bin.XXXXXX")
receipt="$sim_dir/.p50sim-build.json"
receipt_temporary=$(mktemp "$sim_dir/.p50sim-build.json.XXXXXX")
trap 'rm -f "$temporary" "$receipt_temporary"' EXIT HUP INT TERM

config_cppflags=
if [ -f "$build_root/config.h" ]; then
    config_cppflags="-include $build_root/config.h"
fi

# KNOWN TRAP (part 1): cache/Makefile and services/Makefile both assign
# "CXX = g++" unconditionally, so a $CXX environment variable never reaches
# these make invocations and any ICECC_P50SIM_* instrumentation macro
# silently never makes it into the compiled libraries (stage timers then
# read back as 0). CXX must be passed as a make COMMAND-LINE variable
# assignment, which takes the highest precedence and overrides the
# Makefile's own assignment; a plain exported/env var does not. When any
# extra flags are requested, also force an unconditional rebuild (-B): make
# only tracks file mtimes, not compiler flags, so a flag-only change would
# otherwise be silently skipped against a previously-built, now-stale .a
# compiled under different flags.
#
# KNOWN TRAP (part 2, discovered while building the guard below): both
# Makefiles are autoconf/automake-generated and ALSO plainly assign their
# own non-empty "CXXFLAGS = -g -Wall ... -std=c++23 -DNDEBUG ..." at
# configure time. Overriding CXXFLAGS wholesale via the make command line
# (the obvious fix for part 1) does deliver a requested macro, but it also
# clobbers that baked-in -std=c++23/-DNDEBUG/warning set -- which breaks
# compilation outright (observed: a std::variant/visit overload-resolution
# error in protocol50.cpp, unrelated to stage timers, caused purely by
# losing -std=c++23). The automake-generated compile rule
# (CXXCOMPILE = $(CXX) ... $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CXXFLAGS)
# $(CXXFLAGS)) places $(CPPFLAGS) BEFORE $(CXXFLAGS) and ADDS to it rather
# than replacing it, so extra -D macro defines must be delivered via
# CPPFLAGS (also plainly assigned as "CPPFLAGS = " i.e. empty, so it must
# still be a command-line arg, not just an env var -- but being empty by
# default, overriding it is additive, not destructive). CXXFLAGS itself is
# therefore left to the Makefile's own default in the two make invocations
# below; only CPPFLAGS carries $cxxflags into them.
make_force=
if [ -n "$cxxflags" ]; then
    make_force=-B
fi

if [ -f "$build_root/cache/Makefile" ] && [ -f "$build_root/services/Makefile" ]; then
    make -C "$build_root/cache" $make_force \
        CXX="$cxx" CPPFLAGS="$cxxflags" \
        libprotocol50.a libp50endpoint.a libp50adoptedoutcomewriter.a
    make -C "$build_root/services" $make_force \
        CXX="$cxx" CPPFLAGS="$cxxflags" \
        libicecc.la
    "$cxx" -std=c++23 -O2 -DBOOST_ERROR_CODE_HEADER_ONLY $cxxflags \
        -I"$root" -I"$build_root" -I"$dep_root/include" $config_cppflags \
        "$sim_dir/p50sim.cpp" \
        "$build_root/cache/libp50endpoint.a" \
        "$build_root/cache/libp50adoptedoutcomewriter.a" \
        "$build_root/cache/libprotocol50.a" \
        "$build_root/services/.libs/libicecc.a" \
        -L"$dep_root/lib/x86_64-linux-gnu" \
        -lzstd -lxxhash -llzo2 -pthread -o "$temporary"
else
    "$cxx" -std=c++23 -O2 -DBOOST_ERROR_CODE_HEADER_ONLY $cxxflags \
        -I"$root" -I"$build_root" -I"$dep_root/include" $config_cppflags \
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

# Guard: if a stage-timer macro was requested, prove it actually reached the
# compiled binary before this build is allowed to replace the published
# output/receipt. A fully synthetic, corpus-independent single-TU batch run
# is used so this guard never depends on (or touches) the real TU corpus.
case " $cxxflags " in
    *"ICECC_P50SIM_STAGE_TIMERS"*)
        selftest_dir=$(mktemp -d "$sim_dir/.p50sim-selftest.XXXXXX")
        trap 'rm -rf "$selftest_dir"; rm -f "$temporary" "$receipt_temporary"' EXIT HUP INT TERM
        printf 'int p50sim_build_selftest(void){return 0;}\n' > "$selftest_dir/tu.c"
        printf '%s\n' "$selftest_dir/tu.c" > "$selftest_dir/manifest.txt"
        printf 'cardinality=1\n0\n' > "$selftest_dir/map.txt"
        mkdir -p "$selftest_dir/fp"
        if ! ICECC_P50_PROFILE=P29V1 "$temporary" \
                --batch-manifest "$selftest_dir/manifest.txt" \
                --batch-assignment-map "$selftest_dir/map.txt" \
                --batch-output "$selftest_dir/out.jsonl" \
                --p29-fingerprint-cache-directory "$selftest_dir/fp" \
                > "$selftest_dir/out.log" 2>&1; then
            echo "p50sim: stage-timer self-test batch run failed:" >&2
            cat "$selftest_dir/out.log" >&2
            exit 1
        fi
        if ! python3 - "$selftest_dir/out.jsonl" <<'PYEOF'
import json
import sys

with open(sys.argv[1]) as handle:
    row = json.loads(handle.readline())
stages = row.get("p29v1_codec_stage_ns")
sys.exit(0 if stages and any(value != 0 for value in stages.values()) else 1)
PYEOF
        then
            echo "p50sim: ICECC_P50SIM_STAGE_TIMERS was requested in CXXFLAGS but every" >&2
            echo "p50sim: p29v1_codec_stage_ns value in the self-test output is zero -- the" >&2
            echo "p50sim: macro did not reach the compiled binary (see the KNOWN TRAP note" >&2
            echo "p50sim: in cache/sim/build_p50sim.sh: CXX/CXXFLAGS must be make" >&2
            echo "p50sim: command-line variable assignments, not just env/exported vars)." >&2
            exit 1
        fi
        rm -rf "$selftest_dir"
        trap 'rm -f "$temporary" "$receipt_temporary"' EXIT HUP INT TERM
        ;;
esac

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
compiler_path=$(command -v "$cxx" || printf '%s' "$cxx")
compiler_version=$($cxx --version 2>/dev/null | head -n 1 | tr '\n' ' ')
printf '{"schema":"icecream-p50sim-build-v1","source":{"root":"%s","head":"%s","tree":"%s","tracked_clean":true},"binary":{"path":"%s","sha256":"%s","bytes":%s},"inputs":{"build_script":%s,"p50sim_source":%s,"config_h":%s,"cache_makefile":%s,"services_makefile":%s},"configuration":{"make_mode":"%s","dependency_root":"%s","compiler_path":"%s","compiler_version":"%s"}}\n' \
    "$git_root" "$source_commit" "$source_tree" "$output" "$(sha256_file "$output")" "$(bytes_file "$output")" \
    "$(artifact_json "$sim_dir/build_p50sim.sh")" "$(artifact_json "$sim_dir/p50sim.cpp")" \
    "$(artifact_json "$build_root/config.h")" "$(artifact_json "$build_root/cache/Makefile")" \
    "$(artifact_json "$build_root/services/Makefile")" \
    "$(if [ -f "$build_root/cache/Makefile" ] && [ -f "$build_root/services/Makefile" ]; then printf product_make; else printf direct_sources; fi)" \
    "$dep_root" "$compiler_path" "$compiler_version" > "$receipt_temporary"
mv "$receipt_temporary" "$receipt"
trap - EXIT HUP INT TERM
