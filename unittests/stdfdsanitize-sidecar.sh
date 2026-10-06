#!/bin/sh
# SIDECAR STD-FD regression (the remote-compile fd bug, virtu-fdfix) -- the
# cache-service leg of the stdfd story.
#
# The daemon may start with fds 1/2 closed (container/supervisor).  The cache
# sidecar (cache/p50_daemon_sidecar_adapter.cpp) forks and execve's the
# icecc-cache-service WITHOUT touching 0/1/2 (it closes only its own owned
# fds and close_range(3,..) is CLOEXEC-only) -- the service inherits the
# daemon's std fds verbatim.  So whatever the daemon's std fds look like,
# the service's must look the same, and after the startup sanitize that
# means /dev/null (or a real inherited stream), NEVER a socket or a pipe
# end (a squatted listener/pipe on a std slot surfacing in the child).
#
# This leg runs the daemon as ROOT (production shape: the cache adapter
# refuses non-root/no-CAP_SYS_CHROOT configs -- "invalid cache sidecar
# configuration"), with the env dir pre-chowned to the icecc user so the
# capng drop does not fail its chown.  The daemon drops to icecc before
# configure_cache_adapter(), so the service child runs as icecc -- and the
# fd probe runs same-uid via setpriv (container-root has no CAP_SYS_PTRACE:
# a root readlink of an icecc-owned /proc/<pid>/fd is EPERM).
#
# GREEN-only regression (verified both colors): at c8bb615f the service
# child's std-fd state is TIMING-DEPENDENT -- the daemon's fd 2 is a
# revolving door of transient opens (capng status reads, sockets), and the
# service's OWN static logfile_null("/dev/null") init re-fills whichever
# std slot happened to be closed at its exec, masking whatever it
# inherited.  With the fix the daemon pins 0/1/2 open before anything
# else runs, so the service's inheritance is DETERMINISTIC -- that is what
# this leg asserts (open, /dev/null or a real stream, never a socket).
src_dir=$(dirname "$0")
build_dir=${ICECC_TEST_BUILDDIR:-$src_dir}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$build_dir/..}
ICECCD=${1:-$top_build_dir/daemon/iceccd}
CACHE_SERVICE=${CACHE_SERVICE:-$top_build_dir/cache/icecc-cache-service}
SCHEDULER=${SCHEDULER:-$top_build_dir/scheduler/icecc-scheduler}

fail() { echo "stdfdsanitize-sidecar: FAIL: $*" 1>&2; exit 1; }

# Root-gated like the two root-side e2e tests: the cache adapter refuses
# non-root daemon configs, so this leg cannot run unprivileged.  Skip (77)
# under the unprivileged native-check and run it from native-root-check.
if [ "$(id -u)" != "0" ]; then
    echo "stdfdsanitize-sidecar: skipping (needs root: the cache adapter refuses non-root daemons)" >&2
    exit 77
fi

icecc_user=""
if id -u icecc >/dev/null 2>&1; then icecc_user=icecc; fi

workdir=$(mktemp -d /tmp/stdfdsc.XXXXXX) || fail mktemp
mkdir -p "$workdir/envs"
mkdir -m 0700 "$workdir/cache-rt"
if [ -n "$icecc_user" ]; then
    chown -R "$icecc_user" "$workdir" || fail "cannot chown $workdir to $icecc_user"
    # The adapter requires the runtime dir PRIVATE to the post-drop uid:
    # exactly icecc:icecc 0700 (valid_config -> private_directory).
    chown "$icecc_user:$icecc_user" "$workdir/cache-rt"
    chmod 0700 "$workdir/cache-rt"
fi
trap 'kill $daemon_pid 2>/dev/null; kill $service_pid 2>/dev/null; kill $sched_pid 2>/dev/null; rm -rf "$workdir"' EXIT INT TERM

cat >"$workdir/closefd-helper.sh" <<'HELPER'
#!/bin/sh
exec 1<&- 2<&-
exec "$@"
HELPER
chmod +x "$workdir/closefd-helper.sh"

# A scheduler on the same netname first: the cache sidecar only launches in
# positive mode, i.e. once the daemon has a scheduler (the daemon logs
# "scheduler not yet found/selected" and spawns no service without one).
HOME="$workdir" "$workdir/closefd-helper.sh" "$SCHEDULER" \
    -v -n stdfdscnet -l "$workdir/sched.log" &
sched_pid=$!
sleep 2

# HOME pinned to the workdir: the unix-listener bind at $HOME/.iceccd.socket
# is fatal when HOME is not writable by the (dropped) uid.  NO -l logfile:
# with 1/2 closed, the pre-main logfile_null static takes fd 1 and the log
# open takes fd 2 -- masking the RED (the service would inherit the daemon's
# log FILE on fd 2 instead of the squatted listener socket it should catch).
HOME="$workdir" "$workdir/closefd-helper.sh" "$ICECCD" \
    -v -n stdfdscnet -b "$workdir/envs" \
    --cache-service "$CACHE_SERVICE" --cache-runtime-dir "$workdir/cache-rt" &
daemon_pid=$!

# Wait for the service child (exec'd by the sidecar adapter, same uid as
# the dropped daemon).  Readiness budget: the adapter waits up to 5s for
# the service's ready marker.  Select by ppid == daemon: a plain
# pgrep -f ALSO matches the daemon itself (its own --cache-service argv)
# and this script (the service path in its argv), both of which carry
# legitimate non-/dev/null std fds -- the earlier probe hit exactly that.
service_pid=""
for try in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    sleep 1
    kill -0 "$daemon_pid" 2>/dev/null || \
        fail "daemon exited early: $(tail -3 "$workdir/daemon.log" 2>/dev/null)"
    service_pid=$(pgrep -P "$daemon_pid" | head -n1)
    [ -n "$service_pid" ] && break
done
[ -n "$service_pid" ] || fail "cache service never started (20s)"

# Probe the SERVICE child's std fds, same-uid (setpriv drops to the
# service's uid; container-root cannot read a non-root /proc fd table).
probe_link() { # fd -> prints the readlink or FAIL
    fd=$1
    setpriv --reuid "$service_uid" --regid "$service_gid" --clear-groups \
        --inh-caps=-all readlink "/proc/$service_pid/fd/$fd" 2>/dev/null || \
        echo "PROBE-FAIL"
}
service_uid=$(stat -c %u "/proc/$service_pid" 2>/dev/null) || fail "no /proc/$service_pid"
service_gid=$(stat -c %g "/proc/$service_pid" 2>/dev/null) || fail "no /proc/$service_pid"

for fd in 0 1 2; do
    link=$(probe_link "$fd")
    [ "$link" = "PROBE-FAIL" ] && fail "cannot probe fd $fd same-uid (uid=$service_uid)"
    [ -n "$link" ] || fail "fd $fd is CLOSED in the cache service (inherited a closed std fd from a daemon that lost its std fds)"
    case "$link" in
        socket:*) fail "fd $fd in the cache service is a SOCKET ($link) -- squatted listener inherited through the sidecar exec" ;;
        pipe:*) fail "fd $fd in the cache service is a PIPE ($link) -- squatted pipe end inherited through the sidecar exec" ;;
        /dev/null) ;;
        /dev/*|/*) ;;  # a real inherited stream (the daemon's own logfile etc.)
        *) fail "fd $fd in the cache service is unexpected: $link" ;;
    esac
done

kill "$daemon_pid" 2>/dev/null
kill "$service_pid" 2>/dev/null
echo "stdfdsanitize-sidecar: OK (cache service keeps 0/1/2 open, no socket/pipe)"
exit 0
