#!/bin/sh
# SIDECAR CONFIG REFUSAL naming regression (R17a, virtu2-diag).
#
# The cache sidecar adapter's valid_config() is a 25-clause boolean chain
# (cache/p50_daemon_sidecar_adapter.cpp); the daemon refused positive mode
# with ONE generic line: "invalid cache sidecar configuration; refusing
# positive mode" -- and the operator had to bisect the chain by hand (the
# private-pathname leg alone checks lstat + directory + owner + 0700).
#
# The fix adds DaemonSidecarAdapter::config_refusal_reason(): the first
# failing clause, formatted with the field and, where a pathname is
# involved, the path plus EXPECTED vs ACTUAL owner and mode.  The daemon's
# refusal line now carries it.  Behaviour identical: same refusal, same
# positive-mode skip -- only the log is named.
#
# This test pins the WRONG-MODE runtime dir leg end to end: a runtime dir
# deliberately at 0755 (and a second leg at the wrong owner) must log the
# path, BOTH owners/modes and the 0700 expectation, and the daemon still
# refuses positive mode (no sidecar service child).  RED at d36df986: the
# log has only the generic string, none of the named fields.
#
# Root-gated like stdfdsanitize-sidecar.sh: the cache adapter refuses
# non-root daemon configs ("invalid cache sidecar configuration") -- the
# production shape IS the root-started capng-drop, and the fixture needs
# chown.  Skip (77) under the unprivileged native-check; runs in
# native-root-check.
set -u

fail() { echo "p50sidecarrefusal: FAIL: $*" 1>&2; exit 1; }

src_dir=$(dirname "$0")
build_dir=${ICECC_TEST_BUILDDIR:-$src_dir}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$build_dir/..}
ICECCD=${ICECCD:-$top_build_dir/daemon/iceccd}
SCHEDULER=${SCHEDULER:-$top_build_dir/scheduler/icecc-scheduler}

if [ "$(id -u)" != "0" ]; then
    echo "p50sidecarrefusal: skipping (needs root: the cache adapter refuses non-root daemons)" 1>&2
    exit 77
fi

icecc_user=icecc
if ! id -u "$icecc_user" >/dev/null 2>&1; then
    echo "p50sidecarrefusal: skipping (no $icecc_user user in the image)" 1>&2
    exit 77
fi
icecc_uid=$(id -u "$icecc_user")
icecc_gid=$(id -g "$icecc_user")

workdir=$(mktemp -d /tmp/p5scref.XXXXXX) || fail mktemp
trap 'kill $sched_pid $daemon_pid 2>/dev/null; rm -rf "$workdir"' EXIT INT TERM

mkdir -p "$workdir/envs" "$workdir/home"
# Leg 1: the runtime dir at the WRONG MODE (0755, correct owner).
mkdir -m 0755 "$workdir/cache-rt-wrongmode"
chown "$icecc_user:$icecc_user" "$workdir/cache-rt-wrongmode"
# Leg 2: the runtime dir at the WRONG OWNER (root-owned, correct 0700).
mkdir -m 0700 "$workdir/cache-rt-wrongowner"
chown -R "$icecc_user" "$workdir"
chown root:root "$workdir/cache-rt-wrongowner"
chmod 0700 "$workdir/cache-rt-wrongowner"

run_leg() { # <runtime-dir> <logname> -> starts scheduler+daemon, waits, greps
    rt=$1; log=$2
    ICECC_TEST_SOCKET="$workdir/sched.sock" HOME="$workdir/home" \
        "$SCHEDULER" -vvv -n screfnet -l "$workdir/$log-sched.log" &
    sched_pid=$!
    sleep 1
    ICECC_TEST_SOCKET="$workdir/daemon.sock" HOME="$workdir/home" \
        "$ICECCD" -vvv -n screfnet -N localnode -b "$workdir/envs" \
        --cache-service "$top_build_dir/cache/icecc-cache-service" \
        --cache-runtime-dir "$rt" -l "$workdir/$log.log" &
    daemon_pid=$!
    # The refusal happens at adapter creation, before any service child.
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sleep 1
        grep -q "invalid cache sidecar configuration" "$workdir/$log.log" 2>/dev/null && return 0
        kill -0 "$daemon_pid" 2>/dev/null || return 0
    done
    return 0
}

check_refusal_named() { # <logname> <must-contain...>
    log=$1; shift
    for needle in "$@"; do
        grep -q "$needle" "$workdir/$log.log" 2>/dev/null || \
            fail "refusal log lacks the named field '$needle' (check $workdir/$log.log)"
    done
}

# ---- leg 1: wrong mode ----
run_leg "$workdir/cache-rt-wrongmode" leg1
# The named reason carries the FULL line: the path, the ACTUAL
# owner:group (icecc:icecc, right owner, wrong mode) AND actual mode,
# plus the EXPECTED uid:gid and mode 0700.  The pre-fix code built the
# line in a 64-byte snprintf buffer and truncated it right after the
# path -- exactly the information this test asserts.
check_refusal_named leg1 "invalid cache sidecar configuration" \
    "cache runtime directory" \
    "owner $icecc_uid:$icecc_gid" \
    "mode 755" \
    "expected $icecc_uid:$icecc_gid mode 0700"
# Positive mode refused: no service child was exec'd (no cache service process
# parented by our daemon).
children=$(pgrep -P "$daemon_pid" 2>/dev/null | wc -l)
[ "$children" -eq 0 ] || fail "positive mode was not refused (service child exists)"
kill "$sched_pid" "$daemon_pid" 2>/dev/null
sleep 1

# ---- leg 2: wrong owner ----
run_leg "$workdir/cache-rt-wrongowner" leg2
# Full line again: ACTUAL owner 0:0 with the right mode, EXPECTED
# icecc:icecc mode 0700.
check_refusal_named leg2 "invalid cache sidecar configuration" \
    "cache runtime directory" \
    "owner 0:0" \
    "mode 700" \
    "expected $icecc_uid:$icecc_gid mode 0700"
children=$(pgrep -P "$daemon_pid" 2>/dev/null | wc -l)
[ "$children" -eq 0 ] || fail "positive mode was not refused (service child exists)"

kill "$sched_pid" "$daemon_pid" 2>/dev/null
echo "p50sidecarrefusal: OK (refusal names the path, owner and mode; positive mode still refused)"
