#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="${SRC_DIR:-/src}"
OUT_DIR="${OUT_DIR:-/out}"

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

if ! ls -1 "$OUT_DIR"/*.rpm >/dev/null 2>&1; then
    echo "ERROR: no .rpm files found in $OUT_DIR (run the build first)" >&2
    exit 1
fi

normalize_proxy_env
configure_dnf
dnf_cmd -y clean all
dnf_cmd -y makecache

dnf_cmd -y install \
    ca-certificates \
    findutils \
    gcc \
    gcc-c++ \
    make \
    which

mapfile -t RPMS < <(find "$OUT_DIR" -maxdepth 1 -type f -name '*.rpm' ! -name '*.src.rpm' | sort)
if [ "${#RPMS[@]}" -eq 0 ]; then
    echo "ERROR: no binary RPMs found in $OUT_DIR" >&2
    exit 1
fi

dnf_cmd -y install "${RPMS[@]}"

UPSTREAM_VERSION="$(parse_upstream_version "$SRC_DIR/configure.ac")"

if ! command -v icecc >/dev/null 2>&1; then
    echo "ERROR: icecc not found after installing packages" >&2
    exit 1
fi

if ! icecc --version | grep -q "ICECC ${UPSTREAM_VERSION}"; then
    echo "ERROR: unexpected icecc --version output:" >&2
    icecc --version >&2 || true
    exit 1
fi

if ! rpm -q --qf '%{VERSION}\n' icecream | grep -q "^${UPSTREAM_VERSION}$"; then
    echo "ERROR: unexpected rpm version for icecream:" >&2
    rpm -q icecream >&2 || true
    exit 1
fi

WRAPDIR="/usr/libexec/icecc/bin"
if [ ! -d "$WRAPDIR" ]; then
    echo "ERROR: expected wrapper dir missing: $WRAPDIR" >&2
    exit 1
fi

for tool in gcc g++; do
    if [ ! -e "$WRAPDIR/$tool" ]; then
        echo "ERROR: expected wrapper missing: $WRAPDIR/$tool" >&2
        exit 1
    fi
done

if ! command -v iceccd >/dev/null 2>&1; then
    echo "ERROR: iceccd not found after installing packages" >&2
    exit 1
fi

if ! command -v icecc-scheduler >/dev/null 2>&1; then
    echo "ERROR: icecc-scheduler not found after installing packages" >&2
    exit 1
fi

SCHED_PORT="${SCHED_PORT:-8765}"
export ICECC_SCHEDULER="127.0.0.1:${SCHED_PORT}"

icecc-scheduler -p "$SCHED_PORT" -vv >/tmp/icecc-scheduler.log 2>&1 &
SCHED_PID=$!

cleanup() {
    kill "$SCHED_PID" >/dev/null 2>&1 || true
    kill "$ICECCD_PID" >/dev/null 2>&1 || true
}

iceccd --no-remote -m 1 --max-preprocess 8 -s "$ICECC_SCHEDULER" -vv >/tmp/iceccd.log 2>&1 &
ICECCD_PID=$!
trap cleanup EXIT

sleep 1

cat >/tmp/icecc_verify.c <<'EOF'
int main(void) { return 0; }
EOF

"$WRAPDIR/gcc" -c /tmp/icecc_verify.c -o /tmp/icecc_verify.o

test -s /tmp/icecc_verify.o

echo "OK"
