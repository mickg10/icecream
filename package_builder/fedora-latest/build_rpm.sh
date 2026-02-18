#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="${SRC_DIR:-/src}"
OUT_DIR="${OUT_DIR:-/out}"
WORK_DIR="${WORK_DIR:-/work}"

mkdir -p "$WORK_DIR" "$OUT_DIR"
cd "$WORK_DIR"

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

dnf -y clean all
dnf -y makecache

dnf -y install \
    asciidoc \
    ca-certificates \
    dnf-plugins-core \
    libarchive-devel \
    libzstd-devel \
    rpm-build \
    rpmdevtools \
    rsync \
    tar \
    xz \
    xmlto \
    make \
    gcc-c++ \
    autoconf \
    automake \
    libtool \
    patch \
    diffutils \
    findutils \
    which

UPSTREAM_VERSION="$(parse_upstream_version "$SRC_DIR/configure.ac")"

rpmdev-setuptree

cd "$WORK_DIR"
dnf config-manager --set-enabled fedora-source updates-source >/dev/null 2>&1 || true
dnf -y download --source icecream

SRPM="$(ls -1 icecream-*.src.rpm | head -n1 || true)"
if [ -z "${SRPM:-}" ]; then
    echo "ERROR: failed to download icecream SRPM" >&2
    exit 1
fi

rpm -ivh --define "_topdir ${HOME}/rpmbuild" "$SRPM"

SPEC_PATH="${HOME}/rpmbuild/SPECS/icecream.spec"
if [ ! -f "$SPEC_PATH" ]; then
    echo "ERROR: spec not found at $SPEC_PATH" >&2
    exit 1
fi

sed -i -E \
    -e "s/^(Version:\\s*).*/\\1${UPSTREAM_VERSION}/" \
    -e "s/^(Release:\\s*).*/\\11.obs1%{?dist}/" \
    "$SPEC_PATH"

sed -i -E "s@^Source0:.*@Source0: %{name}-%{version}.tar.xz@" "$SPEC_PATH"

# Fedora packaging for RC snapshots may use a different topdir (e.g. appending
# "rc1") in %prep via %autosetup/%setup -n. We generate Source0 with the
# standard "%{name}-%{version}" directory name, so force %prep to match.
sed -i -E 's/^(%autosetup.*-n[[:space:]]+)[^[:space:]]+(.*)$/\1%{name}-%{version}\2/' "$SPEC_PATH"
sed -i -E 's/^(%setup.*-n[[:space:]]+)[^[:space:]]+(.*)$/\1%{name}-%{version}\2/' "$SPEC_PATH"

# The Fedora SRPM often carries patch hunks that don't apply cleanly to a newer
# upstream checkout. Keep the packaging bits, but disable patch application.
sed -i -E '/^%patch[0-9]*/d' "$SPEC_PATH"
sed -i -E '/^%autosetup/ {/ -N/! s/^%autosetup/%autosetup -N/}' "$SPEC_PATH"

# Don't regenerate build system files; the git checkout ships a working
# configure script.
sed -i -E '/^[[:space:]]*\\.\\/autogen\\.sh/d' "$SPEC_PATH"

# Newer upstream versions install additional helper tools.
if ! grep -q 'icecc-test-env' "$SPEC_PATH"; then
    sed -i -E '/^%files[[:space:]]*$/a %{_bindir}/icecc-test-env' "$SPEC_PATH"
fi
SOURCE0_NAME="icecream-${UPSTREAM_VERSION}.tar.xz"

SOURCE0_PATH="${HOME}/rpmbuild/SOURCES/${SOURCE0_NAME}"
rm -f "${HOME}/rpmbuild/SOURCES/"icecream-*.tar.*

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/icecream-${UPSTREAM_VERSION}"
rsync -a --delete \
    --exclude ".git" \
    --exclude ".deps" \
    --exclude ".libs" \
    --exclude "autom4te.cache" \
    --exclude "config.h" \
    --exclude "config.log" \
    --exclude "config.status" \
    --exclude "GNUmakefile" \
    --exclude "Makefile" \
    --exclude "package_builder" \
    --exclude "tests/compose/out" \
    --exclude "tests/webgui/node_modules" \
    --exclude "stamp-h1" \
    --exclude "*.a" \
    --exclude "*.la" \
    --exclude "*.lo" \
    --exclude "*.o" \
    "$SRC_DIR"/ "$STAGE/icecream-${UPSTREAM_VERSION}"/

tar -C "$STAGE" -cJf "$SOURCE0_PATH" "icecream-${UPSTREAM_VERSION}"

dnf -y builddep "$SPEC_PATH"

rpmbuild -ba "$SPEC_PATH"

find "${HOME}/rpmbuild/RPMS" "${HOME}/rpmbuild/SRPMS" -type f -name '*.rpm' -print -exec cp -av {} "$OUT_DIR"/ \;

echo "OK"
ls -lh "$OUT_DIR" || true
