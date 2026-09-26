#!/bin/sh
set -eu

top_src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
top_build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
unit_build=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
service=${ICECC_TEST_CACHE_SERVICE:?ICECC_TEST_CACHE_SERVICE is required}
cxx=${ICECC_TEST_CXX:-c++}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++20}
src="$top_src/cache/p50_daemon_sidecar_adapter.cpp"
header="$top_src/cache/p50_daemon_sidecar_adapter.h"
test_source="$top_src/unittests/p50_daemon_sidecar_adapter_test.cpp"

test -x "$service" || {
    echo 'FAIL: adapter source gate requires the built service' >&2
    exit 1
}
grep -F 'outer_current_ready_lease()' "$src" >/dev/null
grep -F 'outer_prepare_attempt_retirement' "$src" >/dev/null
grep -F 'launch_identities' "$src" >/dev/null
grep -F 'runtime_directory' "$src" >/dev/null
grep -F 'cumulative_post_ready_exits_' "$src" >/dev/null
grep -F 'observation.cumulative_post_ready_exits' "$src" >/dev/null
grep -F 'runtime_nodes_valid()' "$src" >/dev/null
grep -F 'socket_info.st_dev == socket_device_' "$src" >/dev/null
grep -F 'socket_info.st_ino == socket_inode_' "$src" >/dev/null
grep -F 'directory_info.st_ino != attempt_directory_inode_' "$src" >/dev/null
grep -F 'controller_.observe(observation)' "$src" >/dev/null
grep -F 'executable_file(config.executable)' "$src" >/dev/null
grep -F 'observe_public_listener' "$header" >/dev/null
grep -F 'outer_begin_turn' "$header" >/dev/null
grep -F 'outer_advance_turn' "$header" >/dev/null
grep -F 'outer_append_pollfds' "$header" >/dev/null
grep -F 'outer_immediate_turn_required' "$header" >/dev/null
grep -F 'outer_prepare_attempt_retirement' "$header" >/dev/null
grep -F 'outer_commit_attempt_replacement' "$header" >/dev/null
grep -F 'outer_close_logical_input_lease' "$header" >/dev/null
grep -F 'AttemptLeafRetirementJoin' "$header" >/dev/null

if grep -E 'daemon/main\.cpp|signal\(|sigaction\(|listen_unix\(' "$src" "$header" >/dev/null; then
    echo 'FAIL: adapter acquired daemon-main, signal-handler, or public-listener ownership' >&2
    exit 1
fi
if grep -F 'unlink(socket_path_.c_str())' "$src" >/dev/null \
        || grep -F 'supervisor_->' "$src" >/dev/null \
        || grep -E '(^|[^[:alnum:]_])waitpid[[:space:]]*\(' "$src" >/dev/null; then
    echo 'FAIL: adapter retains a synchronous supervisor/reap path' >&2
    exit 1
fi

tmp_root=$(mktemp -d "${TMPDIR:-/tmp}/p50daemonsidecaradapter-source.XXXXXX")
runtime_roots=
cleanup() {
    cleanup_status=0
    for runtime_root in $runtime_roots; do
        # The adapter deliberately leaves teardown to the outer-loop owner;
        # this test must retire a detached child even when the test process is
        # interrupted before compile_and_expect_red can do its normal cleanup.
        if command -v retire_mutant_sidecars >/dev/null 2>&1; then
            retire_mutant_sidecars "$runtime_root" || cleanup_status=1
        fi
        rm -rf -- "$runtime_root" || cleanup_status=1
    done
    rm -rf -- "$tmp_root" || cleanup_status=1
    return "$cleanup_status"
}
trap cleanup EXIT HUP INT TERM

forget_runtime_root() {
    forgotten_root=$1
    retained_roots=
    for retained_root in $runtime_roots; do
        test "$retained_root" = "$forgotten_root" || \
            retained_roots="$retained_roots $retained_root"
    done
    runtime_roots=$retained_roots
}

# The production object must contain no test-only counter/overflow hook.
production_object="$tmp_root/production.o"
"$cxx" "$standard" -Wall -Wextra -Werror -pthread -DHAVE_CONFIG_H \
    -I"$top_build" -I"$top_src" -I"$top_src/cache" \
    -I"$top_src/client" -I"$top_src/services" \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
    -UICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS -c "$src" -o "$production_object"
if nm -C "$production_object" | grep -E 'test_force_attempt|test_force_counter_state|test_force_input_lifecycle_operation' >/dev/null ||
   strings "$production_object" | grep -F 'ICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS' >/dev/null; then
    echo 'FAIL: production adapter contains a test-only hook' >&2
    exit 1
fi
echo 'ok - production adapter contains no test-only hook'

