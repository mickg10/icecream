#!/usr/bin/env bash
set -uo pipefail

usage() {
    echo "usage: run-gate.sh {p51-wrapper-compile|p51-arm-expiry|p51-restart-w30|p51-scheduler-restart-w30|p51-scheduler-f-restart-w30|p51-restart-chain-w30|p51-capacity-w30|p51-compiler-loss-w30|p50-live-core|p50-c02-channel|p50-d15-r2-wire}" >&2
}

capture_gate_status() {
    if "$@"; then
        status=0
    else
        status=$?
    fi
}

require_wrapper_compile_cells() {
    local log=$1 jobs profile exact count
    for jobs in 2 100; do
        for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
            exact="P51_WRAPPER_COMPILE_PASS profile=$profile jobs=$jobs "
            count=$(grep -F -c "$exact" "$log" || true)
            if [[ $count -ne 1 ]]; then
                echo "FAIL: expected exactly one wrapper C01 marker for profile=$profile jobs=$jobs, found $count; retained log=$log" >&2
                return 1
            fi
        done
    done
}

require_compiler_loss_w30_profiles() {
    local log=$1 profile exact count
    for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
        exact="P51_WRAPPER_COMPILER_LOSS_W30_PASS profile=$profile "
        count=$(grep -F -c "$exact" "$log" || true)
        if [[ $count -ne 1 ]]; then
            echo "FAIL: expected exactly one compiler-loss W30 marker for profile=$profile, found $count; retained log=$log" >&2
            return 1
        fi
    done
}

require_d15_r2_semantic_cells() {
    local log=$1 profile field marker count log_contents
    log_contents=$(cat -- "$log") || return 1
    for marker in \
        'P51_D15_R2_WIRE malformed recovery records all directions/profiles: PASS' \
        'P51_D15_R2_WIRE malformed outer frames all-applicable-profiles: PASS'; do
        count=$(grep -F -x -c "$marker" <<<"$log_contents" || true)
        if [[ $count -ne 1 ]]; then
            echo "FAIL: expected exactly one D15 malformed-matrix marker, found $count; retained log=$log" >&2
            return 1
        fi
    done
    for profile in 1 2 3; do
        for field in ordinal binding-digest envelope-txn-digest history-nonce \
                     rel-seq tu-seq inner-txn-digest raw-digest post-state-digest; do
            marker="P51_D15_R2_TX_COMMIT_SEMANTIC profile=$profile field=$field "
            count=$(grep -F -c "$marker" <<<"$log_contents" || true)
            if [[ $count -ne 1 ]]; then
                echo "FAIL: expected exactly one D15 semantic cell profile=$profile field=$field, found $count; retained log=$log" >&2
                return 1
            fi
        done
    done
}

if [[ $# -ne 1 ]]; then
    usage
    exit 2
fi

case "$1" in
    p51-wrapper-compile)
        gate=$1
        target=p51wrappercompile-check
        timeout_s=1800
        marker='P51_WRAPPER_COMPILE_PASS profile='
        expected_markers=6
        ;;
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
    p51-capacity-w30)
        gate=$1
        target=p51capacity-w30-run.sh
        timeout_s=600
        marker='P51_CAPACITY_W30_PASS topology=C1F4 profile='
        expected_markers=3
        capacity_profile=${ICECC_TEST_P51_CAPACITY_W30_PROFILE:-}
        case "$capacity_profile" in
            '') ;;
            P29V1|ZSTD_TU|ZSTD_ROUTE) expected_markers=1 ;;
            *) echo "FAIL: unsupported capacity W30 profile filter: $capacity_profile" >&2; exit 2 ;;
        esac
        ;;
    p51-compiler-loss-w30)
        gate=$1
        target=p51wrappercompile-compiler-loss-w30-check
        timeout_s=960
        marker='P51_WRAPPER_COMPILER_LOSS_W30_PASS profile='
        expected_markers=3
        ;;
    p50-live-core)
        gate=$1
        target="six required root/live P50 gates"
        timeout_s=1200
        marker=P50_LIVE_CORE_PASS=
        expected_markers=1
        ;;
    p50-c02-channel)
        gate=$1
        target="C02 cold/warm R2 same-channel compile witness"
        timeout_s=720
        c02_profile=${ICECC_TEST_P50_C02_PROFILE:-ZSTD_ROUTE}
        case "$c02_profile" in
            P29V1|ZSTD_TU|ZSTD_ROUTE) ;;
            *) echo "FAIL: unsupported C02 profile: $c02_profile" >&2; exit 2 ;;
        esac
        marker="C02_CHANNEL_WITNESS profile=$c02_profile "
        expected_markers=1
        ;;
    p50-d15-r2-wire)
        gate=$1
        target=p50endpoint-d15-wire-check
        timeout_s=420
        marker='P51_D15_R2_TX_COMMIT_SEMANTIC profile='
        expected_markers=27
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
status=0
if [[ "$gate" == p50-live-core ]]; then
    worker_scheduler_host=$(hostname -I | awk '{print $1}')
    if ! python3 - "$worker_scheduler_host" <<'PY'
