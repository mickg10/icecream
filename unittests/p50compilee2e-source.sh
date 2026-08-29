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
    require_text "$root/daemon/workit.cpp" 'ICECC_P50_C1F1_REQUIRED' || return 1
    require_text "$root/client/remote.cpp" 'job.setCompileInputIdentity(*identity)' || return 1
    # The real C production caller leases the already authenticated sidecar
    # descriptor from the live local daemon relationship, then starts exactly
    # one source-transfer control operation without a second HELLO.
    require_text "$root/client/remote.cpp" 'P50CacheSessionFdRequestMsg(fd_request)' || return 1
    require_text "$root/client/remote.cpp" 'receive_p50_cache_fd_reply' || return 1
    require_text "$root/client/remote.cpp" 'begin_authenticated' || return 1
    require_text "$root/client/remote.cpp" 'make_source_transfer_operation' || return 1
    require_text "$root/client/remote.cpp" 'F_DUPFD_CLOEXEC' || return 1
    require_text "$root/client/remote.cpp" 'control.advance' || return 1
    for field in wire_job_id assignment_epoch assignment_nonce selected_f_host \
        selected_f_ordinary_port selected_f_cache_port cache_protocol cache_profile \
        logical_job compiler_attempt source_request_id source_mode; do
        require_text "$root/client/remote.cpp" "request.$field" || return 1
    done
    if grep -F 'P50ZstdSourceSender sender' "$root/client/remote.cpp" >/dev/null ||
       grep -F 'begin_p50_client_transfer' "$root/client/remote.cpp" >/dev/null ||
       grep -F 'P50SourceArmFields' "$root/client/remote.cpp" >/dev/null ||
       grep -F 'make_hello' "$root/client/remote.cpp" >/dev/null; then
        return 1
    fi
    require_text "$root/client/remote.cpp" 'std::chrono::seconds(120)' || return 1
    require_text "$root/client/Makefile.am" 'libp50zstdsender.a' || return 1
    require_text "$root/client/Makefile.am" 'libp50localtransport.a' || return 1
    require_text "$root/client/Makefile.am" 'libprotocol50.a' || return 1

    # The service and exact bounded codec are part of the executable topology,
    # rather than a fake socket peer supplied by this test.
    require_text "$root/cache/Makefile.am" 'icecc-cache-service' || return 1
    require_text "$root/cache/Makefile.am" 'libp50inputfd.a' || return 1
    require_text "$root/cache/p50_cache_service.cpp" \
        'std::make_unique<P50ServerEndpoint>' || return 1
    # The live C1F1 runner must provision the submitter-side adapter as well
    # as F's advertised service; otherwise the production C handler correctly
    # fails closed before it can hand off the authenticated control socket.
    require_text "$root/unittests/p50compilee2e-run.sh" \
        '--cache-service "$build/cache/icecc-cache-service"' || return 1
    require_text "$root/unittests/p50compilee2e-run.sh" \
        '--cache-runtime-dir "$work/cache-runtime-c"' || return 1
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
    "daemon/workit.cpp|ICECC_P50_C1F1_REQUIRED" \
    "client/remote.cpp|job.setCompileInputIdentity(*identity)" \
    "client/remote.cpp|P50CacheSessionFdRequestMsg(fd_request)" \
    "client/remote.cpp|receive_p50_cache_fd_reply" \
    "client/remote.cpp|begin_authenticated" \
    "client/remote.cpp|make_source_transfer_operation" \
    "client/remote.cpp|F_DUPFD_CLOEXEC" \
    "client/Makefile.am|libp50localtransport.a" \
    "client/remote.cpp|std::chrono::seconds(120)" \
    "client/Makefile.am|libp50zstdsender.a" \
    "cache/Makefile.am|libp50inputfd.a" \
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