test_object="$tmp_root/test.o"
dispatch_object="$tmp_root/dispatch.o"
handoff_object="$tmp_root/handoff.o"
attachment_object="$tmp_root/input_fd_attachment.o"
lifecycle_object="$tmp_root/p50_input_lifecycle.o"
for source_and_object in \
    "$top_src/cache/p50_daemon_cache_dispatch.cpp:$dispatch_object" \
    "$top_src/cache/p50_fd_handoff.cpp:$handoff_object" \
    "$top_src/cache/p50_input_fd_attachment.cpp:$attachment_object" \
    "$top_src/cache/p50_input_lifecycle.cpp:$lifecycle_object"; do
    source=${source_and_object%%:*}
    object=${source_and_object#*:}
    "$cxx" "$standard" -Wall -Wextra -Werror -pthread -DHAVE_CONFIG_H \
        -I"$top_build" -I"$top_src" -I"$top_src/cache" \
        -I"$top_src/client" -I"$top_src/services" \
        ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
        -c "$source" -o "$object"
done
"$cxx" "$standard" -Wall -Wextra -Werror -pthread -DHAVE_CONFIG_H \
    -DICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS \
    -I"$top_build" -I"$top_src" -I"$top_src/cache" \
    -I"$top_src/client" -I"$top_src/services" \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
    -c "$test_source" -o "$test_object"

link_binary() {
    adapter_object=$1
    binary=$2
    "$cxx" "$standard" -pthread ${ICECC_TEST_LDFLAGS:-} \
        "$test_object" "$dispatch_object" "$handoff_object" \
        "$attachment_object" "$lifecycle_object" "$adapter_object" \
        "$top_build/cache/libp50sidecar.a" \
        "$top_build/cache/libp50localtransport.a" \
        "$top_build/cache/libprotocol50.a" \
        "$top_build/services/.libs/libicecc.a" \
        ${ICECC_TEST_LIBCAP_NG_LIBS:-} -llzo2 \
        ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} \
        ${ICECC_TEST_XXHASH_LIBS:--lxxhash} -o "$binary"
}

