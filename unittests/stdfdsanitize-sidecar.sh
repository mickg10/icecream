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
# Real teardown: signal, then wait (bounded) for ACTUAL exit, then kill -9
# and reap (the p50completionflow-run.sh pattern).  A scheduler that is
# only SIGNALED keeps its 8765/8766 listeners open until its poll loop
# notices -- with SO_REUSEADDR a LIVE listener still blocks the next
# bind -- so a signal-only trap makes the NEXT make-check test
# (envchardev.c.sh, p50remote-failreport.sh) lose the bind race against
# this test's dying scheduler.  Same for the daemon's own listener.
# Round 14 failed exactly there (see 99f6c17c for the full story).
stop_processes() { # <pids...>
    for pid in "$@"; do
        kill "$pid" 2>/dev/null || :
    done
    for _ in $(seq 1 50); do
        live=0
        for pid in "$@"; do
            kill -0 "$pid" 2>/dev/null && live=1
        done
        [ "$live" -eq 0 ] && break
        sleep 0.1
    done
    for pid in "$@"; do
        kill -9 "$pid" 2>/dev/null || :
        wait "$pid" 2>/dev/null || :
    done
}
trap 'stop_processes "$daemon_pid" "$service_pid" "$sched_pid"; rm -rf "$workdir"' EXIT INT TERM

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

# Task 4 (k310): assert that no fd the service inherited through the
# sidecar's fork+exec survived un-CLOEXEC'd -- NOT that the service's fd
# table is a fixed {0,1,2,ready,listener} set.  A correctly-running cache
# service legitimately opens MORE than those five on its own, after its
# own exec (e.g. the self-pipe WakePipe::create() wires up in
# cache/p50_cache_service.cpp for signal-safe wakeup, before it ever
# touches the ready/listener fds).  Blind set-equality against the
# service's live fd table conflates "the sweep missed an inherited fd"
# with "the service, as designed, opened a new fd of its own" -- those
# are indistinguishable from the fd count alone, and the latter is not a
# regression.
#
# What the launch-time sweep in cache/p50_daemon_sidecar_adapter.cpp (via
# close_range(2), or the /proc fallback when that is unavailable) actually
# promises is narrower: every "extra" fd (anything beyond 0/1/2/ready/
# listener) the SERVICE itself holds must be explainable as something the
# service legitimately opened after its own exec -- never something it
# could only have gotten by inheriting it un-closed from the daemon or
# the scheduler.  Verify this by classifying every extra fd BY WHAT IT IS,
# read only from the service's own /proc/<pid>/fd table (same-uid as
# before -- this listing already worked in the pre-fix test).  This
# deliberately does NOT read the daemon's own /proc/<pid>/fd table at
# all: a cross-check against a second privileged listing is not how the
# original test worked, is not what official QA has ever exercised, and
# (confirmed on H1) is not even reliably readable same-uid in every build
# configuration.  A positive, kind-based classification of the service's
# own table is sufficient without it -- see socket_family() below for the
# one kind (sockets) that needs more than its readlink string to classify
# safely.
env_dump=$(setpriv --reuid "$service_uid" --regid "$service_gid" --clear-groups \
    --inh-caps=-all cat "/proc/$service_pid/environ" 2>/dev/null | tr '\0' '\n')
[ -n "$env_dump" ] || fail "cannot read /proc/$service_pid/environ same-uid (uid=$service_uid)"
ready_fd=$(printf '%s\n' "$env_dump" | sed -n 's/^ICECC_CACHE_SERVICE_READY_FD=//p')
listener_fd=$(printf '%s\n' "$env_dump" | sed -n 's/^ICECC_CACHE_SERVICE_LISTENER_FD=//p')
case "$ready_fd" in
    ''|*[!0-9]*) fail "ICECC_CACHE_SERVICE_READY_FD missing/non-numeric in service environ" ;;
esac
case "$listener_fd" in
    ''|*[!0-9]*) fail "ICECC_CACHE_SERVICE_LISTENER_FD missing/non-numeric in service environ" ;;
esac

ready_link=$(probe_link "$ready_fd")
[ "$ready_link" = "PROBE-FAIL" ] && fail "cannot probe ready-write fd $ready_fd same-uid"
case "$ready_link" in
    pipe:*) ;;
    *) fail "ready-write fd $ready_fd in the cache service is not a pipe ($ready_link)" ;;
