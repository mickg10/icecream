#!/usr/bin/env bash
set -uo pipefail

usage() {
    echo "usage: run-qa.sh bootstrap|qa [jobs (default: 2)]" >&2
}

MODE=${1:-}
JOBS=${2:-${JOBS:-2}}
if [[ $# -gt 2 || ( "$MODE" != bootstrap && "$MODE" != qa ) ]]; then
    usage
    exit 2
fi
case "$JOBS" in
    ''|*[!0-9]*) echo "jobs must be an integer from 1 to 8" >&2; exit 2 ;;
esac
if (( JOBS < 1 || JOBS > 8 )); then
    echo "jobs must be an integer from 1 to 8" >&2
    exit 2
fi

SOURCE_MOUNT=/source
WORK_ROOT=/work
if [[ ! -d "$SOURCE_MOUNT" || ! -r "$SOURCE_MOUNT" ]]; then
    echo "required read-only source mount /source is missing or unreadable" >&2
    exit 2
fi
source_mount_options=$(findmnt -n -o OPTIONS --target "$SOURCE_MOUNT" 2>/dev/null || true)
case ",$source_mount_options," in
    *,ro,*) ;;
    *) echo "source snapshot at /source must be mounted read-only" >&2; exit 2 ;;
esac
if [[ ! -d "$WORK_ROOT" || ! -w "$WORK_ROOT" ]]; then
    echo "required writable scratch mount /work is missing or unwritable; no fallback is used" >&2
    exit 2
fi

# Native checks may need root inside Docker, but their retained output must
# remain removable by the host user. Never follow symlinks outside this mount.
if [[ -n "${ICEFARM_OUTPUT_UID:-}" || -n "${ICEFARM_OUTPUT_GID:-}" ]]; then
    if [[ ! "${ICEFARM_OUTPUT_UID:-}" =~ ^[0-9]+$ || ! "${ICEFARM_OUTPUT_GID:-}" =~ ^[0-9]+$ ]]; then
        echo "output ownership requires numeric UID and GID" >&2
        exit 2
    fi
    trap 'result=$?; chown -R --no-dereference "$ICEFARM_OUTPUT_UID:$ICEFARM_OUTPUT_GID" "$WORK_ROOT" || result=1; exit "$result"' EXIT
fi

SRC="$WORK_ROOT/source"
BUILD="$WORK_ROOT/build"
INSTALL="$WORK_ROOT/install"
TMP="$WORK_ROOT/tmp"
ARTIFACTS="$WORK_ROOT/artifacts"
for path in "$SRC" "$BUILD" "$INSTALL" "$TMP" "$ARTIFACTS" \
    "$WORK_ROOT/uv-cache" "$WORK_ROOT/python-env"; do
    if [[ -L "$path" ]]; then
        echo "refusing symlinked work path: $path" >&2
        exit 2
    fi
done
mkdir -p "$TMP" "$ARTIFACTS" || exit 2
if [[ ! -w "$TMP" || ! -w "$ARTIFACTS" ]]; then
    echo "/work/tmp and /work/artifacts must be writable" >&2
    exit 2
fi

# Unix socket paths have a small fixed length limit. /tmp is a short alias,
# not fallback storage: the wrapper must bind the very same owned scratch
# directory at both /work/tmp and /tmp.
if [[ $(stat -c '%d:%i' "$TMP") != $(stat -c '%d:%i' /tmp) ]]; then
    echo "bind /work/tmp at /tmp too; temporary objects must stay in the selected scratch directory" >&2
    exit 2
fi
TMP=/tmp

export TMPDIR="$TMP" TMP="$TMP" TEMP="$TMP" TEMPDIR="$TMP"
export ICEFARM_TMPDIR="$TMP"
export PYTHONDONTWRITEBYTECODE=1
export PYTHONPYCACHEPREFIX="$TMP/python-cache"
export UV_PYTHON_INSTALL_DIR=/opt/uv-python
export UV_CACHE_DIR="$WORK_ROOT/uv-cache"
export UV_PROJECT_ENVIRONMENT="$WORK_ROOT/python-env"
export VIRTUAL_ENV="$UV_PROJECT_ENVIRONMENT"
export PATH="$VIRTUAL_ENV/bin:$PATH"
export UV_OFFLINE=1
export UV_PYTHON_DOWNLOADS=never