import ipaddress
import sys

try:
    address = ipaddress.IPv4Address(sys.argv[1])
except ipaddress.AddressValueError:
    raise SystemExit(1)
if (address.is_loopback or address.is_unspecified or address.is_multicast or
        address.is_reserved or address.is_link_local):
    raise SystemExit(1)
PY
    then
        echo "FAIL: could not derive an ordinary bridge IPv4 for the live P50 worker scheduler: $worker_scheduler_host" >>"$log"
        status=2
    else
        export ICECC_P50_C1F1_WORKER_SCHEDULER_HOST="$worker_scheduler_host"
        export ICECC_P50_C1F1_KEEP_WORK=1
        export ICECC_TEST_DAEMON_UID=icecc ICECC_TEST_DAEMON_GID=icecc
        build_status=0
        timeout --signal=TERM --kill-after=15s 300s \
            make -C /work/build/cache icecc-cache-service-test \
            >>"$log" 2>&1 || build_status=$?
        if [[ $build_status -eq 0 ]]; then
            completion_build_status=0
            timeout --signal=TERM --kill-after=15s 180s \
                make -C /work/build/client icecc-p50-completion-test \
                >>"$log" 2>&1 || completion_build_status=$?
            if [[ $completion_build_status -ne 0 ]]; then
                echo "FAIL: could not build required completion-flow client helper (exit=$completion_build_status)" >>"$log"
                build_status=$completion_build_status
            fi
        fi
        if [[ $build_status -eq 0 ]]; then
            timeout --signal=TERM --kill-after=15s 300s \
                make -C /work/build/unittests p50daemonpositive p50sourcearm-live \
                >>"$log" 2>&1 || build_status=$?
        fi
        live_tests=(
            remoteice-quick
            p50assignment-remote
            p50completionflow-run
            p50compilee2e-run
            p50daemonpositive-run
            p50sourcearm-live-run
        )
        if [[ $build_status -ne 0 ]]; then
            status=$build_status
        fi
        for test_name in "${live_tests[@]}"; do
            [[ $status -eq 0 ]] || break
            script="/work/source/unittests/$test_name.sh"
            base=${test_name%.sh}
            test_log="/work/build/unittests/$base.log"
            test_trs="/work/build/unittests/$base.trs"
            if [[ -e "$test_log" || -e "$test_trs" ]]; then
                echo "FAIL: refusing stale live-test result for $test_name" >>"$log"
                status=1
                break
            fi
            echo "LIVE_TEST_START name=$test_name script=$script" >>"$log"
            test_status=0
            case "$test_name" in
                remoteice-quick|p50assignment-remote)
                    ICECC_TEST_REQUIRE_REMOTE=1 \
                        timeout --signal=TERM --kill-after=15s 240s \
                        make -C /work/build/unittests -W "$script" "$base.log" \
                        >>"$log" 2>&1 || test_status=$?
                    ;;
                p50completionflow-run|p50compilee2e-run)
                    timeout --signal=TERM --kill-after=15s 600s \
                        make -C /work/build/unittests -W "$script" "$base.log" \
                        >>"$log" 2>&1 || test_status=$?
                    ;;
                p50daemonpositive-run)
                    timeout --signal=TERM --kill-after=15s 600s \
                        env ICECC_TEST_POSITIVE_DAEMON=1 \
                        make -C /work/build/unittests -W "$script" "$base.log" \
                        >>"$log" 2>&1 || test_status=$?
                    ;;
                p50sourcearm-live-run)
                    timeout --signal=TERM --kill-after=15s 600s \
                        env ICECC_TEST_SOURCE_ARM_LIVE=1 \
                        make -C /work/build/unittests -W "$script" "$base.log" \
                        >>"$log" 2>&1 || test_status=$?
                    ;;
            esac
            if [[ $test_status -ne 0 ]] ||
               [[ ! -f "$test_trs" ]] ||
               ! grep -Fxq ':test-result: PASS' "$test_trs"; then
                echo "FAIL: $test_name did not produce a fresh PASS .trs (exit=$test_status)" >>"$log"
                status=${test_status:-1}
                [[ $status -eq 0 ]] && status=1
                break
            fi
            echo "LIVE_TEST_PASS name=$test_name trs=$test_trs" >>"$log"
        done
        if [[ $status -eq 0 ]]; then
            echo "P50_LIVE_CORE_PASS=1 worker_scheduler_host=$worker_scheduler_host tests=${#live_tests[@]}" >>"$log"
        fi
    fi