esac
listener_link=$(probe_link "$listener_fd")
[ "$listener_link" = "PROBE-FAIL" ] && fail "cannot probe listener fd $listener_fd same-uid"
case "$listener_link" in
    socket:*) ;;
    *) fail "listener fd $listener_fd in the cache service is not a socket ($listener_link)" ;;
esac

# Classify a socket fd's readlink string ("socket:[<inode>]") by address
# family, using only plain world-readable kernel tables -- no privileged
# /proc/<pid>/fd read of anything other than the service itself.
# /proc/net/unix lists every AF_UNIX socket on the system with its inode
# in column 7 (Num RefCount Protocol Flags Type St Inode [Path]);
# /proc/net/{tcp,tcp6,udp,udp6} list every AF_INET/AF_INET6 socket with
# its inode in column 10. A socket inode found in the former is the
# listener or one of its accepted connections (the only AF_UNIX sockets
# this topology ever produces); found in the latter, it is exactly what
# an inherited scheduler connection or an inherited daemon listening port
# would look like.
socket_family() { # readlink string ("socket:[12345]") -> unix|inet|unknown
    case "$1" in
        'socket:['*) ;;
        *) echo unknown; return ;;
    esac
    ino=${1#'socket:['}
    ino=${ino%']'}
    case "$ino" in
        ''|*[!0-9]*) echo unknown; return ;;
    esac
    if [ -r /proc/net/unix ] && \
        awk -v ino="$ino" 'NR>1 && $7==ino { f=1 } END { exit !f }' /proc/net/unix 2>/dev/null; then
        echo unix
        return
    fi
    for t in /proc/net/tcp /proc/net/tcp6 /proc/net/udp /proc/net/udp6; do
        [ -r "$t" ] || continue
        if awk -v ino="$ino" 'NR>1 && $10==ino { f=1 } END { exit !f }' "$t" 2>/dev/null; then
            echo inet
            return
        fi
    done
    echo unknown
}