if [[ ! -e "$SRC" ]]; then
    stage_dir=$(mktemp -d "$WORK_ROOT/.source.staging.XXXXXX") || exit 2
    if ! cp -a "$SOURCE_MOUNT"/. "$stage_dir"/; then
        echo "failed to stage source snapshot from /source; partial copy: $stage_dir" >&2
        exit 2
    fi
    if ! mv "$stage_dir" "$SRC"; then
        echo "failed to publish staged source snapshot at $SRC; copy remains at $stage_dir" >&2
        exit 2
    fi
elif [[ ! -d "$SRC" ]]; then
    echo "/work/source already exists and is not a directory" >&2
    exit 2
else
    echo "Using existing staged snapshot at $SRC (use a fresh /work for another snapshot)."
fi

# The legacy product source (the mixed-version QA baseline) predates the
# Python tooling and is only built natively.
native_only=0
if [[ "$MODE" == bootstrap && ! -e "$SRC/pyproject.toml" ]]; then
    native_only=1
fi

# The managed Python environment was resolved from this exact metadata during
# SDK image construction. Refuse to run a source snapshot with a different
# lockfile or interpreter pin; rebuilding the SDK is required instead.
SDK_METADATA_ROOT=/opt/icecream-python-src
for metadata in pyproject.toml uv.lock .python-version; do
    (( native_only )) && break
    if [[ ! -f "$SRC/$metadata" || ! -f "$SDK_METADATA_ROOT/$metadata" ]] ||
       ! cmp -s "$SRC/$metadata" "$SDK_METADATA_ROOT/$metadata"; then
        echo "SDK Python metadata mismatch for $metadata; rebuild the SDK from this source snapshot" >&2
        exit 2
    fi
done
mkdir -p "$BUILD" "$INSTALL" || exit 2
mkdir -p "$WORK_ROOT/uv-cache" "$WORK_ROOT/python-env" || exit 2
if [[ -d /opt/uv-cache ]]; then
    cp -a /opt/uv-cache/. "$WORK_ROOT/uv-cache/" || exit 2
fi

# Match a normal developer checkout. Several P50 tests intentionally require
# a non-root service identity. Root remains only the container orchestrator.
run_as_user=
if [[ $(id -u) == 0 ]]; then
    run_as_user=nobody
    chown -R --no-dereference "$(id -u nobody):$(id -g nobody)" \
        "$SRC" "$BUILD" "$INSTALL" "$TMP" "$ARTIFACTS" \
        "$WORK_ROOT/uv-cache" "$WORK_ROOT/python-env" || exit 2
fi

autogen_status=null
configure_status=null
build_status=null
install_status=null
python_sync_status=null
native_check_status=null
native_proc_check_status=null
native_proc_setcap_status=null
native_e2e_setcap_status=null
native_e2e_clang_status=null
native_e2e_check_status=null
native_root_check_status=null
native_proc_root_check_status=null
pytest_status=null