proc_starttime() {
    ms_pid=$1
    ms_stat=$(cat "/proc/$ms_pid/stat" 2>/dev/null) || return 1
    ms_rest=${ms_stat##*)}
    set -- $ms_rest
    test "$#" -ge 20 || return 1
    printf '%s\n' "${20}"
}

ready_trace_pid() {
    ready_trace=$1
    test -s "$ready_trace" || return 1
    ready_line_count=$(wc -l <"$ready_trace" | tr -d '[:space:]')
    test "$ready_line_count" = 1 || return 1
    ready_line=$(cat "$ready_trace") || return 1
    printf '%s\n' "$ready_line" | grep -Eq \
        '^READY v2 generation=[1-9][0-9]* attempt=[1-9][0-9]* F_STORE_GENERATION=[1-9][0-9]* DERIVATION_VERSION=1 pid=[1-9][0-9]* C_STORE_GUID=[0-9A-Fa-f]{32} F_STORE_GUID=[0-9A-Fa-f]{32} PATH=[^[:space:]]+ DIGEST=[0-9A-Fa-f]{32} DEV=[1-9][0-9]* INO=[1-9][0-9]*$' || return 1
    ready_pid=$(printf '%s\n' "$ready_line" |
        sed -n 's/.* pid=\([0-9][0-9]*\) .*/\1/p')
    test -n "$ready_pid" || return 1
    printf '%s\n' "$ready_pid"
}

# Keep parser controls local and deterministic.  They exercise the exact
# full READY v2 record emitted by the service without launching a second
# process or contacting a remote host.
ready_trace_valid="$tmp_root/ready-trace-valid"
cat >"$ready_trace_valid" <<'EOF'
READY v2 generation=1 attempt=1 F_STORE_GENERATION=1 DERIVATION_VERSION=1 pid=42 C_STORE_GUID=00112233445566778899aabbccddeeff F_STORE_GUID=ffeeddccbbaa99887766554433221100 PATH=/tmp/p50-ready.sock DIGEST=00112233445566778899aabbccddeeff DEV=1 INO=2
EOF
test "$(ready_trace_pid "$ready_trace_valid")" = 42
ready_trace_red() {
    ready_mutant_label=$1
    ready_mutant_path=$2
    if ready_trace_pid "$ready_mutant_path" >/dev/null 2>&1; then
        echo "FAIL: READY parser accepted $ready_mutant_label" >&2
        exit 1
    fi
    echo "ok - READY parser rejects $ready_mutant_label"
}
ready_trace_empty="$tmp_root/ready-trace-empty"
: >"$ready_trace_empty"
ready_trace_red zero-record "$ready_trace_empty"
ready_trace_duplicate="$tmp_root/ready-trace-duplicate"
cat "$ready_trace_valid" "$ready_trace_valid" >"$ready_trace_duplicate"
ready_trace_red duplicate-record "$ready_trace_duplicate"
ready_trace_no_pid="$tmp_root/ready-trace-no-pid"
sed 's/ pid=42 / worker=42 /' "$ready_trace_valid" >"$ready_trace_no_pid"
ready_trace_red missing-pid "$ready_trace_no_pid"
ready_trace_malformed="$tmp_root/ready-trace-malformed"
sed 's/^READY v2 /READY v1 /' "$ready_trace_valid" >"$ready_trace_malformed"
ready_trace_red malformed-record "$ready_trace_malformed"
ready_trace_generation_bogus="$tmp_root/ready-trace-generation-bogus"
sed 's/generation=1 /generation=bogus /' "$ready_trace_valid" >"$ready_trace_generation_bogus"
ready_trace_red nonnumeric-generation "$ready_trace_generation_bogus"
ready_trace_pid_zero="$tmp_root/ready-trace-pid-zero"
sed 's/ pid=42 / pid=0 /' "$ready_trace_valid" >"$ready_trace_pid_zero"
ready_trace_red zero-pid "$ready_trace_pid_zero"
ready_trace_dev_zero="$tmp_root/ready-trace-dev-zero"
sed 's/ DEV=1 / DEV=0 /' "$ready_trace_valid" >"$ready_trace_dev_zero"
ready_trace_red zero-device "$ready_trace_dev_zero"
ready_trace_ino_zero="$tmp_root/ready-trace-ino-zero"
sed 's/ INO=2$/ INO=0/' "$ready_trace_valid" >"$ready_trace_ino_zero"
ready_trace_red zero-inode "$ready_trace_ino_zero"

mutant_sidecar_records() {
    runtime_root=$1
    if test -n "${interruption_ready_trace:-}" && \
            test -s "$interruption_ready_trace"; then
        process_pid=$(ready_trace_pid "$interruption_ready_trace") || return 1
        if mutant_sidecar_record_for_pid "$runtime_root" "$process_pid"; then
            return 0
        else
            record_status=$?
        fi
        # A valid READY owner that has already exited is an empty result;
        # an existing owner with wrong executable/socket/starttime/stat
        # identity is a hard error and must not be treated as empty.
        test -d "/proc/$process_pid" || return 0
        return "$record_status"
    fi
    if test "${ICECC_P50_TEST_FORBID_PROC_FALLBACK:-0}" = 1; then
        echo 'FAIL: sidecar ownership discovery used forbidden /proc fallback' >&2
        return 1
    fi
    for process_root in /proc/[0-9]*; do
        process_pid=${process_root##*/}
        mutant_sidecar_record_for_pid "$runtime_root" "$process_pid" || continue
    done
}

records_to_pids() {
    records_input=$1
    pids_output=$2
    : >"$pids_output" || return 1
    while IFS= read -r sidecar_record; do
        test -n "$sidecar_record" || continue
        record_pid=${sidecar_record%%|*}
        record_rest=${sidecar_record#*|}
        test "$record_rest" != "$sidecar_record" || return 1
        record_start=${record_rest%%|*}
        record_rest=${record_rest#*|}
        test -n "$record_start" || return 1
        test "$record_rest" != "$record_start" || return 1
        record_exe=${record_rest%%|*}
        record_rest=${record_rest#*|}
        test -n "$record_exe" || return 1
        test "$record_rest" != "$record_exe" || return 1
        record_listener_identity=${record_rest%%|*}
        record_rest=${record_rest#*|}
        record_path_identity=${record_rest%%|*}
        record_socket_path=${record_rest#*|}
        test -n "$record_listener_identity" || return 1
        test "$record_rest" != "$record_listener_identity" || return 1
        test -n "$record_path_identity" || return 1
        test "$record_socket_path" != "$record_rest" || return 1
        test -n "$record_socket_path" || return 1
        case "$record_pid" in ''|0|*[!0-9]*) return 1 ;; esac
        case "$record_start" in ''|*[!0-9]*) return 1 ;; esac
        case "$record_listener_identity" in *:*) ;; *) return 1 ;; esac
        case "$record_path_identity" in ENOENT|*:*) ;; *) return 1 ;; esac
        printf '%s\n' "$record_pid" >>"$pids_output" || return 1
    done <"$records_input"
}

malformed_sidecar_records="$tmp_root/malformed-sidecar-records"
malformed_sidecar_pids="$tmp_root/malformed-sidecar-pids"
printf '%s\n' '12|34|/service|not-an-identity|ENOENT|/tmp/socket' \
    >"$malformed_sidecar_records"
if records_to_pids "$malformed_sidecar_records" "$malformed_sidecar_pids"; then
    echo 'FAIL: malformed listener identity reached PID extraction' >&2
    exit 1
fi
printf '%s\n' '12|34|/service|8:9|ENOENT' >"$malformed_sidecar_records"
if records_to_pids "$malformed_sidecar_records" "$malformed_sidecar_pids"; then
    echo 'FAIL: truncated sidecar record reached PID extraction' >&2
    exit 1
fi
echo 'ok - malformed sidecar records cannot reach PID extraction'

listener_socket_identity_for_pid() {
    listener_pid=$1
    listener_socket=$2
    test -n "$listener_socket" || return 1
    listener_fd_root=/proc/$listener_pid/fd
    listener_count=0
    listener_identity=
    for listener_fd_path in "$listener_fd_root"/[0-9]*; do
        test -e "$listener_fd_path" || continue
        listener_fd_target=$(readlink "$listener_fd_path" 2>/dev/null) || continue
        case "$listener_fd_target" in
            socket:\[[0-9]*\]) ;;
            *) continue ;;
        esac
        listener_socket_inode=${listener_fd_target#socket:[}
        listener_socket_inode=${listener_socket_inode%]}
        case "$listener_socket_inode" in
            ''|*[!0-9]*) continue ;;
        esac
        # The fd itself is the retained identity.  Bind its kernel socket
        # inode to the exact --socket pathname through /proc/net/unix; the
        # filesystem socket-node inode is a different inode namespace and
        # cannot be compared directly with the fd identity.
        if ! awk -v inode="$listener_socket_inode" -v path="$listener_socket" \
                'NR > 1 && $4 == "00010000" && $5 == "0001" &&
                 $6 == "01" && $7 == inode && $8 == path { found = 1 }
                 END { exit(found ? 0 : 1) }' \
                "/proc/$listener_pid/net/unix" 2>/dev/null; then
            continue
        fi
        listener_fd_identity=$(stat -Lc '%d:%i' "$listener_fd_path" 2>/dev/null) || return 1
        case "$listener_fd_identity" in *:*) ;; *) return 1 ;; esac
        listener_count=$((listener_count + 1))
        listener_identity=$listener_fd_identity
    done
    test "$listener_count" = 1 || return 1
    printf '%s\n' "$listener_identity"
}

