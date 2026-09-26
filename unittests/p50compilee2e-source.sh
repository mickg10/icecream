#!/bin/sh
# Deletion-sensitive contract for the real Protocol-50 C1F1 compile gate.
#
# This is intentionally separate from the mechanism/unit tests. Those tests
# exercise codec, endpoint, sidecar and descriptor-handoff pieces in-process;
# this gate is allowed to pass only when the production daemon/client path
# actually owns those pieces. Until that wiring exists, return 77 (skip),
# never turn a legacy FileChunk compile into a false P50 success.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}

say_skip() {
    echo "SKIP: p50 C1F1 compile wiring is not installed: $*" >&2
    exit 77
}

require_text() {
    file=$1
    needle=$2
    grep -F -- "$needle" "$file" >/dev/null 2>&1
}

contract() {
    root=$1

    # Daemon-side lifecycle ownership and cache-session bridge must be in the
    # production daemon, not only in standalone P50 unit tests.
    require_text "$root/daemon/main.cpp" 'DaemonSidecarAdapter' || return 1
    require_text "$root/daemon/main.cpp" 'observe_public_listener' || return 1
    require_text "$root/daemon/main.cpp" 'local_only_cache_adapter' || return 1
    require_text "$root/daemon/main.cpp" 'config.public_listener_port = local_only_cache_adapter' || return 1
    require_text "$root/daemon/main.cpp" 'cache_adapter' || return 1
    require_text "$root/daemon/Makefile.am" 'libp50daemonsidecaradapter.a' || return 1

    # Both C and F compile paths must name the P50 transaction. Merely
    # forwarding a legacy CompileFile/FileChunk stream is not this gate.
    require_text "$root/client/remote.cpp" 'ZSTD_TU' || return 1
    require_text "$root/daemon/serve.cpp" 'ZSTD_TU' || return 1
    require_text "$root/daemon/workit.cpp" 'ZSTD_TU' || return 1
    # The required-mode knob is a fail-closed contract: a missing sidecar or
    # P50 route must fail the requested test, never silently use FileChunk.
    require_text "$root/client/remote.cpp" 'ICECC_P50_C1F1_REQUIRED' || return 1
    require_text "$root/client/remote.cpp" \
        'strict all-P50 run refuses scheduler-selected localhost' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" \
        'worker_scheduler_host=${ICECC_P50_C1F1_WORKER_SCHEDULER_HOST:-}' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" \
        '-s "$worker_scheduler_host:$port_sched"' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" \
        'accepted $worker_scheduler_host' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" \
        'I am known as $worker_scheduler_host' || return 1
    require_text "$root/client/remote.cpp" 'ICECC_P50_C1F1_TIMEOUT' || return 1
    require_text "$root/daemon/workit.cpp" 'ICECC_P50_C1F1_REQUIRED' || return 1
    require_text "$root/client/remote.cpp" 'job.setCompileInputIdentity(*identity)' || return 1
    # Both deployed cache revisions stay wired through the actual wrapper:
    # R1 retains its one-shot FD request; opt-in R2 performs the P51 lease and
    # ARM on the original compiler channel before its bounded kind-8 transfer.
    require_text "$root/client/remote.cpp" 'P50CacheSessionFdRequestMsg(fd_request)' || return 1
    require_text "$root/client/remote.cpp" 'receive_p50_cache_fd_reply' || return 1
    require_text "$root/client/remote.cpp" 'P51SourceLeaseRequestMsg(lease_request)' || return 1
    require_text "$root/client/remote.cpp" 'receive_p51_cache_fd_reply' || return 1
    require_text "$root/client/remote.cpp" 'P51SourceArmMsg arm_message' || return 1
    require_text "$root/client/remote.cpp" 'P51SourceTransferRequest transfer_request' || return 1
    require_text "$root/client/remote.cpp" 'make_p51_source_transfer_operation' || return 1
    require_text "$root/client/remote.cpp" 'transfer_p51_source(' || return 1
    require_text "$root/client/remote.cpp" 'begin_authenticated' || return 1
    require_text "$root/client/remote.cpp" 'make_source_transfer_operation' || return 1
    require_text "$root/client/remote.cpp" 'source.release()' || return 1
    require_text "$root/client/remote.cpp" 'control.advance' || return 1
    for field in wire_job_id assignment_epoch assignment_nonce selected_f_host \
        selected_f_ordinary_port selected_f_cache_port cache_protocol cache_profile \
        logical_job compiler_attempt source_request_id source_mode; do
        require_text "$root/client/remote.cpp" "request.$field" || return 1
    done
    if grep -F 'P50ZstdSourceSender sender' "$root/client/remote.cpp" >/dev/null ||
       grep -F 'begin_p50_client_transfer' "$root/client/remote.cpp" >/dev/null ||
       grep -F 'make_hello' "$root/client/remote.cpp" >/dev/null; then
        return 1
    fi
    require_text "$root/client/remote.cpp" 'std::chrono::seconds(120)' || return 1
    require_text "$root/client/remote.cpp" \
        'kP50CompilerConnectBudget = std::chrono::seconds(20)' || return 1
    require_text "$root/client/remote.cpp" \
        'const bool cache_advertised_assignment =' || return 1
    require_text "$root/client/remote.cpp" 'Service::createChannelRetryUntil(' || return 1
    require_text "$root/client/remote.cpp" \
        'kP50CompilerConnectAttemptBudget = std::chrono::seconds(5)' || return 1
    require_text "$root/client/remote.cpp" \
        'P50 compiler connection uses absolute 20-second deadline' || return 1
    require_text "$root/client/remote.cpp" \
        'Service::createChannel(hostname, port, 10)' || return 1
    # S7 may retain each successful preprocessor output only when the runner
    # opts in with a destination; the normal client path remains unchanged.
    require_text "$root/client/remote.cpp" 'ICECC_P50_PREPROCESSED_CAPTURE' || return 1
    require_text "$root/client/remote.cpp" 'retain_p50_preprocessed_capture' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" \
        "printf '%s/out/s7-%s-preprocessed.ii" || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" \
        "printf '%s/s7-%s-preprocessed.ii" || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" 's7-warm-c-action-trace.jsonl' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" 's7-warm-f-action-trace.jsonl' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" 'ICECC_P50_C_ACTION_TRACE=' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" 'ICECC_P50_F_ACTION_TRACE=' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" 's7-prewarm-c-action-trace.jsonl' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" 's7-measured-f-action-trace.jsonl' || return 1
    # Every supported selector must remain a live S7 profile cell.  The
    # runner maps the wire-level capability spelling separately from the
    # positive profile marker emitted by the client.
    require_text "$root/unittests/p50compilee2e-run.sh" 'P29V1) profile_advertisement=p29v1' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" 'ZSTD_TU) profile_advertisement=zstd_tu' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" 'ZSTD_ROUTE) profile_advertisement=zstd_route' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" 'profile_advertisement' || return 1
    # S2's process-loss gate must stop the exact F sidecar only after its
    # complete TX_BEGIN witness is published, then require the original
    # compiler invocation and its replacement/replay to finish.
    require_text "$root/cache/p50_actions.cpp" 'ICECC_P50_TEST_ACTION_HOLD' || return 1
    require_text "$root/cache/p50_actions.cpp" 'sync_file(fd' || return 1
    require_text "$root/cache/p50_actions.cpp" '::rename(temporary.c_str(), marker)' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" 's2_compile_with_process_loss' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" \
        'original compile did not recover after F sidecar loss' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" 's2_verify_process_loss' || return 1
    require_text "$root/research/farmharness/s8_external_farm_executor.py" \
        '-e ICECC_P50_TEST_ACTION_HOLD=F:TX_BEGIN' || return 1
    require_text "$root/research/farmharness/s8_external_farm_executor.py" \
        'S2_KILL before_pid=' || return 1
    require_text "$root/research/farmharness/s8_external_farm_executor.py" \
        'external-s2-process-loss.json' || return 1
    require_text "$root/client/Makefile.am" 'libp50zstdsender.a' || return 1
    require_text "$root/client/Makefile.am" 'libp50localtransport.a' || return 1
    require_text "$root/client/Makefile.am" 'libprotocol50.a' || return 1

    # The service and exact bounded codec are part of the executable topology,
    # rather than a fake socket peer supplied by this test.
    require_text "$root/cache/Makefile.am" 'icecc-cache-service' || return 1
    require_text "$root/cache/Makefile.am" 'libp50input.a' || return 1
    require_text "$root/cache/p50_cache_service.cpp" \
        'std::make_unique<P50ServerEndpoint>' || return 1
    # The live C1F1 runner must provision the submitter-side adapter as well
    # as F's advertised service; otherwise the production C handler correctly
    # fails closed before it can hand off the authenticated control socket.
    require_text "$root/unittests/p50compilee2e-run.sh" \
        '--cache-service "$build/cache/icecc-cache-service"' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" \
        '--cache-runtime-dir "$work/cache-runtime-c"' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" \
        'compile_binding = (db, db_sha, compile_source, compile_output) if db else ("-", "-", "-", "-")' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" \
        'batch compile binding has partial empty-column sentinels' || return 1
    require_text "$root/unittests/p51wrappercompile-run.sh" \
        'measured_offset=$(cat "$work/f-measured-log-offset-0")' || return 1
    require_text "$root/unittests/p51wrappercompile-run.sh" \
        'P51 cache-link descriptor adopted by sidecar' || return 1
}