elif [[ "$gate" == p51-wrapper-compile ]]; then
    worker_scheduler_host=$(hostname -I | awk '{print $1}')
    if ! python3 - "$worker_scheduler_host" <<'PY'
import ipaddress
import sys

try:
    address = ipaddress.IPv4Address(sys.argv[1])
except (ipaddress.AddressValueError, IndexError):
    raise SystemExit(1)
if (address.is_loopback or address.is_unspecified or address.is_multicast or
        address.is_reserved or address.is_link_local):
    raise SystemExit(1)
PY
    then
        echo "FAIL: could not derive an ordinary bridge IPv4 for the wrapper worker scheduler: $worker_scheduler_host" >>"$log"
        status=2
    else
        export ICECC_P50_C1F1_WORKER_SCHEDULER_HOST="$worker_scheduler_host"
        export ICECC_TEST_DAEMON_UID=icecc ICECC_TEST_DAEMON_GID=icecc
        capture_gate_status timeout --signal=TERM --kill-after=15s 1800s \
            make -C /work/build/unittests "$target" >"$log" 2>&1
    fi
elif [[ "$gate" == p50-c02-channel ]]; then
    worker_scheduler_host=$(hostname -I | awk '{print $1}')
    if ! python3 - "$worker_scheduler_host" <<'PY'
import ipaddress
import sys

try:
    address = ipaddress.IPv4Address(sys.argv[1])
except (ipaddress.AddressValueError, IndexError):
    raise SystemExit(1)
if (address.is_loopback or address.is_unspecified or address.is_multicast or
        address.is_reserved or address.is_link_local):
    raise SystemExit(1)