mutant_sidecar_record_for_pid() {
    runtime_root=$1
    process_pid=$2
    process_root=/proc/$process_pid
    test -d "$process_root" || return 1
    process_exe=$(readlink "$process_root/exe" 2>/dev/null) || return 1
    test "$process_exe" = "$service" || return 1
    process_command=$(tr '\000' ' ' <"$process_root/cmdline" 2>/dev/null) || return 1
    case "$process_command" in *" --socket "*) ;; *) return 1 ;; esac
    process_socket=$(printf '%s\n' "$process_command" |
        sed -n 's/.* --socket \([^ ]*\).*/\1/p')
    case "$process_socket" in "$runtime_root"/*) ;; *) return 1 ;; esac
    process_starttime=$(proc_starttime "$process_pid") || return 1
    process_listener_identity=$(listener_socket_identity_for_pid \
        "$process_pid" "$process_socket") || return 1
    if process_socket_info=$(stat -c '%F|%d:%i' "$process_socket" 2>/dev/null); then
        process_socket_type=${process_socket_info%%|*}
        process_socket_identity=${process_socket_info#*|}
        test "$process_socket_type" = 'socket' || return 1
        case "$process_socket_identity" in *:*) ;; *) return 1 ;; esac
    else
        # A rejected mutant can unlink the pathname while retaining the
        # listener fd.  ENOENT is safe only with that independently captured
        # listener identity; other existing/non-socket nodes remain RED.
        test ! -e "$process_socket" || return 1
        process_socket_identity=ENOENT
    fi
    printf '%s|%s|%s|%s|%s|%s\n' "$process_pid" "$process_starttime" \
        "$process_exe" "$process_listener_identity" "$process_socket_identity" \
        "$process_socket"
}

# A READY PID that is still alive must be rejected when it is not the cache
# service, or when its command/socket ownership cannot be authenticated.
ready_trace_live_pid="$tmp_root/ready-trace-live-pid"
sed "s/ pid=42 / pid=$$ /" "$ready_trace_valid" >"$ready_trace_live_pid"
interruption_ready_trace="$ready_trace_live_pid"
if mutant_sidecar_records "$tmp_root/no-runtime" \
        >"$tmp_root/wrong-service.output" 2>"$tmp_root/wrong-service.error"; then
    echo 'FAIL: READY PID for the wrong service was accepted' >&2
    exit 1
fi
echo 'ok - READY PID for the wrong service is rejected'
service_before_ownership_test=$service
service=$(readlink "/proc/$$/exe")
if mutant_sidecar_records "$tmp_root/no-runtime" \
        >"$tmp_root/ownership.output" 2>"$tmp_root/ownership.error"; then
    echo 'FAIL: READY PID without authenticated socket ownership was accepted' >&2
    exit 1
fi
service=$service_before_ownership_test
echo 'ok - READY PID without authenticated socket ownership is rejected'
unset interruption_ready_trace

sidecar_process_matches() {
    ms_record=$1
    ms_pid=${ms_record%%|*}; ms_rest=${ms_record#*|}
    ms_start=${ms_rest%%|*}; ms_rest=${ms_rest#*|}
    ms_exe=${ms_rest%%|*}; ms_rest=${ms_rest#*|}
    ms_listener_identity=${ms_rest%%|*}; ms_rest=${ms_rest#*|}
    ms_socket_identity=${ms_rest%%|*}; ms_socket=${ms_rest#*|}
    test -d "/proc/$ms_pid" || return 1
    test "$(readlink "/proc/$ms_pid/exe" 2>/dev/null || :)" = "$ms_exe" || return 1
    test "$(proc_starttime "$ms_pid" 2>/dev/null || :)" = "$ms_start" || return 1
    ms_command=$(tr '\000' ' ' <"/proc/$ms_pid/cmdline" 2>/dev/null) || return 1
    case "$ms_command" in
        *" --socket $ms_socket "*) ;;
        *)
            case "$ms_command" in
                *" --socket $ms_socket") ;; *) return 1 ;;
            esac
            ;;
    esac
}

sidecar_record_matches() {
    ms_record=$1
    ms_pid=${ms_record%%|*}; ms_rest=${ms_record#*|}
    ms_start=${ms_rest%%|*}; ms_rest=${ms_rest#*|}
    ms_exe=${ms_rest%%|*}; ms_rest=${ms_rest#*|}
    ms_listener_identity=${ms_rest%%|*}; ms_rest=${ms_rest#*|}
    ms_socket_identity=${ms_rest%%|*}; ms_socket=${ms_rest#*|}
    sidecar_process_matches "$ms_record" || return 1
    test "$(listener_socket_identity_for_pid \
        "$ms_pid" "$ms_socket" 2>/dev/null || :)" = \
        "$ms_listener_identity" || return 1
    if test "$ms_socket_identity" = ENOENT; then
        # A record made after pathname removal remains valid only while the
        # retained listener identity above is still exact.
        test ! -e "$ms_socket"
        return $?
    fi
    if ms_socket_info=$(stat -c '%F|%d:%i' "$ms_socket" 2>/dev/null); then
        ms_socket_type=${ms_socket_info%%|*}
        ms_current_socket_identity=${ms_socket_info#*|}
        test "$ms_socket_type" = 'socket' || return 1
        test "$ms_current_socket_identity" = "$ms_socket_identity"
        return $?
    fi
    # ENOENT is the sole pathname failure admitted by the retained listener
    # identity check above.  A permission/error or replacement node is RED.
    test ! -e "$ms_socket"
}

retire_mutant_sidecars() {
    runtime_root=$1
    sidecar_record_file=$(mktemp "$tmp_root/sidecar-records.XXXXXX") || return 1
    sidecar_records=
    # Give a detached rejected mutant a short, bounded publication window;
    # otherwise a late attempt-2 launch could appear after an empty scan.
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if mutant_sidecar_records "$runtime_root" >"$sidecar_record_file"; then
            if sidecar_records=$(cat "$sidecar_record_file"); then
                :
            else
                rm -f -- "$sidecar_record_file"
                return 1
            fi
        else
            rm -f -- "$sidecar_record_file"
            return 1
        fi
        test -n "$sidecar_records" && break
        test -n "${interruption_ready_trace:-}" && break
        sleep 0.05
    done
    rm -f -- "$sidecar_record_file" || return 1
    for sidecar_record in $sidecar_records; do
        sidecar_record_matches "$sidecar_record" || continue
        sidecar_pid=${sidecar_record%%|*}
        kill -TERM "$sidecar_pid" 2>/dev/null || :
    done
    remaining_records=$sidecar_records
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        still_matching=
        for sidecar_record in $remaining_records; do
            # Keep exact process identity in the survivor set even if the
            # listener vanished during teardown; that condition must fail the
            # gate rather than silently treating a live service as gone.
            sidecar_process_matches "$sidecar_record" || continue
            still_matching="$still_matching $sidecar_record"
        done
        remaining_records=$still_matching
        test -z "$remaining_records" && return 0
        sleep 0.05
    done
    for sidecar_record in $remaining_records; do
        sidecar_process_matches "$sidecar_record" || continue
        sidecar_pid=${sidecar_record%%|*}
        kill -KILL "$sidecar_pid" 2>/dev/null || :
    done
    remaining_records=
    for sidecar_record in $sidecar_records; do
        sidecar_process_matches "$sidecar_record" || continue
        remaining_records="$remaining_records $sidecar_record"
    done
    test -z "$remaining_records"
}

# If an exact process remains after both signals, losing its listener must not
# make the final census report success.  This pure-shell control exercises the
# retirement decision without launching or signalling a real process.
if (
    simulated_listener_lost=0
    mutant_sidecar_records() {
        printf '%s\n' '123|456|/service|8:9|ENOENT|/tmp/simulated.sock'
    }
    sidecar_record_matches() { test "$simulated_listener_lost" = 0; }
    sidecar_process_matches() { return 0; }
    kill() { simulated_listener_lost=1; return 0; }
    sleep() { :; }
    retire_mutant_sidecars "$tmp_root/simulated-survivor"
); then
    echo 'FAIL: exact process survivor was hidden by listener loss' >&2
    exit 1
fi
echo 'ok - final cleanup census retains an exact process after listener loss'

# A malformed READY frame must remain a hard parser error through the cleanup
# owner too; an empty `cut` result is not an acceptable success signal.
interruption_ready_trace="$ready_trace_malformed"
if mutant_sidecar_records "$tmp_root/no-runtime" \
        >"$tmp_root/poll-parser.output" 2>"$tmp_root/poll-parser.error"; then
    echo 'FAIL: malformed READY trace was accepted by polling' >&2
    exit 1
fi
echo 'ok - malformed READY trace status propagates through polling'
if ICECC_P50_TEST_FORBID_PROC_FALLBACK=1 \
        retire_mutant_sidecars "$tmp_root/no-runtime" \
        >"$tmp_root/cleanup-parser.output" 2>"$tmp_root/cleanup-parser.error"; then
    echo 'FAIL: malformed READY trace was accepted by cleanup' >&2
    exit 1
fi
echo 'ok - malformed READY trace status propagates through cleanup'
if mutant_sidecar_records "$tmp_root/no-runtime" \
        >"$tmp_root/final-parser.output" 2>"$tmp_root/final-parser.error" && \
        records_to_pids "$tmp_root/final-parser.output" \
            "$tmp_root/final-parser.pids"; then
    echo 'FAIL: malformed READY trace reached final PID extraction' >&2
    exit 1
fi
echo 'ok - malformed READY trace cannot reach final PID extraction'
unset interruption_ready_trace

# Keep the runtime prefix short enough for the adapter's sockaddr_un path
# contract; the unique directory name still scopes exact child cleanup.
baseline_runtime_root=$(mktemp -d /tmp/p5b.XXXXXX)
runtime_roots="$runtime_roots $baseline_runtime_root"
baseline="$tmp_root/baseline"
link_binary "$production_object" "$baseline"
TMPDIR="$baseline_runtime_root" ICECC_TEST_CACHE_SERVICE="$service" timeout 60s "$baseline"
echo 'ok - current-source linked service/SCM_RIGHTS lifecycle baseline passes'

# Interrupt a live baseline and prove that its exact socket-owned sidecar is
# retired.  The wrapper owns only this child and runtime root; it never sends
# a broad process or process-group signal.
interruption_runtime_root=$(mktemp -d /tmp/p5i.XXXXXX)
runtime_roots="$runtime_roots $interruption_runtime_root"
interruption_log="$tmp_root/interruption.log"
interruption_ready_trace="$tmp_root/interruption.ready"
interruption_ready_hold="$tmp_root/interruption.hold"
interruption_abort_trace="$tmp_root/interruption.abort"
# The parent shell performs every ownership poll and final retirement call;
# keep the fallback prohibition in this scope, not only in the child env.
export ICECC_P50_TEST_FORBID_PROC_FALLBACK=1
interruption_ready_trace_missing="$tmp_root/interruption.ready.missing"
: >"$interruption_ready_trace_missing"
interruption_ready_trace="$interruption_ready_trace_missing"
ready_fallback_error="$tmp_root/ready-fallback.error"
if mutant_sidecar_records "$interruption_runtime_root" \
        >"$tmp_root/ready-fallback.output" 2>"$ready_fallback_error"; then
    echo 'FAIL: deleted READY trace reached sidecar ownership discovery' >&2
    exit 1
fi
grep -qF 'forbidden /proc fallback' "$ready_fallback_error" || {
    echo 'FAIL: deleted READY trace did not fail specifically at the fallback guard' >&2
    cat "$ready_fallback_error" >&2
    exit 1
}
echo 'ok - deleted READY trace is RED at the parent fallback guard'
interruption_ready_trace="$tmp_root/interruption.ready"

# Parent-side failures must terminate and reap the exact wrapper owner.  Keep
# this helper argument-based so it cannot accidentally signal a sibling or a
# process group.
stop_interruption_wrapper() {
    wrapper_pid=$1
    kill -CONT "$wrapper_pid" 2>/dev/null || :
    kill -TERM "$wrapper_pid" 2>/dev/null || :
    wait "$wrapper_pid" 2>/dev/null || :
}
# Bash and some /bin/sh implementations copy the parent's EXIT trap into an
# asynchronous command's child environment. Do not let that exact test child
# run the wrapper cleanup before stop_interruption_wrapper reaps it.
trap - EXIT HUP INT TERM
sleep 60 &
interruption_orphan_test_pid=$!
trap cleanup EXIT HUP INT TERM
stop_interruption_wrapper "$interruption_orphan_test_pid"
if kill -0 "$interruption_orphan_test_pid" 2>/dev/null; then
    echo 'FAIL: parent wrapper cleanup left an orphan' >&2
    exit 1
fi
echo 'ok - parent wrapper cleanup terminates and reaps the exact owner'

(
    # Bash may inherit the parent EXIT trap into this compound child. Its
    # own signal cleanup below must own only this interruption runtime root.
    trap - EXIT HUP INT TERM
    interruption_child_pid=
    stop_interruption_child() {
        if test -z "$interruption_child_pid"; then
            return 0
        fi
        # Do not let a broken test binary make the cleanup regression hang.
        # TERM is preferred; after a bounded grace period KILL is sent only
        # while the exact wrapper-owned child PID is still present.
        kill -CONT "$interruption_child_pid" 2>/dev/null || :
        kill -TERM "$interruption_child_pid" 2>/dev/null || :
        interruption_wait=0
        while kill -0 "$interruption_child_pid" 2>/dev/null && \
                test "$interruption_wait" -lt 40; do
            sleep 0.05
            interruption_wait=$((interruption_wait + 1))
        done
        if kill -0 "$interruption_child_pid" 2>/dev/null; then
            kill -KILL "$interruption_child_pid" 2>/dev/null || :
        fi
        wait "$interruption_child_pid" 2>/dev/null || :
    }
    interruption_cleanup() {
        stop_interruption_child
        retire_mutant_sidecars "$interruption_runtime_root" || exit 1
        rm -rf -- "$interruption_runtime_root" || exit 1
        exit 0
    }
    trap interruption_cleanup HUP INT TERM
    ICECC_P50_C1F1_REQUIRED=1 ICECC_P50_TEST_READY_TRACE="$interruption_ready_trace" \
        ICECC_P50_TEST_READY_HOLD="$interruption_ready_hold" \
        ICECC_P50_TEST_FORBID_PROC_FALLBACK=1 \
        TMPDIR="$interruption_runtime_root" ICECC_TEST_CACHE_SERVICE="$service" \
        "$baseline" >"$interruption_log" 2>&1 &
    interruption_child_pid=$!
    while ! test -s "$interruption_ready_trace"; do
        kill -0 "$interruption_child_pid" 2>/dev/null || exit 1
        sleep 0.01
    done
    while ! test -s "$interruption_ready_hold"; do
        kill -0 "$interruption_child_pid" 2>/dev/null || exit 1
        sleep 0.01
    done
    interruption_wrapper_record_file=$(mktemp "$tmp_root/interruption-wrapper-records.XXXXXX") || {
        stop_interruption_child
        exit 1
    }
    interruption_wrapper_pids_file=$(mktemp "$tmp_root/interruption-wrapper-pids.XXXXXX") || {
        rm -f -- "$interruption_wrapper_record_file"
        stop_interruption_child
        exit 1
    }
    interruption_sidecar_wait=0
    while test "$interruption_sidecar_wait" -lt 40; do
        if mutant_sidecar_records "$interruption_runtime_root" \
                >"$interruption_wrapper_record_file" && \
                records_to_pids "$interruption_wrapper_record_file" \
                    "$interruption_wrapper_pids_file"; then
            if interruption_wrapper_pids=$(cat "$interruption_wrapper_pids_file"); then
                :
            else
                rm -f -- "$interruption_wrapper_record_file" "$interruption_wrapper_pids_file"
                stop_interruption_child
                exit 1
            fi
        else
            rm -f -- "$interruption_wrapper_record_file" "$interruption_wrapper_pids_file"
            stop_interruption_child
            exit 1
        fi
        test -n "$interruption_wrapper_pids" && break
        sleep 0.05
        interruption_sidecar_wait=$((interruption_sidecar_wait + 1))
    done
    if test -z "$interruption_wrapper_pids"; then
        rm -f -- "$interruption_wrapper_record_file" "$interruption_wrapper_pids_file"
        stop_interruption_child
        exit 1
    fi
    rm -f -- "$interruption_wrapper_record_file" "$interruption_wrapper_pids_file" || exit 1
    # Hold the baseline at the authenticated READY edge until the parent has
    # observed the exact sidecar record.  The parent then requests the
    # interruption, so the sidecar cannot disappear before ownership polling
    # has established the target.
    while ! test -e "$interruption_abort_trace"; do
        kill -0 "$interruption_child_pid" 2>/dev/null || exit 1
        sleep 0.01
    done
    stop_interruption_child
    interruption_status=0
    retire_mutant_sidecars "$interruption_runtime_root" || exit 1
    rm -rf -- "$interruption_runtime_root" || exit 1
    exit "$interruption_status"
) &
interruption_wrapper_pid=$!
interruption_sidecar_pids=
interruption_parent_record_file=$(mktemp "$tmp_root/interruption-parent-records.XXXXXX") || {
    stop_interruption_wrapper "$interruption_wrapper_pid"
    exit 1
}
interruption_parent_pids_file=$(mktemp "$tmp_root/interruption-parent-pids.XXXXXX") || {
    rm -f -- "$interruption_parent_record_file"
    stop_interruption_wrapper "$interruption_wrapper_pid"
    exit 1
}
interruption_poll=0
while test ! -s "$interruption_ready_trace" && \
        kill -0 "$interruption_wrapper_pid" 2>/dev/null && \
        test "$interruption_poll" -lt 200; do
    sleep 0.05
    interruption_poll=$((interruption_poll + 1))
done
if test ! -s "$interruption_ready_trace"; then
    rm -f -- "$interruption_parent_record_file" "$interruption_parent_pids_file"
    stop_interruption_wrapper "$interruption_wrapper_pid"
    echo 'FAIL: interruption regression did not publish a complete READY trace' >&2
    cat "$interruption_log" >&2
    exit 1
fi
interruption_poll=0
while test "$interruption_poll" -lt 200; do
    if mutant_sidecar_records "$interruption_runtime_root" \
            >"$interruption_parent_record_file" && \
            records_to_pids "$interruption_parent_record_file" \
                "$interruption_parent_pids_file"; then
        if interruption_sidecar_pids=$(cat "$interruption_parent_pids_file"); then
            :
        else
            rm -f -- "$interruption_parent_record_file" "$interruption_parent_pids_file"
            stop_interruption_wrapper "$interruption_wrapper_pid"
            echo 'FAIL: interruption READY PID extraction failed' >&2
            exit 1
        fi
    else
        rm -f -- "$interruption_parent_record_file" "$interruption_parent_pids_file"
        stop_interruption_wrapper "$interruption_wrapper_pid"
        echo 'FAIL: interruption READY identity did not resolve to its exact sidecar' >&2
        exit 1
    fi
    test -n "$interruption_sidecar_pids" && break
    kill -0 "$interruption_wrapper_pid" 2>/dev/null || break
    sleep 0.05
    interruption_poll=$((interruption_poll + 1))
done
if test -z "$interruption_sidecar_pids"; then
    rm -f -- "$interruption_parent_record_file" "$interruption_parent_pids_file"
    stop_interruption_wrapper "$interruption_wrapper_pid"
    echo 'FAIL: interruption regression did not observe a live cache sidecar' >&2
    cat "$interruption_log" >&2
    exit 1
fi
interruption_socket_path=$(sed -n '1p' "$interruption_parent_record_file" |
    cut -d'|' -f6)
case "$interruption_socket_path" in
    /tmp/p5i.*/*) ;;
    *)
        echo 'FAIL: interruption record escaped its runtime root' >&2
        exit 1
        ;;