# Keep primitive P50 evidence visible in this test. These assertions prevent
# an accidental rename/removal from making the wiring preflight look ready.
for pair in \
    "cache/p50_zstd.cpp|ZstdTuDialogue::begin" \
    "cache/p50_zstd.cpp|ZSTD_decompressStream" \
    "cache/p50_endpoint.cpp|run_adopted" \
    "cache/p50_cache_service.cpp|FdHandoffReceiver receiver" \
    "cache/p50_daemon_cache_dispatch.cpp|release_fd_if_input_empty"; do
    file=${pair%%|*}
    needle=${pair#*|}
    require_text "$src/$file" "$needle" ||
        say_skip "missing P50 primitive $file:$needle"
done

if ! contract "$src"; then
    say_skip "daemon/client compiler integration is still a hard prerequisite"
fi

# The batch TSV is read with tab in IFS, where adjacent empty columns collapse.
# Exercise the exact nonempty sentinel contract for no binding, a complete
# binding, and malformed partially-empty rows before relying on the runner.
decode_optional_binding() {
    row=$1
    IFS="$(printf '\t')" read -r db db_sha binding_source binding_output <<EOF
$row
EOF
    if test "$db" = "-"; then
        test "$db_sha" = "-" && test "$binding_source" = "-" && \
            test "$binding_output" = "-"
    else
        test "$db_sha" != "-" && test "$binding_source" != "-" && \
            test "$binding_output" != "-"
    fi
}
decode_optional_binding "-$(printf '\t')-$(printf '\t')-$(printf '\t')-" || {
    echo "FAIL: empty optional compile binding shifted TSV columns" >&2
    exit 1
}
decode_optional_binding "db.json$(printf '\t')abc123$(printf '\t')src.cpp$(printf '\t')out.o" || {
    echo "FAIL: complete optional compile binding rejected" >&2
    exit 1
}
if decode_optional_binding "db.json$(printf '\t')-$(printf '\t')src.cpp$(printf '\t')-"; then
    echo "FAIL: partially empty optional compile binding survived" >&2
    exit 1
fi

# Every production edge above is deletion-sensitive. Remove each required
# edge from a temporary copy and prove that the same contract rejects it. No
# runtime result can therefore survive deletion of one of the P50 owners.
mutant_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50compilee2e-source.XXXXXX")
trap 'rm -rf "$mutant_dir"' EXIT HUP INT TERM
cp -R "$src" "$mutant_dir/tree"

for pair in \
    "daemon/main.cpp|DaemonSidecarAdapter" \
    "daemon/main.cpp|observe_public_listener" \
    "daemon/main.cpp|cache_adapter" \
    "daemon/Makefile.am|libp50daemonsidecaradapter.a" \
    "client/remote.cpp|ZSTD_TU" \
    "daemon/serve.cpp|ZSTD_TU" \
    "daemon/workit.cpp|ZSTD_TU" \
    "client/remote.cpp|ICECC_P50_C1F1_REQUIRED" \
    "client/remote.cpp|strict all-P50 run refuses scheduler-selected localhost" \
    "unittests/p50compilee2e-run.sh|worker_scheduler_host=\${ICECC_P50_C1F1_WORKER_SCHEDULER_HOST:-}" \
    "unittests/p50compilee2e-run.sh|-s \"\$worker_scheduler_host:\$port_sched\"" \
    "unittests/p50compilee2e-run.sh|accepted \$worker_scheduler_host" \
    "unittests/p50compilee2e-run.sh|I am known as \$worker_scheduler_host" \
    "daemon/workit.cpp|ICECC_P50_C1F1_REQUIRED" \
    "client/remote.cpp|job.setCompileInputIdentity(*identity)" \
    "client/remote.cpp|P50CacheSessionFdRequestMsg(fd_request)" \
    "client/remote.cpp|P51SourceLeaseRequestMsg(lease_request)" \
    "client/remote.cpp|receive_p51_cache_fd_reply" \
    "client/remote.cpp|P51SourceArmMsg arm_message" \
    "client/remote.cpp|make_p51_source_transfer_operation" \
    "client/remote.cpp|receive_p50_cache_fd_reply" \
    "client/remote.cpp|begin_authenticated" \
    "client/remote.cpp|make_source_transfer_operation" \
    "client/remote.cpp|source.release()" \
    "client/Makefile.am|libp50localtransport.a" \
    "client/remote.cpp|std::chrono::seconds(120)" \
    "client/remote.cpp|ICECC_P50_PREPROCESSED_CAPTURE" \
    "client/remote.cpp|retain_p50_preprocessed_capture" \
    "unittests/p50compilee2e-run.sh|printf '%s/out/s7-%s-preprocessed.ii" \
    "unittests/p50compilee2e-run.sh|printf '%s/s7-%s-preprocessed.ii" \
    "unittests/p50compilee2e-run.sh|s7-warm-c-action-trace.jsonl" \
    "unittests/p50compilee2e-run.sh|s7-warm-f-action-trace.jsonl" \
    "unittests/p50compilee2e-run.sh|ICECC_P50_C_ACTION_TRACE=" \
    "unittests/p50compilee2e-run.sh|ICECC_P50_F_ACTION_TRACE=" \
    "unittests/p50compilee2e-run.sh|s7-prewarm-c-action-trace.jsonl" \
    "unittests/p50compilee2e-run.sh|s7-measured-f-action-trace.jsonl" \
    "unittests/p50compilee2e-run.sh|P29V1) profile_advertisement=p29v1" \
    "unittests/p50compilee2e-run.sh|ZSTD_TU) profile_advertisement=zstd_tu" \
    "unittests/p50compilee2e-run.sh|ZSTD_ROUTE) profile_advertisement=zstd_route" \
    "unittests/p50compilee2e-run.sh|profile_advertisement" \
    "cache/p50_actions.cpp|ICECC_P50_TEST_ACTION_HOLD" \
    "cache/p50_actions.cpp|sync_file(fd" \
    "cache/p50_actions.cpp|::rename(temporary.c_str(), marker)" \
    "unittests/p50compilee2e-run.sh|s2_compile_with_process_loss" \
    "unittests/p50compilee2e-run.sh|original compile did not recover after F sidecar loss" \
    "unittests/p50compilee2e-run.sh|s2_verify_process_loss" \
    "research/farmharness/s8_external_farm_executor.py|-e ICECC_P50_TEST_ACTION_HOLD=F:TX_BEGIN" \
    "research/farmharness/s8_external_farm_executor.py|S2_KILL before_pid=" \
    "research/farmharness/s8_external_farm_executor.py|external-s2-process-loss.json" \
    "client/Makefile.am|libp50zstdsender.a" \
    "cache/Makefile.am|libp50input.a" \
    "cache/p50_cache_service.cpp|std::make_unique<P50ServerEndpoint>" \
    "client/Makefile.am|libprotocol50.a"; do
    file=${pair%%|*}
    needle=${pair#*|}
    mutated="$mutant_dir/tree/$file.mutated"
    awk -v needle="$needle" 'index($0, needle) == 0' \
        "$mutant_dir/tree/$file" >"$mutated"
    mv -- "$mutated" "$mutant_dir/tree/$file"
    if contract "$mutant_dir/tree"; then
        echo "FAIL: P50 compile contract deletion mutant survived: $file:$needle" >&2
        exit 1
    fi
done

echo "ok - p50 C1F1 production wiring contract and deletion gates hold"
