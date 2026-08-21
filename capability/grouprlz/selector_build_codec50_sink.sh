#!/usr/bin/env bash
# Build codec50-sink from THIS directory, so every result on this branch is reproducible
# from the branch rather than from a binary that happens to sit on one machine.
#
# Sources in this directory:
#   codec50-sink.cpp        the codec (P29 + physical two-direction wire sinks)
#   alpha_line_codec.h      md5 d4459e0a7d64b93875f2dd794c9a0c6f
#   mo_factor_codec.h       md5 f4a5050b617d92925a7bd06b27d86f72
#   residual_group_codec.h  md5 a5bd5811f6163a7f223ec69d0f27389b
#
# External, NOT vendored here:
#   libbsc  https://github.com/IlyaGrebnov/libbsc  v3.3.12, commit baffa62
#           ("Merge pull request #14 from vgaetera/fix-build-warning").
#           Needs libbsc.h on the include path and libbsc.a to link.
#   zstd    linked statically with worker support.  Note the ambient trap recorded elsewhere
#           in this lane: some distro static/shared builds accept the API but return
#           `Unsupported parameter` only after a blob-bearing corpus requests workers.
#
# Usage:  LIBBSC_DIR=~/libbsc LIBBSC_A=~/grouprlz/libbsc.a ./selector_build_codec50_sink.sh [outdir]
set -Eeuo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=${1:-$HERE/build}
LIBBSC_DIR=${LIBBSC_DIR:-$HOME/libbsc}
LIBBSC_A=${LIBBSC_A:-$HOME/grouprlz/libbsc.a}
ZSTD_MT_CANDIDATE=$HOME/gdict/zstd/zstd-1.4.8/lib/libzstd.a
if [ -f "$ZSTD_MT_CANDIDATE" ]; then
  ZSTD_DEFAULT=$ZSTD_MT_CANDIDATE
else
  ZSTD_DEFAULT=/usr/lib/x86_64-linux-gnu/libzstd.a
fi
LIBZSTD_A=${LIBZSTD_A:-$ZSTD_DEFAULT}
# The 2^21 source default is a small-corpus development setting.  The published breadth
# measurements already established 2^24 as output-neutral and large enough for Firefox/Godot;
# make the reproducible measurement build use that capacity unless a larger run overrides it.
LINE_CAP_LOG2=${ICE_LINE_CAP_LOG2:-24}

for f in "$LIBBSC_DIR/libbsc/libbsc.h" "$LIBBSC_A" "$LIBZSTD_A"; do
  [ -e "$f" ] || { echo "missing dependency: $f" >&2; exit 1; }
done
mkdir -p "$OUT"

# the exact command every measurement on this branch was built with.
# -fopenmp is REQUIRED, not optional: the recorded static libbsc is built with OpenMP, so
# omitting it fails the link with undefined GOMP_* references.  It has to be on BOTH flag
# sets -- a sanitizer build that cannot link is a gate that never runs.
#
# -Werror=format is here because this codec writes its evidence with printf: a TSV column
# added to the header but not to the format string produced a silently EMPTY column, and
# separately a format slot with no argument once printed stack garbage as a byte count.  Both
# are compile-time detectable, so they are now compile-time errors.
CXXFLAGS_OPT="-O3 -march=native -std=c++17 -fopenmp -Wformat=2 -Werror=format -DWITH_BSC_GROUPS -DICE_LINE_CAP_LOG2=$LINE_CAP_LOG2"
# and the sanitizer build used to verify the step-2b grow-on-demand change
CXXFLAGS_SAN="-O1 -g -fsanitize=address,undefined -std=c++17 -fopenmp -Wformat=2 -Werror=format -DWITH_BSC_GROUPS -DICE_LINE_CAP_LOG2=$LINE_CAP_LOG2"

build() { # build <flags> <output>
  g++ $1 -I"$HERE" -I"$LIBBSC_DIR/libbsc" "$HERE/codec50-sink.cpp" -o "$2" \
      "$LIBBSC_A" "$LIBZSTD_A" -lz -lpthread
  echo "built $2"
}
build "$CXXFLAGS_OPT" "$OUT/codec50-sink"
if ! "$OUT/codec50-sink" --selftest-zstd-workers; then
  echo "selected libzstd lacks worker support; set LIBZSTD_A to an MT-enabled static build" >&2
  exit 1
fi
[ "${WITH_SANITIZERS:-0}" = 1 ] && build "$CXXFLAGS_SAN" "$OUT/codec50-sink-asan"
exit 0