PY
    then
        echo "FAIL: could not derive an ordinary bridge IPv4 for C02 worker scheduler: $worker_scheduler_host" >>"$log"
        status=2
    else
        export ICECC_P50_C1F1_WORKER_SCHEDULER_HOST="$worker_scheduler_host"
        export ICECC_TEST_DAEMON_UID=icecc ICECC_TEST_DAEMON_GID=icecc
        export ICECC_P50_C02_R2_CHANNEL=1 ICECC_P50_PROFILE="$c02_profile"
        export ICECC_P50_C1F1_KEEP_WORK=1
        build_status=0
        timeout --signal=TERM --kill-after=15s 300s \
            make -C /work/build/cache icecc-cache-service-test >>"$log" 2>&1 || build_status=$?
        if [[ $build_status -eq 0 ]]; then
            timeout --signal=TERM --kill-after=15s 300s \
                make -C /work/build/unittests p50daemonpositive >>"$log" 2>&1 || build_status=$?
        fi
        if [[ $build_status -ne 0 ]]; then
            status=$build_status
            echo "FAIL: could not build required P51 helper prerequisites (exit=$build_status)" >>"$log"
        else
            source_status=0
            timeout --signal=TERM --kill-after=15s 120s \
                make -C /work/build/unittests \
                -W /work/source/unittests/p50compilee2e-source.sh \
                p50compilee2e-source.log >>"$log" 2>&1 || source_status=$?
            source_trs=/work/build/unittests/p50compilee2e-source.trs
            if [[ $source_status -ne 0 || ! -f "$source_trs" ]] || \
                    ! grep -Fxq ':test-result: PASS' "$source_trs"; then
                status=$source_status
                [[ $status -eq 0 ]] && status=1
                echo "FAIL: C02 production source preflight did not produce a fresh PASS .trs (exit=$source_status)" >>"$log"
            fi
        fi
        if [[ $status -eq 0 ]]; then
            test_status=0
            timeout --signal=TERM --kill-after=15s "${timeout_s}s" \
                make -C /work/build/unittests -W /work/source/unittests/p50compilee2e-run.sh \
                p50compilee2e-run.log >>"$log" 2>&1 || test_status=$?
            test_trs=/work/build/unittests/p50compilee2e-run.trs
            test_log=/work/build/unittests/p50compilee2e-run.log
            if [[ -f "$test_log" ]]; then
                cat "$test_log" >>"$log"
            fi
            if [[ $test_status -ne 0 || ! -f "$test_trs" ]] || \
                    ! grep -Fxq ':test-result: PASS' "$test_trs"; then
                status=$test_status
                [[ $status -eq 0 ]] && status=1
                echo "FAIL: C02 compile gate did not produce a fresh PASS .trs (exit=$test_status)" >>"$log"
            fi
        fi
    fi
elif [[ "$gate" == p51-capacity-w30 ]]; then
    profiles=(P29V1 ZSTD_TU ZSTD_ROUTE)
    if [[ -n "$capacity_profile" ]]; then
        profiles=("$capacity_profile")
    fi
    status=0
    # Automake check_PROGRAMS are intentionally not part of `make all` or
    # `make install`; build the helper and its dedicated hook-enabled service
    # explicitly. The latter uses distinct objects and is never installed.
    if timeout --signal=TERM --kill-after=5s 120s \
         make -C /work/build/cache icecc-cache-service-test >>"$log" 2>&1 && \
         make -C /work/build/unittests p50daemonpositive >>"$log" 2>&1; then
        :
    else
        status=$?
    fi
    if [[ $status -eq 0 ]]; then
        for profile in "${profiles[@]}"; do
            echo "CAPACITY_W30_PROFILE_START profile=$profile" >>"$log"
            if ICECC_TEST_BUILDDIR=/work/build/unittests \
               ICECC_TEST_TOP_BUILDDIR=/work/build \
               ICECC_TEST_P51_CAPACITY_W30_PROFILE="$profile" \
               timeout --signal=TERM --kill-after=5s 90s \
                   /bin/bash /source/unittests/p51capacity-w30-run.sh >>"$log" 2>&1; then
                :
            else
                status=$?
                break
            fi
        done
    fi
elif [[ "$gate" == p51-compiler-loss-w30 ]]; then
    worker_scheduler_host=$(hostname -I | awk '{print $1}')
    if ! python3 - "$worker_scheduler_host" <<'PY'
import ipaddress
import sys

try:
    address = ipaddress.IPv4Address(sys.argv[1])
