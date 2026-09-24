#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="${SRC_DIR:-/src}"
OUT_DIR="${OUT_DIR:-/out}"

export DEBIAN_FRONTEND=noninteractive

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

configure_apt_insecure() {
    if [ "${ICECREAM_BUILDER_INSECURE:-}" = "1" ] || [ "${ICECREAM_BUILDER_INSECURE:-}" = "true" ]; then
        printf '%s\n' \
            'Acquire::https::Verify-Peer "false";' \
            'Acquire::https::Verify-Host "false";' \
            > /etc/apt/apt.conf.d/99icecream-builder-insecure
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

[ -s "$OUT_DIR/manifest.txt" ] || { echo "ERROR: no manifest.txt in $OUT_DIR (run the build first)" >&2; exit 1; }
# Digest verification: every manifest entry must match manifest.meta before
# anything is installed.
[ -s "$OUT_DIR/manifest.meta" ] || { echo "ERROR: no manifest.meta in $OUT_DIR" >&2; exit 1; }
echo "verifying against $(grep '^revision=' "$OUT_DIR/manifest.meta")"
while IFS= read -r name; do
    want=$(grep -F "sha256 $name=" "$OUT_DIR/manifest.meta" | cut -d= -f2)
    [ -n "$want" ] || { echo "ERROR: no digest for $name in manifest.meta" >&2; exit 1; }
    got=$(sha256sum "$OUT_DIR/$name" | cut -d" " -f1)
    [ "$want" = "$got" ] || { echo "ERROR: digest mismatch for $name" >&2; exit 1; }
done < "$OUT_DIR/manifest.txt"


normalize_proxy_env
configure_apt_insecure
apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates \
    file \
    xz-utils \
    build-essential

# Install EXACTLY the manifest's packages (PKG-2); unexpected package
# files in the output directory fail the run.
DEBS=()
while IFS= read -r name; do
    [ -f "$OUT_DIR/$name" ] || { echo "ERROR: manifest names missing file $name" >&2; exit 1; }
    DEBS+=("$OUT_DIR/$name")
done < "$OUT_DIR/manifest.txt"
while IFS= read -r -d "" f; do
    grep -qxF "$(basename "$f")" "$OUT_DIR/manifest.txt" \
        || { echo "ERROR: unexpected package file not in manifest: $f" >&2; exit 1; }
done < <(find "$OUT_DIR" -maxdepth 1 -type f -name '*.deb' -print0)
[ "${#DEBS[@]}" -gt 0 ] || { echo "ERROR: manifest lists no .deb packages" >&2; exit 1; }
dpkg -i "${DEBS[@]}" || apt-get -f install -y

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

if ! dpkg-query -W -f='${Version}\n' icecc | grep -q "^${UPSTREAM_VERSION}"; then
    echo "ERROR: unexpected dpkg version for icecc:" >&2
    dpkg-query -W -f='${Package} ${Version}\n' icecc >&2 || true
    exit 1
fi

WRAPDIR="/usr/lib/icecc/bin"
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

icecc-scheduler -p "$SCHED_PORT" -vvv >/tmp/icecc-scheduler.log 2>&1 &
SCHED_PID=$!

cleanup() {
    kill "$SCHED_PID" >/dev/null 2>&1 || true
    kill "$ICECCD_PID" >/dev/null 2>&1 || true
    kill "${WORKER_PID:-0}" >/dev/null 2>&1 || true
}

# The daemons build environments as the icecc user, which cannot write the
# scratch TMPDIR that dev/python.sh exports.
TMPDIR=/tmp iceccd --no-remote -m 1 --max-preprocess 8 -s "$ICECC_SCHEDULER" -vv >/tmp/iceccd.log 2>&1 &
ICECCD_PID=$!
trap cleanup EXIT

# A produced object file proves nothing on its own: the wrapper compiles
# locally by design when no daemon is reachable, so a broken scheduler or
# daemon package still yields /tmp/icecc_verify.o.  Assert the services are
# alive, that the daemon actually registered with the scheduler, and that
# the scheduler recorded the job.
# NB: "login <node>" is a trace-level line, hence the -vvv above; "NEW <id>
# client=" is info-level.  Both must be greppable or these assertions would
# fail on a healthy system.
for _ in $(seq 1 30); do
    if grep -q "login" /tmp/icecc-scheduler.log 2>/dev/null; then
        break
    fi
    sleep 1
done

kill -0 "$SCHED_PID" 2>/dev/null || { echo "ERROR: scheduler died during startup" >&2; tail -20 /tmp/icecc-scheduler.log >&2; exit 1; }
kill -0 "$ICECCD_PID" 2>/dev/null || { echo "ERROR: daemon died during startup" >&2; tail -20 /tmp/iceccd.log >&2; exit 1; }

if ! grep -q "login" /tmp/icecc-scheduler.log 2>/dev/null; then
    echo "ERROR: daemon never registered with the scheduler" >&2
    tail -20 /tmp/icecc-scheduler.log >&2
    tail -20 /tmp/iceccd.log >&2
    exit 1
fi

cat >/tmp/icecc_verify.c <<'EOF'
int main(void) { return 0; }
EOF

"$WRAPDIR/gcc" -c /tmp/icecc_verify.c -o /tmp/icecc_verify.o

test -s /tmp/icecc_verify.o

# The scheduler must have seen this compile, otherwise the wrapper silently
# fell back to a plain local build and the packages were never exercised.
if ! grep -qE "NEW [0-9]+ client=" /tmp/icecc-scheduler.log 2>/dev/null; then
    echo "ERROR: the compile never reached the scheduler (silent local fallback)" >&2
    tail -30 /tmp/icecc-scheduler.log >&2
    tail -30 /tmp/iceccd.log >&2
    exit 1
fi


# --- installed remote-worker path -------------------------------------------
# The scheduler-request check above proves coordination, not execution: a
# broken worker package still passes it.  Run a second INSTALLED daemon as a
# remote-capable worker, force a compile onto it, and require worker-side
# begin/done plus returned-object evidence, all time-bounded.
WORKER_BASE=/var/cache/icecream-worker
mkdir -p "$WORKER_BASE"
WORKER_USER=nobody
id -u icecc >/dev/null 2>&1 && WORKER_USER=icecc
chown "$WORKER_USER" "$WORKER_BASE" 2>/dev/null || true

# Own unix socket: without it the worker contends with the primary daemon
# for the default socket and the wrapper may adopt the WORKER as its local
# daemon, silently changing what this test exercises.
TMPDIR=/tmp ICECC_TEST_SOCKET=/tmp/icecc-worker.sock \
iceccd -p 10262 -m 2 -s "$ICECC_SCHEDULER" -N pkgworker -b "$WORKER_BASE" \
    -l /tmp/icecc-worker.log -vvv &
WORKER_PID=$!

for _ in $(seq 1 30); do
    grep -q "login pkgworker" /tmp/icecc-scheduler.log 2>/dev/null && break
    kill -0 "$WORKER_PID" 2>/dev/null || { echo "ERROR: worker daemon died during startup" >&2; tail -20 /tmp/icecc-worker.log >&2; exit 1; }
    sleep 1
done
grep -q "login pkgworker" /tmp/icecc-scheduler.log \
    || { echo "ERROR: worker daemon never registered" >&2; tail -20 /tmp/icecc-worker.log >&2; exit 1; }

ENVDIR=$(mktemp -d)
( cd "$ENVDIR" && timeout 180 icecc-create-env "$(command -v gcc)" >create-env.log 2>&1 ) \
    || { echo "ERROR: icecc-create-env failed" >&2; tail -20 "$ENVDIR/create-env.log" >&2; exit 1; }
ENVTAR=$(ls "$ENVDIR"/*.tar.gz | head -1)

cat >/tmp/icecc_remote_verify.c <<'EOF'
int icecc_remote_verify_marker(int x) { return x * 43 + 7; }
EOF
timeout 120 env ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=pkgworker \
    ICECC_VERSION="$ENVTAR" ICECC_DEBUG=debug ICECC_LOGFILE=/tmp/icecc-client.log \
    "$WRAPDIR/gcc" -c /tmp/icecc_remote_verify.c -o /tmp/icecc_remote_verify.o
rc=$?
[ $rc -eq 124 ] && { echo "ERROR: remote compile TIMED OUT (installed worker path hangs)" >&2; exit 1; }
[ $rc -eq 0 ] || { echo "ERROR: remote compile failed with $rc" >&2; tail -30 /tmp/icecc-client.log >&2; tail -30 /tmp/icecc-worker.log >&2; exit 1; }
test -s /tmp/icecc_remote_verify.o || { echo "ERROR: remote object missing/empty" >&2; exit 1; }

grep -qE "building myself|local build forced" /tmp/icecc-client.log \
    && { echo "ERROR: client fell back to a local build on the worker path" >&2; exit 1; }
grep -q "Remote compilation completed with exit code 0" /tmp/icecc-worker.log \
    || { echo "ERROR: worker never logged a completed remote compilation" >&2; tail -30 /tmp/icecc-worker.log >&2; exit 1; }
sleep 1
grep -qE "END [0-9]+ status=0 .*out=[1-9].* server=pkgworker" /tmp/icecc-scheduler.log \
    || { echo "ERROR: scheduler recorded no successful JobDone from the installed worker" >&2; tail -30 /tmp/icecc-scheduler.log >&2; exit 1; }
echo "installed worker path: OK"

kill -0 "$SCHED_PID" 2>/dev/null || { echo "ERROR: scheduler died during the compile" >&2; exit 1; }
kill -0 "$ICECCD_PID" 2>/dev/null || { echo "ERROR: daemon died during the compile" >&2; exit 1; }

echo "OK"
