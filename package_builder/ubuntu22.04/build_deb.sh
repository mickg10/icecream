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

normalize_proxy_env() {
    if [ -z "${http_proxy:-}" ] && [ -n "${HTTP_PROXY:-}" ]; then
        export http_proxy="$HTTP_PROXY"
    fi
    if [ -z "${https_proxy:-}" ] && [ -n "${HTTPS_PROXY:-}" ]; then
        export https_proxy="$HTTPS_PROXY"
    fi
    if [ -z "${no_proxy:-}" ] && [ -n "${NO_PROXY:-}" ]; then
        export no_proxy="$NO_PROXY"
    fi
}

mkdir -p "$WORK_DIR" "$OUT_DIR"
cd "$WORK_DIR"

configure_apt_insecure() {
    if [ "${ICECREAM_BUILDER_INSECURE:-}" = "1" ] || [ "${ICECREAM_BUILDER_INSECURE:-}" = "true" ]; then
        printf '%s\n' \
            'Acquire::https::Verify-Peer "false";' \
            'Acquire::https::Verify-Host "false";' \
            > /etc/apt/apt.conf.d/99icecream-builder-insecure
    fi
}

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
    major="$(awk -F'[][]' '$2 == "icecream_version_major" {print $4; exit}' "$1")"
    minor="$(awk -F'[][]' '$2 == "icecream_version_minor" {print $4; exit}' "$1")"
    micro="$(awk -F'[][]' '$2 == "icecream_version_micro" {print $4; exit}' "$1")"
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

normalize_proxy_env
configure_apt_insecure
apt-get update
enable_deb_src
apt-get update

apt-get install -y --no-install-recommends \
    asciidoc \
    ca-certificates \
    docbook-xsl \
    devscripts \
    dpkg-dev \
    equivs \
    fakeroot \
    pkg-config \
    git \
    autoconf \
    automake \
    libtool \
    rsync \
    xmlto \
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

# Clean export of the committed revision (PKG-1/PKG-2): staging from the
# workspace made the package depend on untracked local state -- stale
# generated Autotools files decided whether the build worked at all, and
# host-built executables could ride into Source0.  The debian/ directory
# from the downloaded source package is preserved.
rm -rf ./debian-packaging
mv "$NEW_DIR/debian" ./debian-packaging
rm -rf "$NEW_DIR"
bash "$SRC_DIR/package_builder/make_source_tree.sh" "$SRC_DIR" "$NEW_DIR" --bootstrap
mv ./debian-packaging "$NEW_DIR/debian"

cd "$NEW_DIR"

dch --newversion "$DEB_VERSION" --distribution "$DEB_DIST" "Local build from git checkout."

dpkg-buildpackage -us -uc -b

cd ..
# Empty per-run output directory + manifest (PKG-2): repeated runs used to
# accumulate packages, so a verifier could install stale artifacts beside
# the new ones.  The manifest names exactly what this run produced; the
# verifier installs that and nothing else.
rm -f "$OUT_DIR"/*.deb "$OUT_DIR"/*.ddeb "$OUT_DIR"/*.changes "$OUT_DIR"/*.buildinfo "$OUT_DIR"/manifest.txt
cp -av ./*.deb ./*.ddeb ./*.changes ./*.buildinfo "$OUT_DIR"/ 2>/dev/null || true
( cd "$OUT_DIR" && ls -1 *.deb 2>/dev/null > manifest.txt )
[ -s "$OUT_DIR/manifest.txt" ] || { echo "ERROR: build produced no .deb packages" >&2; exit 1; }

echo "OK"
ls -lh "$OUT_DIR" || true