esac
interruption_sidecar_pid=$(sed -n '1p' "$interruption_parent_record_file" |
    cut -d'|' -f1)
if listener_socket_identity_for_pid "$interruption_sidecar_pid" \
        "$interruption_socket_path.not-owned" >/dev/null 2>&1; then
    echo 'FAIL: listener identity was not bound to its --socket pathname' >&2
    exit 1
fi
echo 'ok - listener identity is bound to the exact --socket pathname'
rm -f -- "$interruption_socket_path"
echo 'ok - interruption cleanup retains listener identity after pathname removal'
: >"$interruption_abort_trace"
kill -TERM "$interruption_wrapper_pid"
set +e
wait "$interruption_wrapper_pid"
interruption_status=$?
set -e
interruption_final_records_file=$(mktemp "$tmp_root/interruption-final-records.XXXXXX")
interruption_final_pids_file=$(mktemp "$tmp_root/interruption-final-pids.XXXXXX")
if mutant_sidecar_records "$interruption_runtime_root" >"$interruption_final_records_file" && \
        records_to_pids "$interruption_final_records_file" "$interruption_final_pids_file"; then
    if interruption_final_pids=$(cat "$interruption_final_pids_file"); then
        :
    else
        interruption_final_pids=parser-output-error
    fi
else
    interruption_final_pids=parser-error
