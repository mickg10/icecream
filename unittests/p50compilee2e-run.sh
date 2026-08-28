#!/bin/sh
# Real all-P50 C1F1 networked ZSTD_ROUTE compile gate.
#
# Once p50compilee2e-source.sh is green this starts the actual built
# scheduler, one actual iceccd F, one actual iceccd C, and the actual
# icecc-cache-service owned by the daemon's sidecar adapter. The compiler
# invocation is required to produce positive ZSTD_ROUTE evidence and a
# byte-identical local reference. No fake peer or legacy FileChunk fallback
# is accepted.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_TOP_BUILDDIR:-$(CDPATH= cd -- "$src" && pwd)}
timeout_s=${ICECC_P50_C1F1_TIMEOUT:-180}
profile_marker=${ICECC_P50_PROFILE:-ZSTD_ROUTE}
warm=${ICECC_P50_C1F1_WARM:-0}
case "$profile_marker" in
    ZSTD_TU|ZSTD_ROUTE) ;;
    *)
        echo "FAIL: ICECC_P50_PROFILE must be ZSTD_TU or ZSTD_ROUTE" >&2
        exit 1
        ;;
esac
case "$warm" in
    0|1) ;;
    *)
        echo "FAIL: ICECC_P50_C1F1_WARM must be 0 or 1" >&2
        exit 1
        ;;
esac

set +e
"$src/unittests/p50compilee2e-source.sh"
rc=$?
set -e
test "$rc" -eq 0 || exit "$rc"

for binary in \
    "$build/daemon/iceccd" \
    "$build/scheduler/icecc-scheduler" \
    "$build/client/icecc" \
    "$build/cache/icecc-cache-service"; do
    test -x "$binary" || {
        echo "SKIP: missing built executable $binary" >&2
        exit 77
    }
done

command -v timeout >/dev/null 2>&1 || {
    echo "SKIP: timeout(1) is required for bounded C1F1 cleanup" >&2
    exit 77
}
command -v g++ >/dev/null 2>&1 || {
    echo "SKIP: g++ is required for the C1F1 compile" >&2
    exit 77
}
command -v bash >/dev/null 2>&1 || {
    echo "SKIP: bash is required for the generated icecc-create-env tool" >&2
    exit 77
}

