#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
fail() { echo "FAIL: $*" >&2; exit 1; }
grep -q 'LIBBSC_BLOCKSORTER_BWT' "$root/cache/p50_grz_residual_codec.h" || fail 'GRZ residual does not bind BWT'
grep -q 'bsc_compress' "$root/cache/p50_grz_residual_codec.h" || fail 'GRZ residual does not call libbsc'
grep -q 'ICECC_P50_WITH_LIBBSC' "$root/cache/p50_grz.cpp" || fail 'GRZ implementation is not dependency gated'
grep -q 'CACHE_PROFILE_GRZ' "$root/services/comm.h" || fail 'GRZ registry bit missing'
grep -q 'GRZ_RESIDUAL disabled' "$root/configure.ac" || fail 'default GRZ withdrawal is not configured'
grep -q 'grz_codec.encode' "$root/cache/p50_endpoint.cpp" || fail 'endpoint preparation does not select GRZ'
grep -q 'validate_grz_residual_begin' "$root/cache/p50_endpoint.cpp" || fail 'endpoint authority does not validate GRZ'
grep -q 'ProfileId::GRZ' "$root/client/p50_compile_binding.cpp" || fail 'compile binding does not carry GRZ'
grep -q 'P50_SOURCE_MODE_GRZ_RESIDUAL' "$root/client/remote.cpp" || fail 'source arm does not carry GRZ mode'
grep -q 'ProfileId::GRZ' "$root/client/p50_zstd_sender.cpp" || fail 'sender does not bind GRZ lifetime'
grep -q 'value == "GRZ" || value == "GRZ_RESIDUAL"' "$root/cache/sim/p50sim.cpp" || fail 'simulator does not recognize GRZ environment selection'
grep -q 'GRZ_RESIDUAL requires a simulator built with --with-libbsc' "$root/cache/sim/p50sim.cpp" || fail 'simulator does not fail closed without libbsc'
grep -q 'ICECC_P50_BUILD_ROOT' "$root/cache/sim/build_p50sim.sh" || fail 'simulator build does not support an out-of-tree product build'
grep -q 'libbsc_cflags' "$root/cache/sim/build_p50sim.sh" || fail 'simulator build does not consume configured libbsc flags'
test "$(git -C "$root" hash-object "$root/vendor/grouprlz/grz2.cpp")" = fa63c7b8ce40a1366b6ba4dc65a59839bfaa7419 || fail 'canonical grz2.cpp source mismatch'
test "$(git -C "$root" hash-object "$root/vendor/grouprlz/grz2g.cpp")" = 7f57775694b8962d3d92f38ce7e3213842139bc2 || fail 'canonical grz2g.cpp source mismatch'
if grep -R -n 'GRZR' "$root/cache/p50_grz"*; then fail 'toy GRZR framing remains'; fi
echo 'PASS GRZ_RESIDUAL source binding, BWT dependency gate, and no toy framing'
