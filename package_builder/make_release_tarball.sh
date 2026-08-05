#!/usr/bin/env bash
# Produce THE release source artifact: one clean, bootstrapped tarball that
# every package builder consumes, so all four distributions build from
# byte-identical Source0 instead of each running its own autoreconf with its
# own tool versions.
#
#   make_release_tarball.sh <git-checkout> [outdir]
#
# Writes to <outdir> (default <checkout>/package_builder/dist):
#   icecream-<version>+g<rev>.tar.xz      the artifact
#   icecream-<version>+g<rev>.tar.xz.sha256
#   icecream-<version>+g<rev>.TOOLVERSIONS   bootstrap tool identities
#
# Builders pick the artifact up via RELEASE_TARBALL (see their compose
# files); without it they fall back to per-distro staging with a warning.
set -euo pipefail

SRC_GIT=${1:?git checkout}
OUT=${2:-$SRC_GIT/package_builder/dist}

GIT=(git -c "safe.directory=$SRC_GIT" -C "$SRC_GIT")
REV=$("${GIT[@]}" rev-parse HEAD)
SHORTREV=$("${GIT[@]}" rev-parse --short "$REV")

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

# The committed staging tool, from the resolved revision.
"${GIT[@]}" show "$REV:package_builder/make_source_tree.sh" > "$STAGE/mst.sh"
bash "$STAGE/mst.sh" "$SRC_GIT" "$STAGE/src" --bootstrap

VER=$(awk -F'[][]' '$2 == "icecream_version_major" {maj=$4} $2 == "icecream_version_minor" {min=$4} $2 == "icecream_version_micro" {mic=$4} END {printf "%s.%s%s%s", maj, min, mic==""?"":".", mic}' "$STAGE/src/configure.ac")
[ -n "$VER" ] || { echo "ERROR: cannot parse version from the exported configure.ac" >&2; exit 2; }

NAME="icecream-${VER}+g${SHORTREV}"
mv "$STAGE/src" "$STAGE/$NAME"
mkdir -p "$OUT"
tar -C "$STAGE" -cJf "$OUT/$NAME.tar.xz" "$NAME"
( cd "$OUT" && sha256sum "$NAME.tar.xz" > "$NAME.tar.xz.sha256" )
{
    echo "revision=$REV"
    echo "version=$VER"
    echo "autoconf=$(autoconf --version | head -1)"
    echo "automake=$(automake --version | head -1)"
    echo "libtool=$(libtoolize --version 2>/dev/null | head -1 || echo unavailable)"
    echo "host=$(uname -sr)"
} > "$OUT/$NAME.TOOLVERSIONS"

echo "release artifact: $OUT/$NAME.tar.xz"
cat "$OUT/$NAME.tar.xz.sha256"