fi
rm -f -- "$interruption_parent_record_file" "$interruption_parent_pids_file" \
    "$interruption_final_records_file" "$interruption_final_pids_file"
if test "$interruption_status" -ne 0 || \
        test -n "$interruption_final_pids" || \
        test -e "$interruption_runtime_root"; then
    echo "FAIL: interrupted baseline left cache sidecar/runtime (status $interruption_status)" >&2
    cat "$interruption_log" >&2
    exit 1
fi
echo 'ok - forced interruption retires the exact live cache sidecar'
# The READY trace above belongs only to the interrupted baseline.  Mutant
# cleanup must return to its own runtime-root-scoped /proc inventory so a
# detached attempt-2 service cannot be hidden behind the dead baseline PID.
unset interruption_ready_trace
unset ICECC_P50_TEST_FORBID_PROC_FALLBACK

compile_and_expect_red() {
    label=$1
    mutant=$2
    expected_status=$3
    object="$tmp_root/$label.o"
    binary="$tmp_root/$label"
    log="$tmp_root/$label.log"
    # Keep the socket path short enough for sockaddr_un while retaining an
    # exact, test-owned prefix for detached-child cleanup.
    runtime_root=$(mktemp -d /tmp/p5m.XXXXXX)
    runtime_roots="$runtime_roots $runtime_root"
    "$cxx" "$standard" -Wall -Wextra -Werror -pthread -DHAVE_CONFIG_H \
        -I"$top_build" -I"$top_src" -I"$top_src/cache" \
        -I"$top_src/client" -I"$top_src/services" \
        ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
        -c "$mutant" -o "$object"
    link_binary "$object" "$binary"
    # A rejected mutant can exit before its detached cache-service child.
    # Retire only the service tied to this mutant's unique runtime root.
    set +e
    TMPDIR="$runtime_root" ICECC_TEST_CACHE_SERVICE="$service" \
        timeout 60s "$binary" >"$log" 2>&1
    status=$?
    retire_mutant_sidecars "$runtime_root"
    cleanup_status=$?
    set -e
    if test "$cleanup_status" -ne 0; then
        echo "FAIL: $label mutant left a cache-service child" >&2
        exit 1
    fi
    # Exact retirement above has already proved this mutant owns no survivor.
    # Remove it from the EXIT-trap inventory instead of repeating the bounded
    # 20-scan publication wait for an already-settled runtime root.
    rm -rf -- "$runtime_root"
    forget_runtime_root "$runtime_root"
    if test "$status" -ne "$expected_status"; then
        echo "FAIL: $label mutant returned $status, expected $expected_status" >&2
        cat "$log" >&2
        exit 1
    fi
    echo "ok - $label mutant is rejected (status $status)"
}

