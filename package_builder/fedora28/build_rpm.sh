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

    cat > /etc/yum.repos.d/fedora-source.repo <<'EOF'
[fedora-source]
name=Fedora 28 - Source
baseurl=https://archives.fedoraproject.org/pub/archive/fedora/linux/releases/28/Everything/source/tree/
enabled=0
gpgcheck=0
metadata_expire=7d
skip_if_unavailable=0
EOF

    cat > /etc/yum.repos.d/fedora-updates-source.repo <<'EOF'
[updates-source]
name=Fedora 28 - Source - Updates
baseurl=https://archives.fedoraproject.org/pub/archive/fedora/linux/updates/28/Everything/source/tree/
enabled=0
gpgcheck=0
metadata_expire=7d
skip_if_unavailable=0
EOF
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
if ! dnf_cmd -y makecache; then
    echo "WARN: dnf makecache failed; retrying with Fedora archives repos" >&2
    write_archive_repos
    dnf_cmd -y clean all
    dnf_cmd -y makecache
fi

dnf_cmd -y install \
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

# The Fedora SRPM often carries patch hunks that don't apply cleanly to a newer
# upstream checkout. Keep the packaging bits, but disable patch application.
sed -i -E '/^%patch[0-9]*/d' "$SPEC_PATH"
sed -i -E '/^%autosetup/ {/ -N/! s/^%autosetup/%autosetup -N/}' "$SPEC_PATH"

# Fedora 28 ships an older Autoconf; don't regenerate build system files.
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

if ! dnf_cmd -y builddep "$SPEC_PATH"; then
    echo "WARN: dnf builddep failed; continuing anyway" >&2
fi

rpmbuild -ba "$SPEC_PATH"

find "${HOME}/rpmbuild/RPMS" "${HOME}/rpmbuild/SRPMS" -type f -name '*.rpm' -print -exec cp -av {} "$OUT_DIR"/ \;

echo "OK"
ls -lh "$OUT_DIR" || true
