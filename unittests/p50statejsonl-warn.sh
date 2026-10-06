#!/bin/sh
# STATE-JSONL WRITER FAILS LOUDLY regression (R17c, virtu2-diag).
#
# aug3's finding: an iceccd started with --state-jsonl pointing into a
# directory its POST-DROP uid cannot write silently wrote NOTHING -- the
# writer child retried its open for 60x5s and abandoned without a word,
# the daemon reported writer_alive=true, and telemetry just never landed.
#
# The fix (daemon/main.cpp + daemon/statewriter.cpp): after the capng
# uid drop, right where the state writer is started, the daemon PROBES
# the jsonl path once with the writer child's own open flags and, on
# failure, logs a NAMED error (path, uid, gid, errno, "state telemetry
# WILL BE LOST; continuing without it").  WARN-AND-CONTINUE is the
# deliberate choice: telemetry must not take down a compile worker, but
# the warning must be loud and it must be in the startup log, not
# discoverable only by the file's absence.  The writer child also names
# the path+errno ONCE on its first failed open (a directory can go bad
# later; the 60x5s retry used to be completely silent).
#
# This test pins it: a root-owned, 0755 state directory (unwritable by the
# dropped icecc uid) must produce the NAMED startup warning naming the
# path and the uid, and the daemon must CONTINUE (it logs in, serves the
# unix socket -- warn-and-continue, not refuse-to-start).  RED at
# d36df986: the startup is completely silent about it.
#
# Root-gated: the post-drop probe is only meaningful in the root-started
# capng-drop shape; skip (77) unprivileged.
set -u

fail() { echo "p50statejsonl-warn: FAIL: $*" 1>&2; exit 1; }

src_dir=$(dirname "$0")
build_dir=${ICECC_TEST_BUILDDIR:-$src_dir}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$build_dir/..}
ICECCD=${ICECCD:-$top_build_dir/daemon/iceccd}

if [ "$(id -u)" != "0" ]; then
    echo "p50statejsonl-warn: skipping (needs the root-started capng-drop shape)" 1>&2
    exit 77
fi
if ! id -u icecc >/dev/null 2>&1; then
    echo "p50statejsonl-warn: skipping (no icecc user in the image)" 1>&2
    exit 77
fi
icecc_uid=$(id -u icecc)

workdir=$(mktemp -d /tmp/p5state.XXXXXX) || fail mktemp
trap 'kill $daemon_pid 2>/dev/null; rm -rf "$workdir"' EXIT INT TERM

# The state dir: deliberately ROOT-OWNED 0755 (unwritable by the dropped
# icecc uid) -- the exact aug3 shape.
mkdir -p "$workdir/statedir"
chmod 0755 "$workdir/statedir"
mkdir -p "$workdir/envs" "$workdir/home"
chown -R icecc:icecc "$workdir"
chown root:root "$workdir/statedir"
chmod 0755 "$workdir/statedir"

# --state-log matters: the writer child carries the DAEMON'S LOGFILE as
# its statelog path exactly then, and the child's first-failed-open
# warning is routed INTO that file (in the daemonized production shape
# fd 2 is /dev/null, so a stderr line would be lost -- the R17c fix).
ICECC_TEST_SOCKET="$workdir/d.sock" HOME="$workdir/home" \
    "$ICECCD" -vvv -n statewarnnet -N statewarnnode -b "$workdir/envs" \
    --state-jsonl "$workdir/statedir/iceccd-state.jsonl" --state-interval 1 \
    --state-log -l "$workdir/daemon.log" &
daemon_pid=$!

warned=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    sleep 1
    kill -0 "$daemon_pid" 2>/dev/null || break
    grep -q "state-jsonl path.*not writable\|state telemetry WILL BE LOST" \
        "$workdir/daemon.log" 2>/dev/null && { warned=1; break; }
done

# 1. the NAMED startup warning fired (the daemon's own probe -> -l log)
[ "$warned" = 1 ] || \
    fail "no named state-jsonl warning in the startup log (check $workdir/daemon.log)"
# 1b. the WRITER CHILD's first-failed-open line landed in the -l file too
# (the R17c log route; with a root-owned dir the child's open of the
# jsonl path fails on its FIRST record -- state-interval 1 makes the
# first dump arrive within seconds)
child_warned=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    sleep 1
    grep -q "state writer: cannot open" "$workdir/daemon.log" 2>/dev/null && { child_warned=1; break; }
done
[ "$child_warned" = 1 ] || \
    fail "no writer-child failure line in the -l log (the child route must land there: stderr is /dev/null in the daemonized shape)"
grep -q "uid $icecc_uid" "$workdir/daemon.log" || \
    fail "the warning does not name the dropped uid"
grep -q "$workdir/statedir/iceccd-state.jsonl" "$workdir/daemon.log" || \
    fail "the warning does not name the state path"
grep -qE "Permission denied|EACCES" "$workdir/daemon.log" || \
    fail "the warning does not name the errno"

# 2. WARN-AND-CONTINUE: the daemon is still alive (did not refuse to start)
kill -0 "$daemon_pid" 2>/dev/null || \
    fail "daemon exited after the state-jsonl warning (must continue: telemetry is not fatal)"

# 3. and it actually served its unix socket (continued, not wedged)
[ -S "$workdir/d.sock" ] || fail "no daemon unix socket (the daemon did not continue serving)"

kill "$daemon_pid" 2>/dev/null
echo "p50statejsonl-warn: OK (root-owned state dir: named warning with path+uid+errno, daemon continues)"