exact_mode_mutant="$tmp_root/exact-mode.cpp"
sed '0,/(info.st_mode & 07777) != 0700/s//(info.st_mode \& 0077) != 0/' \
    "$src" >"$exact_mode_mutant"
cmp -s "$src" "$exact_mode_mutant" && {
    echo 'FAIL: exact-mode mutant was not applied' >&2
    exit 1
}
compile_and_expect_red exact-mode "$exact_mode_mutant" 7

same_identity_mutant="$tmp_root/same-identity.cpp"
sed 's/config.expected_service_uid != config.expected_daemon_uid ||/false ||/' \
    "$src" >"$same_identity_mutant"
cmp -s "$src" "$same_identity_mutant" && {
    echo 'FAIL: same-identity mutant was not applied' >&2
    exit 1
}
compile_and_expect_red same-identity "$same_identity_mutant" 6

runtime_revalidation_mutant="$tmp_root/runtime-revalidation.cpp"
sed '/bool DaemonSidecarAdapter::runtime_nodes_valid()/! s/runtime_nodes_valid()/true/g' \
    "$src" >"$runtime_revalidation_mutant"
cmp -s "$src" "$runtime_revalidation_mutant" && {
    echo 'FAIL: runtime-revalidation mutant was not applied' >&2
    exit 1
}
compile_and_expect_red runtime-revalidation "$runtime_revalidation_mutant" 14

inode_cleanup_mutant="$tmp_root/inode-cleanup.cpp"
sed '0,/info.st_ino != expected_inode/s//true/' \
    "$src" >"$inode_cleanup_mutant"
cmp -s "$src" "$inode_cleanup_mutant" && {
    echo 'FAIL: inode-cleanup mutant was not applied' >&2
    exit 1
}
compile_and_expect_red inode-cleanup "$inode_cleanup_mutant" 11

echo 'PASS: daemon sidecar adapter production guards and executable mutants hold'
