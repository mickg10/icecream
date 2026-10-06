#!/bin/sh
# STD-FD SANITIZE regression (the remote-compile fd bug, virtu-fdfix).
#
# iceccd can be started with fds 1/2 closed (a container or supervisor closing
# its own std fds before exec).  Every open() then squats onto the closed
# descriptors: with 1/2 closed the -l logfile itself lands on fd 1, and any
# pipe the daemon makes fills the remaining gaps -- workit's pipe(sock_err)
# lands exactly on the remaining std slot, exec-ing the compiler with
# stdout/stderr closed (in a chroot without /dev/null, clang's
# FixupStandardFileDescriptors dies instantly).
#
# RED before the fix: iceccd started with 1/2 closed keeps at least one of
# them closed (checked via /proc/<pid>/fd) -- fd 2 stays closed here,
# because fd 1 is accidentally re-filled at startup by the static
# ofstream logfile_null("/dev/null") initializer in logging.cpp.
# GREEN after: icecc::sanitize_standard_fds() (first thing in main) re-opens
# host-fs /dev/null onto every closed std fd, so /proc/<pid>/fd lists 0, 1
# and 2 all open, and the first pipe the daemon makes is >= 3.
#
# The daemon drops to the icecc user when started as root, and /proc/<pid>/fd
# is only readable by the same uid (root without CAP_SYS_PTRACE gets EPERM) --
# so the WHOLE test re-execs itself as that user (setpriv) and the daemon is
# probed from the same uid it runs as.  No -u flag, no logfile (-l): the test
# runs the daemon in the foreground; a logfile would re-fill fd 1 in RED and
# mask the regression.
src_dir=$(dirname "$0")
build_dir=${ICECC_TEST_BUILDDIR:-$src_dir}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$build_dir/..}
ICECCD=${1:-$top_build_dir/daemon/iceccd}

fail() { echo "stdfdsanitize: FAIL: $*" 1>&2; exit 1; }

# Re-exec as the icecc user when started as root (the probe uid == daemon uid).
if [ "$(id -u)" = "0" ]; then
    if id -u icecc >/dev/null 2>&1; then
        exec setpriv --reuid icecc --regid icecc --clear-groups \
            --inh-caps=-all "$0" "$@"
    fi
    echo "stdfdsanitize: skipping (no icecc user, not root-reexecable)" >&2
    exit 77  # skipped
fi

# A dir the daemon can chdir to; iceccd wants a writable base for its state.
workdir=$(mktemp -d /tmp/stdfdtest.XXXXXX) || fail mktemp
mkdir -p "$workdir/envs" "$workdir/cache"
trap 'kill $daemon_pid 2>/dev/null; rm -rf "$workdir"' EXIT INT TERM

# Start iceccd with fds 1 and 2 CLOSED (0 stays open): a tiny helper that
# closes its OWN 1/2 then execs the daemon.  (A shell redirection on the
# daemon itself would race the helper's own close.)
#
# HOME must point somewhere the running uid owns: iceccd binds its local
# listener at $HOME/.iceccd.socket and a bind failure is fatal (exit 1)
# -- as root this is /root (writable); re-execed as icecc the inherited
# HOME=/root is not, so pin it to the workdir.
cat >"$workdir/closefd-helper.sh" <<'HELPER'
#!/bin/sh
exec 1<&- 2<&-
exec "$@"
HELPER
chmod +x "$workdir/closefd-helper.sh"
HOME="$workdir" "$workdir/closefd-helper.sh" "$ICECCD" \
    -v -n stdfdtestnet -b "$workdir/envs" &
daemon_pid=$!

# Wait for the daemon to appear, then read its fd table.  The daemon must have
# been RUNNING for at least one scheduling turn so the startup code completed.
ok=0
for try in 1 2 3 4 5 6 7 8 9 10; do
    sleep 1
    kill -0 "$daemon_pid" 2>/dev/null || fail "daemon exited early"
    # /proc/<pid>/fd/N is a symlink iff N is open -- test existence directly.
    [ -L "/proc/$daemon_pid/fd/0" ] || { ok=0; continue; }
    [ -L "/proc/$daemon_pid/fd/1" ] || { ok=0; continue; }
    [ -L "/proc/$daemon_pid/fd/2" ] || { ok=0; continue; }
    ok=1
    break
done
[ "$ok" = 1 ] || fail "std fds not all open in the daemon (/proc/$daemon_pid/fd)"

# The restored std fds must be /dev/null (the sanitizer's choice) and not a
# pipe or a socket of the daemon's own making (which would be a squat).
link1=$(readlink "/proc/$daemon_pid/fd/1") || fail "no fd 1"
link2=$(readlink "/proc/$daemon_pid/fd/2") || fail "no fd 2"
case "$link1" in
    /dev/null) ;;
    *) fail "fd 1 is not /dev/null: $link1" ;;
esac
case "$link2" in
    /dev/null) ;;
    *) fail "fd 2 is not /dev/null: $link2" ;;
esac

# The daemon's own descriptors must all live at >= 3 now that the std slots
# are open: prove the pipes/sockets it opened during startup found free slots
# above std (nothing had to squat).  /proc/<pid>/fd lists only OPEN fds, and
# 0/1/2 are the std slots themselves -- the listener socket is what we look
# for; a daemon with ONLY 0/1/2 open would have failed to bind (that bind
# is fatal and would have exited before this point).
found_high=0
for fd in $(ls "/proc/$daemon_pid/fd" 2>/dev/null); do
    if [ "$fd" -ge 3 ]; then found_high=1; fi
done
[ "$found_high" = 1 ] || fail "daemon has no fd >= 3 (no listener socket?)"

kill "$daemon_pid" 2>/dev/null
echo "stdfdsanitize: OK (daemon keeps 0/1/2 open when started with 1/2 closed)"
exit 0
# The cache-service (sidecar) leg lives in stdfdsanitize-sidecar.sh: the
# cache adapter refuses non-root daemon configs ("invalid cache sidecar
# configuration"), so that leg runs the production shape (root-started
# daemon, capng drop) and probes the service child same-uid via setpriv.