run_stage() {
    local name=$1; shift
    local logfile="$ARTIFACTS/$name.log"
    local status
    local -a command=("$@")
    if [[ -n "$run_as_user" && "$name" != native-root-check && "$name" != native-e2e-setcap && "$name" != native-proc-setcap && "$name" != native-proc-root-check ]]; then
        command=(runuser -u "$run_as_user" -- "${command[@]}")
    fi
    printf '\n== %s ==\n' "$name"
    if "${command[@]}" >"$logfile" 2>&1; then
        status=0
    else
        status=$?
    fi
    cat "$logfile"
    printf '\n[%s exit=%s; log=%s]\n' "$name" "$status" "$logfile"
    case "$name" in
        autogen) autogen_status=$status ;;
        configure) configure_status=$status ;;
        build) build_status=$status ;;
        install) install_status=$status ;;
        python-sync) python_sync_status=$status ;;
        native-check) native_check_status=$status ;;
        native-proc-check|native-proc-check2) native_proc_check_status=$status ;;
        native-proc-setcap) native_proc_setcap_status=$status ;;
        native-e2e-setcap) native_e2e_setcap_status=$status ;;
        native-e2e-clang) native_e2e_clang_status=$status ;;
        native-e2e-check) native_e2e_check_status=$status ;;
        native-root-check) native_root_check_status=$status ;;
        native-proc-root-check) native_proc_root_check_status=$status ;;
        python-pytest) pytest_status=$status ;;
    esac
    # Per-stage test logs: the make-check native stages share ONE
    # unittests dir and each later stage OVERWRITES the per-test
    # .log/.trs and test-suite.log -- after the round only the LAST
    # stage's skip reasons survive (the k310 gate on H1 hit exactly
    # that: the native-check SKIP text for the e2e/root tests was
    # unrecoverable, native-check.log rolls up "SKIP: <name>" only).
    # Copy each stage's logs out the moment it finishes, from the tree
    # that stage actually CHECKED (native-proc-check2 checks the
    # forced-proc tree, not $BUILD).  Stages that run no make check
    # (setcap, the clang leg's direct script) keep no directory: a
    # stage dir carrying another stage's logs would be worse than none.
    # The copy is best-effort (|| true): a stage's verdict is its own
    # exit status, never the copy's.
    local stage_testdir=
    case "$name" in
        native-check|native-root-check|native-e2e-check) stage_testdir=$BUILD/unittests ;;
        native-proc-check2|native-proc-root-check) stage_testdir=$WORK_ROOT/tmp/build-proc/unittests ;;
    esac
    if [[ -n "$stage_testdir" && -d "$stage_testdir" ]]; then
        mkdir -p "$ARTIFACTS/$name" || true
        cp "$stage_testdir/"*.log "$ARTIFACTS/$name/" 2>/dev/null || true
        cp "$stage_testdir/"*.trs "$ARTIFACTS/$name/" 2>/dev/null || true
        cp "$stage_testdir/test-suite.log" "$ARTIFACTS/$name/" 2>/dev/null || true
    fi
    return 0
}

# Resolve the locked Python environment as the same unprivileged identity used
# for source and native QA. This is required in both bootstrap and QA modes
# (except for the native-only legacy baseline); no prebuilt root-owned
# environment is used as a fallback.
(( native_only )) ||
    run_stage python-sync bash -c 'cd "$1" && uv sync --locked --offline --managed-python --python "$(cat .python-version)"' _ "$SRC"

prepare_build() {
    run_stage autogen bash -c 'cd "$1" && ./autogen.sh' _ "$SRC"
    (( autogen_status == 0 )) || return 1
    run_stage configure bash -c 'cd "$1" && "$2/configure" --prefix="$3" --sbindir="$3/sbin" --without-man' _ "$BUILD" "$SRC" "$INSTALL"
    (( configure_status == 0 )) || return 1
    run_stage build make -C "$BUILD" -j"$JOBS" all
    (( build_status == 0 )) || return 1
    return 0
}

build_ok=0
if prepare_build; then
    build_ok=1
fi

if [[ "$MODE" == bootstrap ]]; then
    if (( build_ok )); then
        run_stage install make -C "$BUILD" install
    fi
