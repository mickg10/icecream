#!/usr/bin/env bash
set -uo pipefail

usage() {
    echo "usage: run-gate.sh {p51-arm-expiry|p51-restart-w30|p51-scheduler-restart-w30|p51-scheduler-f-restart-w30|p51-restart-chain-w30}" >&2
}

if [[ $# -ne 1 ]]; then
    usage
    exit 2
fi

case "$1" in
    p51-arm-expiry)
        gate=$1
        target=p50daemonpositive-p51-arm-expiry-check
        timeout_s=240
        marker=P51_ARM_EXPIRY_WIRE_PASS=
        expected_markers=3
        ;;
    p51-restart-w30)
        gate=$1
        target=p50daemonpositive-p51-restart-w30-check
        timeout_s=4200
        marker=P51_RESTART_W30_PASS=
        expected_markers=18
        ;;
    p51-scheduler-restart-w30)
        gate=$1
        target=p51schedulerrestart-w30-check
        timeout_s=1800
        marker=P51_REAL_SCHEDULER_RESTART_W30_PASS\ profile=
        expected_markers=3
        ;;
    p51-scheduler-f-restart-w30)
        gate=$1
        target=p51schedulerrestart-w30-check
        timeout_s=1800
        marker=P51_REAL_SCHEDULER_F_RESTART_CHAIN_W30_PASS\ profile=
        expected_markers=3
        ;;
    p51-restart-chain-w30)
        gate=$1
        target=p50daemonpositive-p51-restart-chain-w30-check
        timeout_s=1200
        marker=P51_RESTART_CHAIN_PASS=F_then_C/
        expected_markers=3
        ;;
    *)
        echo "FAIL: unsupported opt-in gate: $1" >&2
        usage
        exit 2
        ;;
esac

if [[ $(id -u) -ne 0 || ( ! -e /.dockerenv && ! -e /run/.containerenv ) ]]; then
    echo "FAIL: opt-in process gates require root in a disposable container" >&2
    exit 2
fi
if [[ ! ${ICECREAM_GATE_RUN_ID:-} =~ ^[a-f0-9]{32}$ ]]; then
    echo "FAIL: missing unique gate run identity" >&2
    exit 2
fi
for variable in ICEFARM_OUTPUT_UID ICEFARM_OUTPUT_GID; do
    if [[ ! ${!variable:-} =~ ^[0-9]+$ ]]; then
        echo "FAIL: $variable must be a numeric host identity" >&2
        exit 2
    fi
done
for executable in groupadd useradd getent timeout make chown stat python3; do
    command -v "$executable" >/dev/null 2>&1 || {
        echo "FAIL: required gate utility is missing: $executable" >&2
        exit 2
    }
done

if [[ ! -d /source || ! -r /source || ! -d /work/build/unittests ||
      ! -d /work/tmp || -L /work/tmp || ! -w /work/tmp || ! -d /tmp || ! -w /tmp ||
      ! -d /work/uv-cache || ! -d /work/python-env || ! -d /opt/uv-python ]]; then
    echo "FAIL: gate source/build/scratch mounts are incomplete" >&2
    exit 2
fi
if [[ $(stat -c '%d:%i' /work/tmp) != $(stat -c '%d:%i' /tmp) ]]; then
    echo "FAIL: /tmp must be the explicit short alias of /work/tmp" >&2
    exit 2
fi

# The SDK intentionally stays source-free and does not create test identities.
# Add this account only inside the disposable gate container.
if ! getent group icecc >/dev/null 2>&1; then
    groupadd --system icecc || {
        echo "FAIL: cannot create disposable icecc group" >&2
        exit 2
    }
fi
if ! getent passwd icecc >/dev/null 2>&1; then
    useradd --system --gid icecc --home-dir /nonexistent --no-create-home \
        --shell /usr/sbin/nologin icecc || {
        echo "FAIL: cannot create disposable icecc account" >&2
        exit 2
    }
fi

export ICEFARM_TMPDIR=/tmp TMPDIR=/tmp TMP=/tmp TEMP=/tmp TEMPDIR=/tmp
export PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1
export UV_PROJECT_ENVIRONMENT=/work/python-env VIRTUAL_ENV=/work/python-env
export UV_CACHE_DIR=/work/uv-cache UV_PYTHON_INSTALL_DIR=/opt/uv-python
export UV_OFFLINE=1 UV_PYTHON_DOWNLOADS=never
export PATH="$VIRTUAL_ENV/bin:$PATH"
export PYTHONPATH=/work/source
log_root=/work/artifacts/opt-in-gates
run_dir="$log_root/$ICECREAM_GATE_RUN_ID"
if [[ -e "$run_dir" || -L "$run_dir" ]]; then
    echo "FAIL: refusing to reuse gate artifact directory" >&2
    exit 2
fi
mkdir -p "$run_dir" || exit 2
finish() {
    status=$?
    trap - EXIT
    for owned_path in "$run_dir" /work/tmp /work/uv-cache /work/python-env; do
        if [[ -e "$owned_path" && ! -L "$owned_path" ]] &&
           ! chown -R --no-dereference "$ICEFARM_OUTPUT_UID:$ICEFARM_OUTPUT_GID" "$owned_path"; then
            status=1
        fi
    done
    exit "$status"
}
trap finish EXIT

log="$run_dir/$gate.log"
status_file="$run_dir/$gate.exit"
echo "GATE_START name=$gate target=$target timeout_s=$timeout_s run_id=$ICECREAM_GATE_RUN_ID"
set +e
if [[ "$gate" == p51-scheduler-restart-w30 || "$gate" == p51-scheduler-f-restart-w30 ]]; then
    if [[ "$gate" == p51-scheduler-f-restart-w30 ]]; then
        export ICECC_P50_C1F1_REAL_SCHEDULER_F_RESTART_W30=1
    else
        unset ICECC_P50_C1F1_REAL_SCHEDULER_F_RESTART_W30
    fi
    ICECC_TEST_P51_PRIVATE_NETNS=1 ICECC_TEST_DAEMON_UID=icecc \
        ICECC_TEST_DAEMON_GID=icecc \
        timeout --signal=TERM --kill-after=20s "${timeout_s}s" \
            make -C /work/build/unittests "$target" >"$log" 2>&1
else
    timeout --signal=TERM --kill-after=15s "${timeout_s}s" \
        make -C /work/build/unittests "$target" >"$log" 2>&1
fi
status=$?
set -e
printf '%s\n' "$status" >"$status_file"
cat "$log"
python3 /source/dev/gate-result.py "$status" "$log" "$marker" "$expected_markers" || {
    echo "FAIL: gate result rejected; retained log=$log" >&2
    exit 1
}
echo "GATE_PASS name=$gate log=$log"