work=$(mktemp -d "${TMPDIR:-/tmp}/p50compilee2e.XXXXXX")
# The scheduler changes to its configured service account before opening the
# requested log.  Keep the test root traversable and pre-create only that log
# as writable; the cache runtime and HOME below retain their own 0700 modes.
chmod 0711 "$work"
: >"$work/scheduler.log"
chmod 0666 "$work/scheduler.log"
cleanup() {
    for pid in "${service_pid:-}" "${client_pid:-}" "${worker_pid:-}" "${sched_pid:-}"; do
        test -n "$pid" && kill "$pid" 2>/dev/null || :
    done
    for _ in $(seq 1 50); do
        live=0
        for pid in "${service_pid:-}" "${client_pid:-}" "${worker_pid:-}" "${sched_pid:-}"; do
            if test -n "$pid" && kill -0 "$pid" 2>/dev/null; then
                live=1
            fi
        done
        test "$live" -eq 0 && break
        sleep 0.1
    done
    for pid in "${service_pid:-}" "${client_pid:-}" "${worker_pid:-}" "${sched_pid:-}"; do
        test -n "$pid" && kill -9 "$pid" 2>/dev/null || :
    done
    for pid in "${client_pid:-}" "${worker_pid:-}" "${sched_pid:-}"; do
        if test -n "$pid" && ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || :
        fi
    done
    if test "${ICECC_P50_C1F1_KEEP_WORK:-0}" = 1; then
        echo "INFO: preserving P50 C1F1 workdir $work" >&2
    else
        rm -rf "$work"
    fi
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$work/envs-f" "$work/envs-c" "$work/toolchain" "$work/src" "$work/out" \
    "$work/cache-runtime-f" "$work/home"
chmod 1777 "$work/envs-f" "$work/envs-c"
chmod 0700 "$work/cache-runtime-f" "$work/home"
HOME="$work/home"
export HOME
pick_port_pair() {
    python3 - <<'PY'
import secrets
import socket

start = 40000 + 2 * secrets.randbelow(9000)
for offset in range(0, 10000, 2):
    base = start + offset
    if base + 1 >= 60000:
        base -= 18000
    sockets = []
    try:
        # The scheduler owns TCP scheduler_port + 1 for its text endpoint;
        # keep that reserved and place F on the next port.
        for port in (base, base + 1, base + 2):
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(('0.0.0.0', port))
            sockets.append(sock)
    except OSError:
        for sock in sockets:
            sock.close()
        continue
    for sock in sockets:
        sock.close()
    print(base, base + 2)
    break
else:
    raise SystemExit('no available scheduler/worker port pair')
PY
}
if test -n "${ICECC_P50_C1F1_SCHED_PORT:-}" || test -n "${ICECC_P50_C1F1_WORKER_PORT:-}"; then
    port_sched=${ICECC_P50_C1F1_SCHED_PORT:-}
    port_worker=${ICECC_P50_C1F1_WORKER_PORT:-}
else
    ports=$(pick_port_pair)
    port_sched=${ports%% *}
    port_worker=${ports##* }
fi
test -n "$port_sched" && test -n "$port_worker" || {
    echo "FAIL: scheduler and worker ports must be specified together" >&2
    exit 1
}
test "$port_sched" != "$port_worker" || {
    echo "FAIL: scheduler and worker ports must be distinct" >&2
    exit 1
}
test "$port_worker" -ne "$((port_sched + 1))" || {
    echo "FAIL: worker port collides with scheduler text endpoint" >&2
    exit 1
}
network="p50c1f1-$$"

# A corpus cell may provide an exact input from an authenticated source root.
# Keep root and relative path separate so the input cannot accidentally be
# resolved against the build tree or replaced by the generated smoke TU.
source_root=${ICECC_P50_C1F1_SOURCE_ROOT:-}
source_relative=${ICECC_P50_C1F1_SOURCE_RELATIVE:-}
source_input=${ICECC_P50_C1F1_INPUT:-}
if test -n "$source_root" || test -n "$source_relative"; then
    test -n "$source_root" && test -n "$source_relative" || {
        echo "FAIL: source root and source relative path must be supplied together" >&2
        exit 1
    }
    test "${source_root#/}" != "$source_root" || {
        echo "FAIL: source root must be an absolute path" >&2
        exit 1
    }
    case "$source_relative" in
        ''|/*|../*|*/../*|*/..)
            echo "FAIL: source relative path escapes its authenticated root" >&2
            exit 1
            ;;
    esac
    test -d "$source_root" && test ! -L "$source_root" || {
        echo "FAIL: authenticated source root is unavailable" >&2
        exit 1
    }
    source_input="$source_root/$source_relative"
fi
if test -n "$source_input"; then
    test -f "$source_input" && test ! -L "$source_input" || {
        echo "FAIL: authenticated source input is unavailable" >&2
        exit 1
    }
    cp -- "$source_input" "$work/src/main.cpp"
else
    printf '%s\n' \
        '#include <cstdint>' \
        'int p50_c1f1_translation_unit() {' \
        '    return static_cast<int>(UINT32_C(50));' \
        '}' >"$work/src/main.cpp"
fi

# The environment is made by the real icecc tool, then shipped to the real F
# daemon. This is deliberately not replaced by the host compiler PATH.
(cd "$work/toolchain" && timeout "$timeout_s" \
    bash "$build/client/icecc-create-env" "$(command -v g++)" \
    >"$work/create-env.log" 2>&1)
envtar=$(find "$work/toolchain" -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
test -n "$envtar" || {
    echo "FAIL: real icecc-create-env produced no compiler environment" >&2
    exit 1
}

# A root container still runs the production daemon's normal privilege drop.
# Let an explicit test account own the private tree so the daemon can create
# its log and sidecar runtime files after that drop.  No account is selected
# by default, preserving the ordinary host invocation.
set --
if test -n "${ICECC_TEST_DAEMON_UID:-}"; then
    daemon_gid=${ICECC_TEST_DAEMON_GID:-$ICECC_TEST_DAEMON_UID}
    chown -R "$ICECC_TEST_DAEMON_UID:$daemon_gid" "$work"
    set -- -u "$ICECC_TEST_DAEMON_UID"
fi
if test -n "${ICECC_P50_C1F1_WORKER_INTERFACE:-}"; then
    set -- "$@" -i "$ICECC_P50_C1F1_WORKER_INTERFACE"
fi

run_client_with_timeout() {
    if test -n "${ICECC_TEST_DAEMON_UID:-}"; then
        timeout "$timeout_s" runuser -u "$ICECC_TEST_DAEMON_UID" -- \
            "$build/client/icecc" "$@"
    else
        timeout "$timeout_s" "$build/client/icecc" "$@"
    fi
}

"$build/scheduler/icecc-scheduler" -p "$port_sched" -n "$network" \
    --assignment-fence-mode strict-nonce -l "$work/scheduler.log" -vvv &
sched_pid=$!
sleep 1
kill -0 "$sched_pid" 2>/dev/null || {
    echo "FAIL: real scheduler exited during startup" >&2
    exit 1
}

# This is the only F. The daemon itself must supervise and expose the actual
# cache service required by the P50 path; the harness never starts a fake peer.
ICECC_TEST_SOCKET="$work/worker.sock" ICECC_P50_C1F1_REQUIRED=1 \
    "$build/daemon/iceccd" "$@" -p "$port_worker" -m 1 \
    -s "127.0.0.1:$port_sched" -n "$network" -N p50-f \
    -b "$work/envs-f" -l "$work/f.log" -vvv \
    --cache-service "$build/cache/icecc-cache-service" \
    --cache-runtime-dir "$work/cache-runtime-f" &
worker_pid=$!

ICECC_TEST_SOCKET="$work/client.sock" ICECC_P50_C1F1_REQUIRED=1 \
    "$build/daemon/iceccd" "$@" --no-remote -m 0 \
    -s "127.0.0.1:$port_sched" -n "$network" -N p50-c \
    -b "$work/envs-c" -l "$work/c.log" -vvv &
client_pid=$!

logins=0
for _ in $(seq 1 30); do
    logins=$(grep -c login "$work/scheduler.log" 2>/dev/null || true)
    test "${logins:-0}" -ge 2 && break
    sleep 1
done
test "${logins:-0}" -ge 2 || {
    echo "FAIL: real C1F1 daemons did not register" >&2
    exit 1
}

# The cache executable must be alive as a child of the production daemon
# wiring. Merely checking that the file exists would permit a mechanism-only
# test to masquerade as an end-to-end compile.
service_pid=
for _ in $(seq 1 30); do
    service_pid=$(ps -eo pid=,ppid=,args= | \
        awk -v parent="$worker_pid" -v exe="$build/cache/icecc-cache-service" \
        '$2 == parent && index($0, exe) > 0 { print $1; exit }')
    test -n "$service_pid" && break
    sleep 1
done
test -n "$service_pid" || {
    echo "FAIL: production daemon did not start the actual icecc-cache-service" >&2
    exit 1
}

# A live child is not yet a usable cache endpoint.  Submit only after the
# daemon has authenticated READY and the scheduler has consumed F's real
# cache-bearing relogin; otherwise the assignment is correctly frozen without
# a handoff and a millisecond startup race masquerades as a product failure.
cache_ready=0
for _ in $(seq 1 30); do
    if grep -E 'RELOGIN p50-f.*cache=.*cache_profiles=.*zstd_tu' \
        "$work/scheduler.log" >/dev/null 2>&1; then
        cache_ready=1
        break
    fi
    sleep 1
done
test "$cache_ready" -eq 1 || {
    echo "FAIL: production F cache endpoint was not advertised READY" >&2
    exit 1
}

compile_once() {
    label=$1
    remote_obj="$work/out/remote-$label.o"
    local_obj="$work/out/local-$label.o"
    client_log="$work/client-compile-$label.log"
    ICECC_TEST_SOCKET="$work/client.sock" ICECC_TEST_REMOTEBUILD=1 \
        ICECC_VERSION="$envtar" ICECC_P50_C1F1_REQUIRED=1 \
        ICECC_PREFERRED_HOST=p50-f \
        ICECC_DEBUG=debug ICECC_LOGFILE="$client_log" \
        run_client_with_timeout g++ -std=c++17 -O2 -c \
        "$work/src/main.cpp" -o "$remote_obj"
    g++ -std=c++17 -O2 -c "$work/src/main.cpp" -o "$local_obj"
    cmp -s "$remote_obj" "$local_obj" || {
        echo "FAIL: real P50 object differs from local reference ($label)" >&2
        exit 1
    }
}

# Warm is a real two-transaction lifecycle: the prewarm transaction and the
# measured transaction share the same scheduler, C/F daemons, cache service,
# and input source.  The prewarm remains outside measured evidence.
if test "$warm" = 1; then
    echo "S7_WARM_PREWARM_BEGIN"
    compile_once prewarm
    echo "S7_WARM_PREWARM_COMPLETE"
fi
compile_once measured

# Positive evidence is mandatory. Absence of a local marker is not enough:
# the route must identify ZSTD_ROUTE and cache-session handoff.  Legacy FileChunk
# remains correct for environment upload and object return, so the negative
# evidence below is deliberately limited to the source-stream and local/client
# fallback markers.
grep -F "$profile_marker" "$work"/client-compile-*.log "$work/c.log" \
    "$work/f.log" >/dev/null &&
grep -F 'CACHE_SESSION' "$work"/client-compile-*.log "$work/c.log" \
    "$work/f.log" >/dev/null || {
    echo "FAIL: no positive $profile_marker/CACHE_SESSION wire evidence" >&2
    exit 1
}
if grep -E 'write_fd_to_server from cpp|write_fd_to_server preprocessed|building myself|building_local|local build forced|client_exception|fallback_local' \
    "$work"/client-compile-*.log "$work/c.log" "$work/f.log" >/dev/null 2>&1; then
    echo "FAIL: compile path used legacy source streaming or local/client fallback" >&2
    exit 1
fi

echo "PASS: all-P50 C1F1 $profile_marker compile is remote and byte-identical"
