#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="${SRC_DIR:-/src}"
OUT_DIR="${OUT_DIR:-/out}"
WORK_DIR="${WORK_DIR:-/work}"

mkdir -p "$WORK_DIR" "$OUT_DIR"
cd "$WORK_DIR"

write_archive_repos() {
    cat > /etc/yum.repos.d/fedora.repo <<'EOF'
[fedora]
name=Fedora 28 - $basearch
baseurl=https://archives.fedoraproject.org/pub/archive/fedora/linux/releases/28/Everything/$basearch/os/
enabled=1
gpgcheck=0
metadata_expire=7d
skip_if_unavailable=0
EOF

    cat > /etc/yum.repos.d/fedora-updates.repo <<'EOF'
[updates]
name=Fedora 28 - $basearch - Updates
baseurl=https://archives.fedoraproject.org/pub/archive/fedora/linux/updates/28/Everything/$basearch/
enabled=1
gpgcheck=0
metadata_expire=7d
skip_if_unavailable=0
EOF
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

write_archive_repos

dnf -y clean all
dnf -y makecache

dnf -y install \
    ca-certificates \
    dnf-plugins-core \
    rpm-build \
    rpmdevtools \
    rsync \
    tar \
    xz \
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
SOURCE0_NAME="icecream-${UPSTREAM_VERSION}.tar.xz"

SOURCE0_PATH="${HOME}/rpmbuild/SOURCES/${SOURCE0_NAME}"
rm -f "${HOME}/rpmbuild/SOURCES/"icecream-*.tar.*

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/icecream-${UPSTREAM_VERSION}"
rsync -a --delete \
    --exclude ".git" \
    --exclude "package_builder" \
    "$SRC_DIR"/ "$STAGE/icecream-${UPSTREAM_VERSION}"/

tar -C "$STAGE" -cJf "$SOURCE0_PATH" "icecream-${UPSTREAM_VERSION}"

if ! dnf -y builddep "$SPEC_PATH"; then
    echo "WARN: dnf builddep failed; continuing anyway" >&2
fi

rpmbuild -ba "$SPEC_PATH"

find "${HOME}/rpmbuild/RPMS" "${HOME}/rpmbuild/SRPMS" -type f -name '*.rpm' -print -exec cp -av {} "$OUT_DIR"/ \\;

echo "OK"
ls -lh "$OUT_DIR" || true
