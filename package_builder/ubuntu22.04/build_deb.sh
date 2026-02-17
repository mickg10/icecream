#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="${SRC_DIR:-/src}"
OUT_DIR="${OUT_DIR:-/out}"
WORK_DIR="${WORK_DIR:-/work}"
DEB_DIST="${DEB_DIST:-jammy}"
DEB_EMAIL="${DEB_EMAIL:-icecream-builder@example.invalid}"
DEB_NAME="${DEB_NAME:-icecream builder}"

export DEBIAN_FRONTEND=noninteractive
export DEBEMAIL="$DEB_EMAIL"
export DEBFULLNAME="$DEB_NAME"

mkdir -p "$WORK_DIR" "$OUT_DIR"
cd "$WORK_DIR"

enable_deb_src() {
    if ls /etc/apt/sources.list.d/*.sources >/dev/null 2>&1; then
        sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/*.sources || true
    fi

    if [ -f /etc/apt/sources.list ]; then
        if ! grep -Rqs '^deb-src ' /etc/apt/sources.list /etc/apt/sources.list.d 2>/dev/null; then
            awk '/^deb /{print "deb-src " substr($0,5)}' /etc/apt/sources.list > /etc/apt/sources.list.d/deb-src.list
        fi
    fi
}

parse_upstream_version() {
    local major minor micro
    major="$(sed -n 's/^m4_define(\\[icecream_version_major\\],\\[\\([0-9][0-9]*\\)\\]).*/\\1/p' "$1" | head -n1)"
    minor="$(sed -n 's/^m4_define(\\[icecream_version_minor\\],\\[\\([0-9][0-9]*\\)\\]).*/\\1/p' "$1" | head -n1)"
    micro="$(sed -n 's/^m4_define(\\[icecream_version_micro\\],\\[\\([0-9][0-9]*\\)\\]).*/\\1/p' "$1" | head -n1)"
    if [ -z "${major:-}" ] || [ -z "${minor:-}" ]; then
        echo "ERROR: unable to parse version from $1" >&2
        return 1
    fi
    if [ -n "${micro:-}" ]; then
        echo "${major}.${minor}.${micro}"
    else
        echo "${major}.${minor}"
    fi
}

apt-get update
enable_deb_src
apt-get update

apt-get install -y --no-install-recommends \
    ca-certificates \
    devscripts \
    dpkg-dev \
    equivs \
    fakeroot \
    rsync \
    build-essential

apt-get build-dep -y icecc

UPSTREAM_VERSION="$(parse_upstream_version "$SRC_DIR/configure.ac")"
DEB_VERSION="${UPSTREAM_VERSION}-0obs1~${DEB_DIST}1"

rm -rf "$WORK_DIR/srcpkg"
mkdir -p "$WORK_DIR/srcpkg"
cd "$WORK_DIR/srcpkg"

apt-get source icecc

SRC_PKG_DIR="$(find . -maxdepth 1 -type d -name 'icecc-*' | sort | head -n1)"
if [ -z "${SRC_PKG_DIR:-}" ]; then
    echo "ERROR: unable to locate unpacked icecc source directory" >&2
    exit 1
fi

NEW_DIR="icecc-${UPSTREAM_VERSION}"
rm -rf "$NEW_DIR"
mv "$SRC_PKG_DIR" "$NEW_DIR"

if [ ! -d "$NEW_DIR/debian" ]; then
    echo "ERROR: missing debian/ in downloaded source package" >&2
    exit 1
fi

rsync -a --delete \
    --exclude ".git" \
    --exclude "debian" \
    --exclude "package_builder" \
    "$SRC_DIR"/ "$NEW_DIR"/

cd "$NEW_DIR"

dch --newversion "$DEB_VERSION" --distribution "$DEB_DIST" "Local build from git checkout."

dpkg-buildpackage -us -uc -b

cd ..
cp -av ./*.deb ./*.ddeb ./*.changes ./*.buildinfo "$OUT_DIR"/ 2>/dev/null || true

echo "OK"
ls -lh "$OUT_DIR" || true

