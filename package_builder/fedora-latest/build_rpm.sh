#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="${SRC_DIR:-/src}"
OUT_DIR="${OUT_DIR:-/out}"
WORK_DIR="${WORK_DIR:-/work}"

mkdir -p "$WORK_DIR" "$OUT_DIR"
cd "$WORK_DIR"

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

configure_dnf() {
    local conf proxy
    conf="/etc/dnf/dnf.conf"
    proxy="${https_proxy:-${http_proxy:-}}"

    if [ -n "${proxy:-}" ]; then
        if grep -q '^proxy=' "$conf" 2>/dev/null; then
            sed -i -E "s|^proxy=.*|proxy=${proxy}|" "$conf"
        else
            printf '\nproxy=%s\n' "$proxy" >> "$conf"
        fi
    fi

    if [ "${ICECREAM_BUILDER_INSECURE:-}" = "1" ] || [ "${ICECREAM_BUILDER_INSECURE:-}" = "true" ]; then
        if grep -q '^sslverify=' "$conf" 2>/dev/null; then
            sed -i -E 's/^sslverify=.*/sslverify=0/' "$conf"
        else
            printf '\nsslverify=0\n' >> "$conf"
        fi
    fi
}

dnf_cmd() {
    local proxy_opt=()
    local proxy="${https_proxy:-${http_proxy:-}}"
    if [ -n "${proxy:-}" ]; then
        proxy_opt+=(--setopt=proxy="${proxy}")
    fi

    if [ "${ICECREAM_BUILDER_INSECURE:-}" = "1" ] || [ "${ICECREAM_BUILDER_INSECURE:-}" = "true" ]; then
        dnf --setopt=sslverify=0 "${proxy_opt[@]}" "$@"
    else
        dnf "${proxy_opt[@]}" "$@"
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
configure_dnf
dnf_cmd -y clean all
dnf_cmd -y makecache

dnf_cmd -y install \
    asciidoc \
    boost-devel \
    ca-certificates \
    dnf-plugins-core \
    libarchive-devel \
    lzo-devel \
    libzstd-devel \
    xxhash-devel \
    rpm-build \
    rpmdevtools \
    git \
    autoconf \
    automake \
    libtool \
    libtool-ltdl-devel \
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

# Version metadata comes from the COMMITTED revision -- a local
# configure.ac edit must not relabel committed source.
SRC_REV=$(git -c "safe.directory=$SRC_DIR" -C "$SRC_DIR" rev-parse HEAD)
BUILD_REQUIREMENTS="$(git -c "safe.directory=$SRC_DIR" -C "$SRC_DIR" show \
    "$SRC_REV:package_builder/probe_build_requirements.sh" | bash)"
echo "$BUILD_REQUIREMENTS"
git -c "safe.directory=$SRC_DIR" -C "$SRC_DIR" show "$SRC_REV:configure.ac" > ./configure.ac.committed
UPSTREAM_VERSION="$(parse_upstream_version ./configure.ac.committed)"

rpmdev-setuptree

cd "$WORK_DIR"
if ! dnf_cmd -y download --source \
    --disablerepo="*" \
    --enablerepo=fedora-source \
    icecream; then
    dnf_cmd -y download --source \
        --disablerepo="*" \
        --enablerepo=fedora-source \
        --enablerepo=updates-source \
        icecream
fi

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

# The remote-worker regression gate must RUN here, not skip: %check executes
# as root in this container, where the gate always can run.  Required mode
# turns any skip into a failure.
sed -i -E 's/^%make_build check|^make check/ICECC_TEST_REQUIRE_REMOTE=1 &/' "$SPEC_PATH"
if grep -qE '^%check' "$SPEC_PATH" && ! grep -q 'ICECC_TEST_REQUIRE_REMOTE' "$SPEC_PATH"; then
    sed -i -E '/^%check/a export ICECC_TEST_REQUIRE_REMOTE=1' "$SPEC_PATH"
fi

# Newer upstream versions install additional helper tools.
if ! grep -q 'icecc-test-env' "$SPEC_PATH"; then
    sed -i -E '/^%files[[:space:]]*$/a %{_bindir}/icecc-test-env' "$SPEC_PATH"
fi
SOURCE0_NAME="icecream-${UPSTREAM_VERSION}.tar.xz"

SOURCE0_PATH="${HOME}/rpmbuild/SOURCES/${SOURCE0_NAME}"
rm -f "${HOME}/rpmbuild/SOURCES/"icecream-*.tar.*

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# Clean export of the committed revision, bootstrapped so Source0 carries
# generated configure/Makefile.in (PKG-1): the previous rsync staging made
# the build depend on untracked developer-tree state -- a clean clone had
# no configure at all (the modified spec skips autogen.sh), while a dirty
# tree could ship stale generated files and host-built executables.
# Bootstrapping at staging time also serves build roots whose own
# Autotools are too old to bootstrap (Fedora 28).
RELEASE_TARBALL_SHA256="git-${SRC_REV}"
if [ -n "${RELEASE_TARBALL:-}" ]; then
    # THE release artifact (single Source0 across all distributions).
    ( cd "$(dirname "$RELEASE_TARBALL")" \
        && sha256sum -c --status "$(basename "$RELEASE_TARBALL").sha256" ) \
        || { echo "ERROR: release tarball digest mismatch" >&2; exit 1; }
    RELEASE_TARBALL_SHA256="$(sha256sum "$RELEASE_TARBALL" | cut -d" " -f1)"
    mkdir "$STAGE/extract-src"
    tar -C "$STAGE/extract-src" -xf "$RELEASE_TARBALL"
    mv "$STAGE"/extract-src/icecream-* "$STAGE/icecream-${UPSTREAM_VERSION}"
    tar -C "$STAGE" -cJf "$SOURCE0_PATH" "icecream-${UPSTREAM_VERSION}"
else
    echo "WARNING: no RELEASE_TARBALL; bootstrapping per-distro (single-Source0 flow: package_builder/make_release_tarball.sh)" >&2
    # Execute the COMMITTED copy of the staging tool, not the workspace's.
    git -c "safe.directory=$SRC_DIR" -C "$SRC_DIR" show \
        "$SRC_REV:package_builder/make_source_tree.sh" > "$STAGE/make_source_tree.committed.sh"
    bash "$STAGE/make_source_tree.committed.sh" \
        "$SRC_DIR" "$STAGE/icecream-${UPSTREAM_VERSION}" --bootstrap
    tar -C "$STAGE" -cJf "$SOURCE0_PATH" "icecream-${UPSTREAM_VERSION}"
fi

dnf_cmd -y builddep "$SPEC_PATH"

rpmbuild -ba "$SPEC_PATH"

# Empty per-run output directory + manifest (PKG-2): repeated runs used to
# accumulate packages, so the verifier could install stale artifacts beside
# the new ones.  The manifest names exactly what this run produced.
rm -f "$OUT_DIR"/*.rpm "$OUT_DIR"/manifest.txt
find "${HOME}/rpmbuild/RPMS" "${HOME}/rpmbuild/SRPMS" -type f -name '*.rpm' -print -exec cp -av {} "$OUT_DIR"/ \;
( cd "$OUT_DIR" && ls -1 *.rpm 2>/dev/null > manifest.txt )
[ -s "$OUT_DIR/manifest.txt" ] || { echo "ERROR: build produced no .rpm packages" >&2; exit 1; }
{
    echo "revision=$SRC_REV"
    echo "version=$UPSTREAM_VERSION"
    echo "builder=$(basename "$(cd "$(dirname "$0")" && pwd)")-$(. /etc/os-release; echo "$ID-$VERSION_ID")"
    echo "release_tarball_sha256=$RELEASE_TARBALL_SHA256"
    echo "$BUILD_REQUIREMENTS"
    echo "source0_sha256=$(sha256sum "$SOURCE0_PATH" | cut -d" " -f1)"
    while IFS= read -r f; do
        echo "sha256 $f=$(sha256sum "$OUT_DIR/$f" | cut -d" " -f1)"
    done < "$OUT_DIR/manifest.txt"
} > "$OUT_DIR/manifest.meta"

echo "OK"
ls -lh "$OUT_DIR" || true
