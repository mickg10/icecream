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
#   zstd    linked statically; a system libzstd.a is fine.  Note the ambient trap
#           recorded elsewhere in this lane: a plain `-lzstd` against the distro shared
#           object loses zstd multithreading, which only shows up on blob-bearing corpora.
#
# Usage:  LIBBSC_DIR=~/libbsc LIBBSC_A=~/grouprlz/libbsc.a ./selector_build_codec50_sink.sh [outdir]
set -Eeuo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=${1:-$HERE/build}
LIBBSC_DIR=${LIBBSC_DIR:-$HOME/libbsc}
LIBBSC_A=${LIBBSC_A:-$HOME/grouprlz/libbsc.a}
LIBZSTD_A=${LIBZSTD_A:-/usr/lib/x86_64-linux-gnu/libzstd.a}

for f in "$LIBBSC_DIR/libbsc/libbsc.h" "$LIBBSC_A" "$LIBZSTD_A"; do
  [ -e "$f" ] || { echo "missing dependency: $f" >&2; exit 1; }
done
mkdir -p "$OUT"

# the exact command every measurement on this branch was built with.
# -fopenmp is REQUIRED, not optional: the recorded static libbsc is built with OpenMP, so
# omitting it fails the link with undefined GOMP_* references.  It has to be on BOTH flag
# sets -- a sanitizer build that cannot link is a gate that never runs.
CXXFLAGS_OPT="-O3 -march=native -std=c++17 -fopenmp -DWITH_BSC_GROUPS"
# and the sanitizer build used to verify the step-2b grow-on-demand change
CXXFLAGS_SAN="-O1 -g -fsanitize=address,undefined -std=c++17 -fopenmp -DWITH_BSC_GROUPS"

build() { # build <flags> <output>
  g++ $1 -I"$HERE" -I"$LIBBSC_DIR/libbsc" "$HERE/codec50-sink.cpp" -o "$2" \
      "$LIBBSC_A" "$LIBZSTD_A" -lz -lpthread
  echo "built $2"
}
build "$CXXFLAGS_OPT" "$OUT/codec50-sink"
[ "${WITH_SANITIZERS:-0}" = 1 ] && build "$CXXFLAGS_SAN" "$OUT/codec50-sink-asan"
exit 0