else
    if (( build_ok && python_sync_status == 0 )); then
        run_stage install make -C "$BUILD" install
        run_stage native-check make -C "$BUILD" -j"$JOBS" check
        # Test what we SHIP: the f28-header builds (node image, rpm) have no
        # SYS_close_range from their distro headers, so their fd-hygiene
        # runs the proc_fallback backend in production -- a backend the
        # plain native-check above never exercises end to end on a modern
        # SDK.  Reconfigure a second tree with the QA knob and run the
        # fd-hygiene suite against it (~2-3 min: one configure + the
        # hygiene tests only; the full variant is an f28-SDK QA leg).
        # p50compilee2e-run.sh rides ALONG (it needs the same e2e gates as the
        # native-e2e-check stage below: the forced-proc tree is exactly the
        # shipped f28 path -- close_range compiled out, proc_fallback live).
        # an explicitly exported host wins (tests pin it); the container
        # default is its own bridge address
        e2e_host_ip=${ICECC_P50_C1F1_WORKER_SCHEDULER_HOST:-$(hostname -i 2>/dev/null | awk '{print $1}')}
        # No ICECC_TEST_DAEMON_UID/GID here: those are ROOT-caller plumbing
        # (chown + setuid in the tests) and this stage runs as the
        # unprivileged run_as_user -- the first live run EPERM'd on the chown
        # and failed compilee2e/completionflow before any daemon started.
        # Unprivileged, the tests leave it unset and run same-uid (their own
        # id -u path); the ROOT-side legs run in native-root-check instead.
        e2e_exports=(
            ICECC_TEST_SOURCE_ARM_LIVE=1
            ICECC_TEST_POSITIVE_DAEMON=1
            ICECC_TEST_REQUIRE_REMOTE=1
            "ICECC_P50_C1F1_WORKER_SCHEDULER_HOST=$e2e_host_ip"
            "ICECC_TEST_SCHED_ADDR=$e2e_host_ip"
            TMPDIR=/tmp
            ICEFARM_TMPDIR=/tmp
        )
        # The proc/e2e/setcap stages only mean something when the plain
        # native suite is green; do not burn ~10 min of forced-proc build
        # and a farm spin-up on a tree that already failed check.
        if [[ "$native_check_status" == 0 ]]; then
        # The forced-proc tree is BUILT here (unprivileged) and its
        # daemons are setcap'd by the root-exempt native-proc-setcap stage
        # BELOW before the check runs: the e2e remote-compile tests inside
        # it exercise the SHIPPED f28 backend, and without CAP_SYS_CHROOT
        # the worker daemon downgrades to --no-remote and the cache
        # advertisement stays absent by design -- the compilee2e READY
        # gate then fails deterministically (reproduced: caps stripped ->
        # rc=1 "not advertised READY" every time; caps present -> rc=0).
        run_stage native-proc-check env "${e2e_exports[@]}" bash -c \
            'set -e; mkdir -p "$1/tmp/build-proc"; cd "$1/tmp/build-proc"; "$2/configure" --without-man --enable-qa-force-proc-sweep >/dev/null 2>&1 || "$2/configure" --without-man --enable-qa-force-proc-sweep; make -C . -j"$3" >/dev/null' \
            _ "$WORK_ROOT" "$SRC" "$JOBS"
        if [[ "$native_proc_check_status" == 0 ]]; then
            run_stage native-proc-setcap bash -c '
                command -v setcap >/dev/null || { echo "proc-setcap: setcap missing (install libcap2-bin in the qa image)" >&2; exit 77; }
                for bin in "$1/tmp/build-proc/daemon/iceccd" "$1/tmp/build-proc/scheduler/icecc-scheduler"; do
                    [ -x "$bin" ] || { echo "proc-setcap-skip: missing $bin" >&2; exit 77; }
                    setcap cap_sys_chroot+ep "$bin" \
                        || { echo "proc-setcap-fail: could not setcap $bin" >&2; exit 1; }
                done
            ' _ "$WORK_ROOT"
            if [[ "$native_proc_setcap_status" == 0 ]]; then
                run_stage native-proc-check2 env "${e2e_exports[@]}" bash -c \
                    'set -e; cd "$1/tmp/build-proc"; make -C . check TESTS="p50forkfdhygiene p50forkfdhygiene-source.sh p50forkfdhygiene-mutants.sh p50forkfdhygiene-oot.sh p50forkfdhygiene-package.sh p50_fork_fd_hygiene_sanitize.sh p50compilee2e-run.sh"; ICECC_P50_C1F1_COMPILER=clang++ ICECC_P50_C1F1_COMPILER_REQUIRED=1 ICECC_P50_C1F1_TIMEOUT=300 ICECC_TEST_TOP_BUILDDIR="$PWD" ICECC_TEST_BUILDDIR="$PWD/unittests" bash "$2/unittests/p50compilee2e-run.sh"' \
                    _ "$WORK_ROOT" "$SRC"
                # Task 4 (k310): stdfdsanitize-sidecar.sh is root-gated (exit 77
                # under non-root) and native-proc-check2 above runs UNPRIVILEGED
                # -- so without this stage the script never once exercises the
                # forced-proc/fallback tree built just above, only the regular
                # $BUILD tree via native-root-check further down.  This is the
                # ONLY QA stage that proves the exact-fd-table assertion against
                # the --enable-qa-force-proc-sweep binaries that ship the
                # EPERM/ENOSYS/EINVAL -> /proc fallback path.  Exempt from
                # runuser (like native-root-check) since the script needs root.
                if [[ -n "$run_as_user" && "$native_proc_check_status" == 0 ]]; then
                    run_stage native-proc-root-check bash -c \
                        'set -e; cd "$1/tmp/build-proc"; make -C . check TESTS="stdfdsanitize-sidecar.sh"' \
                        _ "$WORK_ROOT"
                fi
            fi
        fi
        if [[ -n "$run_as_user" && "$native_check_status" == 0 ]]; then
            # Also exercise the service's explicit root-to-user transition,
            # including leak detection, rather than omitting that branch.
            # The two ROOT-side remote-compile tests ride along (they gate on
            # geteuid()==0 and skip in the unprivileged native-e2e-check).
            run_stage native-root-check env \
                ICECC_TEST_SOURCE_ARM_LIVE=1 ICECC_TEST_POSITIVE_DAEMON=1 \
                "ICECC_P50_C1F1_WORKER_SCHEDULER_HOST=$e2e_host_ip" \
                TMPDIR=/tmp ICEFARM_TMPDIR=/tmp \
                make -C "$BUILD/unittests" -j"$JOBS" \
                check TESTS="p50cacheservice p50cacheservice-sanitize.sh p50sourcearm-live-run.sh p50daemonpositive-run.sh stdfdsanitize-sidecar.sh envchardev.c.sh envstore-run.sh p50modeoff-run.sh p50remote-failreport.sh p50sidecarrefusal-run.sh p50statejsonl-warn.sh"
        fi
        # The six END-TO-END REMOTE-COMPILE tests skipped in every release QA
        # (they need root, a real non-loopback IP and the opt-in env gates) --
        # so no real remote compile ever ran in QA against the shipped shape.
        # The qa container IS root with a bridge IP and a short /tmp (the
        # sun_path-bound unix sockets need it); this stage only adds the gates.
        # Serial: completionflow is wall-clock-heavy and timing-sensitive.
        # The six remote-compile e2e tests (seven counting the preprocess-
        # first gate added to the TESTS list below) execute the BUILT
        # daemons as the unprivileged run_as_user; the worker daemon needs
        # CAP_SYS_CHROOT to accept remote compiles (chroot into the env),
        # which an unprivileged start cannot get by any other means.  setcap
        # the binaries as ROOT (this stage is exempt from runuser, like
        # native-root-check) so the tests themselves can run as nobody.
        # libcap-ng then drops to the test account inside, exactly as
        # production does from root.
        # Skip (not fail) when the built binaries are absent (a harness
        # dry-run) or setcap is missing from the image: the same e2e tests
        # themselves will fail loudly on a cap-less daemon, and a hard
        # exit here would mask THAT message with a setcap one.
        run_stage native-e2e-setcap bash -c '
            command -v setcap >/dev/null || { echo "setcap-setcap: setcap missing (install libcap2-bin in the qa image)" >&2; exit 77; }
            for bin in "$1/daemon/iceccd" "$1/scheduler/icecc-scheduler"; do
                [ -x "$bin" ] || { echo "setcap-skip: missing $bin" >&2; exit 77; }
                setcap cap_sys_chroot+ep "$bin" \
                    || { echo "setcap-fail: could not setcap $bin" >&2; exit 1; }
                getcap "$bin"
            done
        ' _ "$BUILD"
        if [[ "$native_e2e_setcap_status" == 77 ]]; then
            echo "native-e2e-setcap skipped (missing setcap or built binaries); the e2e tests will report the cap-less daemon themselves" >&2
        elif [[ "$native_e2e_setcap_status" != 0 ]]; then
            echo "native-e2e-check skipped: setcap FAILED (see native-e2e-setcap.log)" >&2
        fi
        if [[ "$native_e2e_setcap_status" == 0 && -n "$e2e_host_ip" && "$e2e_host_ip" != 127.0.0.1 ]]; then
            # Top-level check (not -C unittests): the completionflow test
            # needs client/check_PROGRAMS (icecc-p50-completion-test) and
            # unittests needs cache/check_LIBRARIES -- both are built by the
            # recursive check, never by plain `make all`.
            run_stage native-e2e-check env "${e2e_exports[@]}" \
                make -C "$BUILD" -j"$JOBS" check \
                TESTS="remoteice-quick.sh p50assignment-remote.sh p50sourcearm-live-run.sh p50daemonpositive-run.sh p50compilee2e-run.sh p50ppfirst-run.sh p50completionflow-run.sh"
            # The clang leg of p50compilee2e: the fd-defect class is
            # clang-only (gcc ignores closed std fds; clang's
            # FixupStandardFileDescriptors dies in a /dev/null-less chroot),
            # so a gcc-only e2e suite can NEVER catch it.  REQUIRED mode:
            # inside QA a missing tool FAILS the stage instead of skipping --
            # its detector stayed dark through every release run before.
            # The TESTS mechanism cannot run one test twice, so this is a
            # direct second invocation of the same script with the clang
            # env, same working dir shape as the make leg.
            run_stage native-e2e-clang env "${e2e_exports[@]}" \
                "ICECC_TEST_TOP_BUILDDIR=$BUILD" \
                "ICECC_TEST_BUILDDIR=$BUILD/unittests" \
                ICECC_P50_C1F1_COMPILER=clang++ \
                ICECC_P50_C1F1_COMPILER_REQUIRED=1 \
                ICECC_P50_C1F1_TIMEOUT=300 \
                bash "$SRC/unittests/p50compilee2e-run.sh"
        else
            echo "native-e2e-check skipped: no non-loopback container IP (hostname -i gave '$e2e_host_ip')" >&2
        fi
        fi  # native_check_status == 0 gate (proc/e2e/setcap stages)
    else
        echo "Native make check not run because build or offline uv sync failed; see stage logs." >&2
    fi

    # This suite is independent of native compilation and still runs on failure.
    if [[ "$python_sync_status" == 0 ]]; then
        run_stage python-pytest bash -c 'cd "$1" && command -v python3 && python3 -B -m pytest -q -p no:cacheprovider --junitxml="$2/pytest.xml" farmharness/integration/tests' _ "$SRC" "$ARTIFACTS"
    else
        echo "Python QA not run because offline uv sync failed; see python-sync stage log." >&2
    fi
