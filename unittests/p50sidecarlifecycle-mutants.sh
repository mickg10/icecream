#!/bin/sh
set -eu
root="${ICECC_TEST_TOP_SRCDIR:-$(pwd)/..}"
build="${ICECC_TEST_BUILDDIR:-$(pwd)}"
cxx="${ICECC_TEST_CXX:-c++}"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/p50-sidecar-lifecycle-mutant.XXXXXX")"
trap 'rm -r -- "$tmp"' EXIT HUP INT TERM
for mutant_name in pgid store-generation; do
    cp "$root/cache/p50_sidecar_lifecycle.cpp" "$tmp/mutant.cpp"
    # These are semantic deletion mutants; the runtime witness must redden
    # each one rather than merely matching source text.
    if test "$mutant_name" = pgid; then
        sed -i 's/observation\.observed_pgid == process_group_/true/' "$tmp/mutant.cpp"
    else
        sed -i 's/observation\.store_generation == identity_->store_generation/true/' "$tmp/mutant.cpp"
    fi
    "$cxx" -std=c++20 -I"$root" -I"$root/cache" -I"$root/services" ${ICECC_TEST_CXXFLAGS:-} \
        -c "$tmp/mutant.cpp" -o "$tmp/mutant.o"
    "$cxx" -std=c++20 -pthread -I"$root" -I"$root/services" \
        "$root/unittests/p50_sidecar_lifecycle_test.cpp" "$tmp/mutant.o" \
        "$build/../cache/libp50sidecarsupervisor.a" "$build/../cache/libp50localtransport.a" \
        "$build/../services/.libs/libicecc.a" \
        -L"${P50_R2_DEPS:-/tanksmall/MICKG2/mickg/src/mickg10/icecream-worktrees/.p50-r2-deps/root/usr/lib/x86_64-linux-gnu}" \
        -lxxhash ${ICECC_TEST_LIBS:-} -o "$tmp/mutant"
    if "$tmp/mutant" >/dev/null 2>&1; then
        echo "$mutant_name lifecycle mutant survived" >&2
        exit 1
    fi
done