except (ipaddress.AddressValueError, IndexError):
    raise SystemExit(1)
if (address.is_loopback or address.is_unspecified or address.is_multicast or
        address.is_reserved or address.is_link_local):
    raise SystemExit(1)
PY
    then
        echo "FAIL: could not derive an ordinary bridge IPv4 for compiler-loss worker scheduler: $worker_scheduler_host" >>"$log"
        status=2
    elif ! command -v runuser >/dev/null 2>&1; then
        echo "FAIL: compiler-loss W30 gate requires runuser for the separate wrapper client identity" >>"$log"
        status=2
    else
        if ! getent passwd icecc-client >/dev/null 2>&1; then
            useradd --system --no-create-home --home-dir /nonexistent \
                --shell /usr/sbin/nologin --gid icecc icecc-client || status=$?
        fi
        client_uid=$(id -u icecc-client 2>/dev/null || true)
        daemon_uid=$(id -u icecc 2>/dev/null || true)
        if [[ -z "$client_uid" || -z "$daemon_uid" || "$client_uid" == 0 ||
              "$client_uid" == "$daemon_uid" ]]; then
            echo "FAIL: compiler-loss gate requires distinct non-root icecc-client and icecc UIDs" >>"$log"
            status=2
        fi
        if [[ $status -eq 0 ]]; then
            export ICECC_P50_C1F1_WORKER_SCHEDULER_HOST="$worker_scheduler_host"
            export ICECC_TEST_DAEMON_UID=icecc ICECC_TEST_DAEMON_GID=icecc
            # This check-only Automake helper has no cross-directory rule in
            # the unittests Makefile. Build it from its owning directory first.
            build_status=0
            timeout --signal=TERM --kill-after=15s 300s \
                make -C /work/build/cache icecc-cache-service-test \
                >>"$log" 2>&1 || build_status=$?
            if [[ $build_status -eq 0 ]]; then
                capture_gate_status timeout --signal=TERM --kill-after=20s "${timeout_s}s" \
                    make -C /work/build/unittests "$target" >>"$log" 2>&1
            else
                status=$build_status
                echo "FAIL: could not build required hook-enabled cache service (exit=$build_status)" >>"$log"
            fi
        fi
    fi
elif [[ "$gate" == p51-scheduler-restart-w30 || "$gate" == p51-scheduler-f-restart-w30 ]]; then
    if [[ "$gate" == p51-scheduler-f-restart-w30 ]]; then
        export ICECC_P50_C1F1_REAL_SCHEDULER_F_RESTART_W30=1
    else
        unset ICECC_P50_C1F1_REAL_SCHEDULER_F_RESTART_W30
    fi
    capture_gate_status env ICECC_TEST_P51_PRIVATE_NETNS=1 ICECC_TEST_DAEMON_UID=icecc \
        ICECC_TEST_DAEMON_GID=icecc \
        timeout --signal=TERM --kill-after=20s "${timeout_s}s" \
            make -C /work/build/unittests "$target" >"$log" 2>&1
else
    capture_gate_status timeout --signal=TERM --kill-after=15s "${timeout_s}s" \
        make -C /work/build/unittests "$target" >"$log" 2>&1
fi
set -e
printf '%s\n' "$status" >"$status_file"
cat "$log"
python3 /source/dev/gate-result.py "$status" "$log" "$marker" "$expected_markers" || {
    echo "FAIL: gate result rejected; retained log=$log" >&2
    exit 1
}
if [[ "$gate" == p51-wrapper-compile ]]; then
    require_wrapper_compile_cells "$log" || exit 1
fi
if [[ "$gate" == p51-compiler-loss-w30 ]]; then
    require_compiler_loss_w30_profiles "$log" || exit 1
fi
if [[ "$gate" == p50-d15-r2-wire ]]; then
    require_d15_r2_semantic_cells "$log" || exit 1
fi
echo "GATE_PASS name=$gate log=$log"