fi

overall=0
for status in "$autogen_status" "$configure_status" "$build_status"; do
    [[ "$status" == 0 ]] || overall=1
done
if [[ "$MODE" == bootstrap ]]; then
    [[ "$install_status" == 0 ]] || overall=1
    (( native_only )) || [[ "$python_sync_status" == 0 ]] || overall=1
else
    [[ "$install_status" == 0 ]] || overall=1
    [[ "$native_check_status" == 0 && "$build_ok" == 1 ]] || overall=1
    [[ "$native_proc_check_status" == 0 ]] || overall=1
    [[ "$native_e2e_check_status" == 0 || "$native_e2e_check_status" == null ]] || overall=1  # null = loud-skipped (no container IP); a skip must not fake green
    [[ "$native_e2e_clang_status" == 0 || "$native_e2e_clang_status" == null ]] || overall=1  # the clang leg is REQUIRED in qa: 77/skip must fail the run
    [[ -z "$run_as_user" || "$native_root_check_status" == 0 ]] || overall=1
    [[ -z "$run_as_user" || "$native_proc_root_check_status" == 0 ]] || overall=1
    [[ "$pytest_status" == 0 ]] || overall=1
fi

summary_tmp="$ARTIFACTS/summary.json.tmp"
printf '{"mode":"%s","source_commit":"%s","jobs":%s,"autogen_exit":%s,"configure_exit":%s,"build_exit":%s,"install_exit":%s,"python_sync_exit":%s,"native_check_exit":%s,"native_proc_check_exit":%s,"native_e2e_clang_exit":%s,"native_e2e_check_exit":%s,"native_root_check_exit":%s,"native_proc_root_check_exit":%s,"pytest_exit":%s,"overall_exit":%s}\n' \
    "$MODE" "${ICECC_QA_SOURCE_COMMIT:-unknown}" "$JOBS" "$autogen_status" "$configure_status" "$build_status" \
    "$install_status" "$python_sync_status" "$native_check_status" "$native_proc_check_status" "$native_e2e_clang_status" "$native_e2e_check_status" "$native_root_check_status" "$native_proc_root_check_status" "$pytest_status" "$overall" > "$summary_tmp"
mv -f "$summary_tmp" "$ARTIFACTS/summary.json"
printf '\nSummary: %s/summary.json\nArtifacts: %s\n' "$ARTIFACTS" "$ARTIFACTS"
exit "$overall"