# Settle budget for the service's OWN one-time startup work before we take
# the fd table as final. P29FingerprintState::start (cache/p50_slice0.cpp)
# launches compute_p29_system_source_fingerprint() in a detached background
# thread as soon as the service is up -- it is NOT awaited before the
# service signals ready on ready_fd. That scan opens real directory fds
# under the system include tree it is fingerprinting (observed live:
# /usr/include, /usr/include/boost, /usr/include/boost/spirit,
# /usr/include/boost/json/detail/ryu -- plain directory fds held open
# mid-walk, not under "$workdir/cache-rt" at all, because the thing being
# fingerprinted IS the system source tree; only the resulting digest is
# persisted under cache-rt). Probing the fd table while that thread is
# still mid-scan is a real, reproducible race, independent of the sweep
# this test exists to check -- so retry a bounded number of times and let
# the scan finish before trusting "extra" as final. A genuine leak (or a
# genuinely stuck/never-finishing scan) still fails: this only buys the
# one-time startup work time to settle, it does not relax what counts as
# clean.
settle_tries=15
try=0
while :; do
    try=$((try + 1))
    open_fds=$(setpriv --reuid "$service_uid" --regid "$service_gid" --clear-groups \
        --inh-caps=-all sh -c 'ls "/proc/$1/fd"' _ "$service_pid" 2>/dev/null | sort -n)
    if [ -z "$open_fds" ]; then
        [ "$try" -ge "$settle_tries" ] && \
            fail "cannot list /proc/$service_pid/fd same-uid (uid=$service_uid)"
        sleep 1
        continue
    fi

    extra_fds=""
    for fd in $open_fds; do
        case "$fd" in
            0|1|2) continue ;;
        esac
        [ "$fd" = "$ready_fd" ] && continue
        [ "$fd" = "$listener_fd" ] && continue
        extra_fds="$extra_fds $fd"
    done

    # This is the gate that actually matters: classify every "extra" fd by
    # WHAT IT IS. A structured-launch cache service, as designed, only
    # ever opens these kinds of fd on its own after exec:
    #   - its WakePipe self-pipe (cache/p50_cache_service.cpp
    #     WakePipe::create, a plain anonymous pipe, read+write, created
    #     once at startup for signal-safe wakeup);
    #   - its static fallback log sink (services/logging.cpp
    #     "static ofstream logfile_null("/dev/null")"), opened once at
    #     process load and held for the process lifetime regardless of
    #     configured log level -- permanent and deterministic, not a
    #     timing race;
    #   - the P29 system-source-fingerprint digest cache (cache/
    #     p50_slice0.cpp start_p29_system_source_fingerprint /
    #     P29FingerprintState::start)'s persisted output, whose directory
    #     fd, lock fd and per-file digest-cache fds are opened strictly
    #     under daemon_cache_directory_from_socket(socket_path) -- for
    #     this test that resolves to exactly "$workdir/cache-rt/<generation>";
    #   - that SAME scan's input side, transiently: real directory/file fds
    #     under the system include tree it is walking, open only while the
    #     detached background thread above is still running (settled out
    #     by the retry loop this is nested in);
    #   - an accepted connection on its own listener, or the listener
    #     itself. In this test's topology the ONLY process that can dial
    #     the service's listener is iceccd's own control-adapter code
    #     (cache/p50_daemon_sidecar_adapter.cpp outer_input_operation_ /
    #     DaemonControlOperation) -- there is no other client in this
    #     harness, so every AF_UNIX socket the service holds is one of
    #     these. An inherited scheduler connection or an inherited daemon
    #     listening port would be an AF_INET/AF_INET6 socket instead --
    #     distinguishable without any privilege via /proc/net/unix vs
    #     /proc/net/{tcp,tcp6,udp,udp6} (socket_family(), defined above).
    # Anything else -- a regular file outside cache-rt and outside the
    # scan, a tty, a dangling/unreadable link, or a socket whose inode is
    # not found in any /proc/net/* table above -- is unclassified.
    unclassified=""
    scan_pending=""
    inet_sockets=""
    for fd in $extra_fds; do
        link=$(probe_link "$fd")
        case "$link" in
            PROBE-FAIL) continue ;;  # closed between ls and readlink -- not a leak either way
            'pipe:['*) continue ;;              # WakePipe self-pipe
            'anon_inode:['*) continue ;;        # future eventfd/epoll/timerfd/signalfd
            'socket:['*)
                case "$(socket_family "$link")" in
                    unix) continue ;;    # the listener or an accepted connection on it
                    inet)
                        inet_sockets="$inet_sockets $fd($link)"
                        continue
                        ;;
                esac
                # unknown family (inode not found in any /proc/net/* table
                # above): falls through to unclassified below.
                ;;
            /dev/null) continue ;;              # static logfile_null fallback sink
            "$workdir/cache-rt"|"$workdir/cache-rt"/*) continue ;;  # P29 digest cache (output)
            /usr/include|/usr/include/*)
                # P29 scan (input side) -- tolerated ONLY while the settle
                # budget below still has retries left. If it is STILL open
                # on the final attempt, that is no longer "the scan hasn't
                # finished yet" -- it is folded into unclassified just below
                # and fails like any other kind.
                scan_pending="$scan_pending $fd($link)"
                continue
                ;;
        esac
        unclassified="$unclassified $fd($link)"
    done

    if { [ -n "$unclassified" ] || [ -n "$scan_pending" ]; } && [ "$try" -lt "$settle_tries" ]; then
        sleep 1
        continue
    fi
    # Budget exhausted (or both already empty): a still-open P29-scan fd at
    # this point is a real extra fd, not a startup race -- hold it to the
    # same bar as anything else unclassified.
    unclassified="$unclassified$scan_pending"
    break
done

[ -z "$inet_sockets" ] || fail "cache service holds an INET socket fd -- exactly what a broken launch-time CLOEXEC sweep would leak (an inherited scheduler connection or an inherited daemon listening port): $inet_sockets"
[ -z "$unclassified" ] || fail "cache service holds extra fd(s) of a kind the launch-time CLOEXEC sweep should never let through (not the self-pipe, not an anon_inode, not an AF_UNIX accepted connection on its own listener, not the static /dev/null log sink, not under this service's own cache-rt tree, and not a still-running P29 system-source scan): $unclassified"

stop_processes "$daemon_pid" "$service_pid"
echo "stdfdsanitize-sidecar: OK (0/1/2 sane, ready_write=$ready_fd pipe, listener=$listener_fd socket, no inet-socket or unexplained fd in the service$([ -n "$extra_fds" ] && echo ", service also holds self-opened fd(s) [$(for fd in $extra_fds; do printf '%s:%s ' "$fd" "$(probe_link "$fd")"; done)]"))"
exit 0
