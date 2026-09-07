#!/usr/bin/env python3
"""Deletion-sensitive source gate for the production P50 completion flow."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class GateFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateFailure(message)


def section(source: str, begin: str, end: str) -> str:
    first = source.find(begin)
    require(first >= 0, f"missing section start: {begin}")
    last = source.find(end, first + len(begin))
    require(last >= 0, f"missing section end: {end}")
    return source[first:last]


def ordered(source: str, *tokens: str) -> None:
    offset = 0
    for token in tokens:
        found = source.find(token, offset)
        require(found >= 0, f"missing/out-of-order anchor: {token}")
        offset = found + len(token)


def check_client(source: str, makefile: str) -> None:
    flow = section(source, "static int build_remote_int(", "static string\nmd5_for_file")
    require(flow.count("p50_disposition_attempted = true;") == 1,
            "submitter does not enforce one disposition attempt")
    require("p50_result_received &&\n            !p50_disposition_attempted" in flow,
            "post-result exception is not fenced from pre-result failure")
    require(flow.count("send_p50_disposition(ResultDispositionMsg::Accepted)") == 1,
            "submitter Accepted is not exactly-once in production")
    require(flow.count("send_p50_disposition(ResultDispositionMsg::DefinitiveCancel)") >= 4,
            "fallback/generic definitive-cancel paths are incomplete")
    ordered(flow,
            "CompileResultMsg *crmsg",
            "if (!crmsg->compileIdentityMatches(job))",
            "append_p50_compile_identity_trace(job, *crmsg);",
            "p50_result_received = p50_input;",
            "receive_file(job.outputFile(), cserver);",
            "send_p50_disposition(ResultDispositionMsg::Accepted)")

    workaround = section(flow, "if ((!crmsg->out.empty()", "if (crmsg->err.find")
    ordered(workaround,
            "send_p50_disposition(ResultDispositionMsg::DefinitiveCancel)",
            "discard_p50_output_file(cserver)",
            "throw remote_error(102")
    missing = section(flow, "if (crmsg->err.find", "ignore_result(write(STDOUT_FILENO")
    ordered(missing,
            "send_p50_disposition(ResultDispositionMsg::DefinitiveCancel)",
            "discard_p50_output_file(cserver)",
            "throw remote_error(104")
    require("failed to acknowledge complete P50 remote result" in flow,
            "Accepted send failure does not fail closed")
    require("ICECC_P50_C1F1_REQUIRED" in flow and
            "ICECC_P50_TEST_DISPOSITION" in flow,
            "real-runtime fault injection is not doubly test-gated")
    require('selected == "malformed"' in flow and
            "cserver->send_msg(EndMsg())" in flow,
            "malformed real-runtime witness is missing")
    require('selected == "disconnect"' in flow and
            "delete cserver;" in flow and "cserver = nullptr;" in flow,
            "disconnect real-runtime witness is missing")
    require(source.count("#ifdef ICECC_P50_COMPLETION_TEST_HOOKS") == 3,
            "completion fault/trace code is not fully compile-time isolated")
    require("check_PROGRAMS = icecc-p50-completion-test" in makefile and
            "-DICECC_P50_COMPLETION_TEST_HOOKS" in makefile and
            "icecc_p50_completion_test_SOURCES = main.cpp remote.cpp" in makefile,
            "fresh-retry seam does not have a check-only client target")
    completion_hooks = section(
        flow,
        "#ifdef ICECC_P50_COMPLETION_TEST_HOOKS\n        /* The real C1F1",
        "#endif\n        const ResultDispositionMsg")
    for token in (
            'const char *test_hook = getenv("ICECC_P50_TEST_DISPOSITION")',
            'selected == "malformed"',
            "cserver->send_msg(EndMsg())",
            'selected == "disconnect"',
            'string(test_hook) == "accepted-send-fail"',
            'string(retry_gate) == "1"',
            "status == 0 && p50_result_received",
            "job.compileInputIdentity().validPresent()",
            "delete cserver;",
            "cserver = nullptr;",
            '"input_present\\\":1',
            '"input_c_store_guid\\\":\\\"%s',
            '"input_tu_seq\\\":%llu',
            '"raw_bytes\\\":%llu',
            '"raw_digest\\\":\\\"%s',
            '"attempt_id\\\":%llu',
            '"request_id\\\":%llu',
            "p50_disposition_sent = false;",
            "return false;"):
        require(token in completion_hooks,
                f"check-only completion fault seam omits {token}")
    local_flow = section(source, "static bool\nmaybe_build_local(",
                         "// Minimal version of remote host")
    ordered(local_flow,
            "usecs->applyAssignmentTo(&job)",
            "append_p50_fresh_legacy_local_trace(job, *usecs);",
            "CompileFileMsg compile_file(&job);",
            "local_daemon->send_msg(compile_file)")
    require("normalizing P50 client error " in source and
            "error.errorCode == 107" in source,
            "107 is not observably normalized into the bounded retry class")


def check_worker(serve: str, record: str) -> None:
    flow = section(serve, "int handle_connection(", "_exit(exit_code);")
    require(flow.count("unsigned int job_stat[8];") == 1,
            "legacy statistics buffer is not one native eight-word value")
    require(flow.count("write(out_fd, job_stat, sizeof(job_stat))") == 2,
            "legacy success/catch writes are not both preserved")
    resource_failure = section(flow, "if (ret) {", "struct stat st;")
    ordered(resource_failure,
            "ret == EXIT_OUT_OF_MEMORY",
            "ret == EXIT_IO_ERROR",
            "rmsg.status = ret;",
            "job_stat[JobStatistics::exit_code] = ret;")
    ordered(flow,
            "if (!client->send_msg(rmsg))",
            "if (!p50_input)",
            "write(out_fd, job_stat, sizeof(job_stat))",
            "write_output_file(obj_file, client);",
            "icecc::p50::receive_p50_result_disposition(",
            "emit_p50_completion_and_close(")
    require("job->usesP50Input()) {\n                emit_p50_completion_and_close(" in flow,
            "worker catch does not emit an attempt-only P50 record")
    require(flow.count("P50CompletionDisposition::AttemptCancelOnly") == 2,
            "worker success/missing or exception attempt-only paths are incomplete")

    require("kLegacyCompletionStatsWireSize = 8 * sizeof(uint32_t)" in record and
            "static_assert(kLegacyCompletionStatsWireSize == 32" in record,
            "legacy 32-byte child ABI is not frozen")
    require("channel.get_msg(timeout_seconds, true)" in record,
            "worker disposition receive is not bounded")
    require("*message != Msg::RESULT_DISPOSITION" in record and
            "!disposition->same_identity(expected)" in record,
            "worker accepts a wrong frame or wrong result owner")
    require("return P50CompletionDisposition::AttemptCancelOnly" in record,
            "missing/malformed/disconnect is not attempt-only")


def check_parent(source: str) -> None:
    flow = section(source, "bool Daemon::handle_compile_done(",
                   "bool Daemon::handle_compile_file(")
    ordered(flow,
            "p50_completion_reader.emplace(",
            "p50_completion_reader->pump()",
            "P50CompletionPumpResult::Pending",
            "return true;",
            "complete_child_registration(client->child_pid)")
    child_completion = section(source, "static void complete_child_registration(",
                               "/* The exact quiescence barrier")
    require("waitpid(pid, &status, WNOHANG)" in child_completion and
            "ChildRecord::COMPLETION_OBSERVED" in child_completion,
            "normal completion does not retain the child for exact reaping")
    require("!p50_input &&\n        read(client->pipe_from_child" in flow,
            "legacy parent read is not isolated from the P50 record")
    require("p50_observation.valid()" in flow,
            "parent consumes an unvalidated P50 child record")
    require("p50_completion_matches_retained_lease" in flow,
            "parent does not bind terminal disposition to its retained lease")
    require("cache_adapter->outer_immediate_turn_required()" in source,
            "daemon poll owner can sleep through finite sidecar work")
    ordered(flow,
            "p50_completion_matches_retained_lease(",
            "InputLifecycleAction::CloseAcceptedJob",
            "InputLifecycleAction::CancelJob",
            "close(client->pipe_from_child)",
            "send_scheduler(*msg)",
            "handle_end(client, end_status)")
    lease = section(source, "static bool p50_completion_matches_retained_lease(",
                    "bool Daemon::handle_compile_done(")
    for token in ("logical_job == record.job_id",
                  "assignment_epoch == record.assignment_epoch",
                  "assignment_nonce == record.assignment_nonce",
                  "c_store_guid.bytes == record.c_store_guid",
                  "tu_seq.value == record.tu_seq",
                  "request_id == record.request_id",
                  "attempt_id == record.assignment_nonce"):
        require(token in lease, f"retained-lease comparison omits {token}")

    settlement = section(source, "void Daemon::settle_p50_input(",
                         "bool Daemon::handle_verify_env(")
    ordered(settlement,
            "cache_adapter->apply_input_lifecycle(lease, action)",
            "ICECC_P50_TEST_POST_TERMINAL_ATTACH",
            "cache_adapter->attach_input(",
            "InputFdAttachmentStatus::Accepted")
    require("attachment.fd.valid()" in settlement and
            "unexpectedly reattached consumed lease" in settlement,
            "post-settlement exact-owner probe is not fail-closed")
    legacy = section(source, "if (job->usesP50Input()) {",
                     "client->job = job;")
    require("P50 compiler input attachment unavailable" in legacy,
            "present unarmed P50 input no longer fails before local compilation")
    ordered(source,
            "client->job = job;",
            "if (!job->usesP50Input()",
            "legacy CompileFile admitted canonical input for job",
            "set_p50_legacy_wire_identity(identity)")


def check_compiler_quiescence(source: str, helper: str, makefile: str) -> None:
    flow = section(source, "/* The exact quiescence barrier",
                   "void Daemon::handle_old_request")
    require('#include "compiler_group_signal.h"' in source,
            "daemon does not use the shared compiler signal-authority primitive")
    for token in ("send_term_if_owned(", "observe_owned_anchor(",
                  "send_final_if_owned(", "settle_retired("):
        require(token in flow, f"quiescence omits shared primitive {token}")
    require("kill(-rec.pgid" not in flow and "kill(rec.pid" not in flow,
            "quiescence bypasses exact child-anchor signal authority")
    reaper = section(source, "if (!child_registry.empty()) {",
                     "/* Push queued state records")
    require("observe_owned_anchor(" in reaper,
            "generic reaper consumes an unfinished compiler leader anchor")

    for token in (
            "WEXITED | WNOHANG | WNOWAIT",
            "if (!authority.active || authority.term_sent || authority.final_sent",
            "if (!authority.active || authority.final_sent",
            "if (leader <= 0 || pgid <= 0 || leader != pgid)",
            "authority.final_sent = true;\n    authority.active = false;",
            "if (authority.active)\n        return false;",
            "operations.consume_anchor(leader)",
            "operations.observe_group(pgid)"):
        require(token in helper, f"signal-authority helper omits {token}")
    ordered(helper,
            "authority.final_sent = true;",
            "authority.active = false;",
            "bool settle_retired(",
            "operations.consume_anchor(leader)",
            "operations.observe_group(pgid)")
    require("check_PROGRAMS += p50compilerquiescence" in makefile and
            "TESTS += p50compilerquiescence" in makefile and
            "p50_compiler_quiescence_test.cpp" in makefile,
            "shared signal authority lacks a compiled runtime test")


def check_cache_service(source: str) -> None:
    ready_trace = section(source, "void append_ready_test_trace(",
                          "void append_terminal_lifecycle_test_trace(")
    for token in ("ICECC_P50_TEST_READY_TRACE", "O_APPEND", "O_CLOEXEC",
                  "::write(fd"):
        require(token in ready_trace, f"sidecar READY evidence omits {token}")
    require("ICECC_P50_C1F1_REQUIRED" not in ready_trace,
            "READY evidence must be opted in by its path, not workload strictness")
    ready = section(source, "bool write_ready_lease(",
                    "bool capture_listener_identity(")
    ordered(ready, "const bool written = write_exact(fd, message);",
            "if (written)", "append_ready_test_trace(message);",
            "return written;")
    source_trace = section(source, "void append_source_result_trace(",
                           "void append_terminal_lifecycle_test_trace(")
    for token in ("transfer.raw_digest", "digest128_hex",
                  '"raw_digest\\\":\\\"%s',
                  '"source_mutex_wait_ns\\\":%llu',
                  '"source_mutex_service_ns\\\":%llu',
                  '"terminal_error_code\\\":%u',
                  '"terminal_error_name\\\":%s',
                  "ErrorCode::WIRE_REVISION_MISMATCH"):
        require(token in source_trace,
                f"C-side source-result trace omits {token}")
    transfer = section(source, "SidecarRuntime::transfer_source_on_owner(",
                       "bool SidecarRuntime::bind_route_endpoint_identity(")
    ordered(transfer,
            "const auto source_mutex_wait_start = std::chrono::steady_clock::now();",
            "source_transfer_lock.try_lock_until(",
            "const auto source_mutex_service_start = std::chrono::steady_clock::now();",
            "source_mutex_service_start - source_mutex_wait_start",
            "std::chrono::steady_clock::now() -\n                        source_mutex_service_start",
            "append_source_result_trace(",
            "completion->set_value(value)")
    helper = section(source, "void append_terminal_lifecycle_test_trace(",
                     "volatile sig_atomic_t g_stop_requested")
    for token in ("ICECC_P50_C1F1_REQUIRED",
                  "ICECC_P50_TEST_LIFECYCLE_TRACE",
                  "c_store_guid=%s", "input_tu=%llu",
                  "before_records=%zu", "after_records=%zu",
                  "before_bytes=%llu", "after_bytes=%llu",
                  "O_APPEND", "O_CLOEXEC"):
        require(token in helper, f"sidecar lifecycle trace omits {token}")
    apply = section(source, "SidecarRuntime::apply_input_lifecycle_on_owner(",
                    "void SidecarRuntime::cancel_active_control(")
    ordered(apply,
            "const P50ServerOwnerUsage before = endpoint_->owner_usage();",
            "input_lifecycle_.begin_apply(request)",
            "endpoint_->close_input_job(request.key)",
            "endpoint_->collect_input_garbage()",
            "input_lifecycle_.finish_apply(request, mutated)",
            "append_terminal_lifecycle_test_trace(",
            "completion->set_value(status)")


def check_runtime_gate(source: str) -> None:
    worker_launch = section(
        source,
        'ICECC_TEST_SOCKET="$work/worker.sock"',
        "worker_pid=$!")
    require("ICECC_P50_C1F1_REQUIRED=1" in worker_launch,
            "F daemon launch is not strict for the authoritative P50 cells")
    require('-s "$worker_scheduler_host:$port_sched"' in worker_launch and
            '-s "127.0.0.1:$port_sched"' not in worker_launch,
            "F daemon does not register through the explicit ordinary address")

    client_launch = section(
        source,
        'ICECC_TEST_SOCKET="$work/client.sock" \\\n    ICECC_P50_SOURCE_RESULT_TRACE="$work/source-result.jsonl"',
        "client_pid=$!")
    require("ICECC_P50_C1F1_REQUIRED" not in client_launch,
            "C daemon inherited strict mode and would refuse the intentional legacy retry")
    require('-s "127.0.0.1:$port_sched"' in client_launch,
            "C daemon no longer preserves loopback registration")

    retry_launch = section(
        source,
        'retry_marker="$work/fresh-retry.barrier"',
        "retry_wrapper_pid=$!")
    require("ICECC_P50_C1F1_REQUIRED" not in retry_launch,
            "fresh retry wrapper is strict and cannot exercise the 107/106 legacy retry")

    injected_fault_launch = section(
        source,
        "    malformed|disconnect)",
        "    *)")
    require('timeout "$timeout_s" "$work/bin/icecc"' in injected_fault_launch,
            "malformed/disconnect cells do not use the isolated check-only wrapper")
    require('timeout "$timeout_s" "$build/client/icecc"' not in injected_fault_launch,
            "malformed/disconnect fault injection still invokes the production wrapper")

    for token in ("run_remote_cell accepted accepted",
                  "run_remote_cell definitive definitive",
                  "run_remote_cell malformed malformed",
                  "run_remote_cell disconnect disconnect",
                  "ICECC_CARET_WORKAROUND=1",
                  "ICECC_P50_TEST_DISPOSITION=\"$mode\"",
                  "ICECC_P50_TEST_READY_TRACE=\"$work/ready.trace\"",
                  "restart_cache_sidecar",
                  "kill -9 \"$old_pid\"",
                  "P50 terminal test post-settlement attach job",
                  "after_records=0 after_bytes=0",
                  "after_records=1 after_bytes=[1-9][0-9]*",
                  "attempt_only_pids", "attempt_only_attempts",
                  "ICECC_P50_COMPILE_IDENTITY_TRACE",
                  "--identity-trace \"$work/compile-identity.jsonl\"",
                  'statuses != {"c_guid": "PASS", "tu_seq": "PASS"}',
                  "p50_runtime_evidence.py", "runtime.json",
                  "did not retain incomplete rows as HOLD",
                  'ICECC_P50_TEST_DISPOSITION=accepted-send-fail',
                  'ICECC_P50_TEST_FRESH_LEGACY_RETRY_BARRIER="$retry_marker"',
                  'ICECC_P50_SOURCE_RESULT_TRACE="$work/source-result.jsonl"',
                  "ICECC_P50_C1F1_WORKER_SCHEDULER_HOST",
                  "ipaddress.IPv4Address",
                  'ln -s "$build/client/icecc-p50-completion-test" "$work/bin/icecc"',
                  'readlink -f "$work/bin/icecc"',
                  'timeout "$timeout_s" "$work/bin/icecc"',
                  'accepted $worker_scheduler_host',
                  'I am known as $worker_scheduler_host',
                  'rm -f -- "$retry_remote_obj"',
                  'wait_for_count 3 \'action 1 status applied reason handle_end\'',
                  'wait_for_count 1 "END $first_retry_job status=0 .* server=p50-f"',
                  'wait_for_count 1 \'remove daemon p50-f\'',
                  ': >"$retry_release"',
                  "fresh-legacy-local-assignment",
                  "C-sidecar committed-source witness does not join the first marker",
                  'source.get("raw_digest") != first["raw_digest"]',
                  'input_present") != 0',
                  'type(second.get("port")) is not int or second["port"] != 0',
                  'scheduler_payload = "\\n".join(',
                  're.sub(r"^\\[\\d+\\] \\d{4}-\\d{2}-\\d{2} '
                  '\\d{2}:\\d{2}:\\d{2}: ", "", line)',
                  "legacy CompileFile admitted canonical input for job",
                  'strings "$build/client/icecc"',
                  'strings "$build/client/icecc-p50-completion-test"'):
        require(token in source, f"real terminal/reclaim matrix omits {token}")
    ordered(source,
            "run_remote_cell malformed malformed",
            "restart_cache_sidecar",
            "run_remote_cell disconnect disconnect",
            "restart_cache_sidecar",
            'retry_marker="$work/fresh-retry.barrier"',
            "ICECC_P50_TEST_DISPOSITION=accepted-send-fail",
            'rm -f -- "$retry_remote_obj"',
            'wait_for_count 1 "END $first_retry_job',
            'kill -TERM "$old_worker_pid"',
            'wait_for_count 1 \'remove daemon p50-f\'',
            ': >"$retry_release"',
            "PASS: real P50 terminal lifecycle plus 107/106 fresh scheduler-local retry")


def check_record(header: str, source: str) -> None:
    for token in ("Accepted = 1", "DefinitiveCancel = 2",
                  "AttemptCancelOnly = 3", "bool record_valid"):
        require(token in header, f"completion record omits {token}")
    require("flags | O_NONBLOCK" in source,
            "parent completion reader can block the daemon loop")
    ordered(source,
            "P50CompletionRecordReader::pump(",
            "checking_eof_ = true",
            "uint8_t trailing",
            "if (count != 0)",
            "decode_p50_completion_record(bytes_, &record)")
    require("record.stats[JobStatistics::exit_code] ==" in source,
            "record status is not bound to legacy statistics")
    require("record.attempt_id == record.assignment_nonce" in source and
            "record.request_id == record.assignment_nonce" in source,
            "child record is not nonce-bound")


def check_all(files: dict[str, str]) -> None:
    check_client(files["client"], files["client_make"])
    check_worker(files["serve"], files["record_h"] + files["record_cpp"])
    check_parent(files["main"])
    check_compiler_quiescence(files["main"], files["compiler_signal"],
                              files["unit_make"])
    check_record(files["record_h"], files["record_cpp"])
    check_cache_service(files["cache_service"])
    check_runtime_gate(files["runtime_gate"])


def deletion_mutants(files: dict[str, str]) -> None:
    mutations = (
        ("client", "p50_disposition_attempted = true;", ""),
        ("client", "send_p50_disposition(ResultDispositionMsg::Accepted)",
         "send_deleted(ResultDispositionMsg::Accepted)"),
        ("client", "p50_result_received &&\n            !p50_disposition_attempted",
         "false"),
        ("client", 'selected == "malformed"', "false"),
        ("client", 'selected == "disconnect"', "false"),
        ("client", "status == 0 && p50_result_received", "false"),
        ("client", "append_p50_fresh_legacy_local_trace(job, *usecs);", ""),
        ("client", "append_p50_compile_identity_trace(job, *crmsg);", ""),
        ("serve", "write(out_fd, job_stat, sizeof(job_stat))", "write_deleted()"),
        ("serve", "rmsg.status = ret;", "status_binding_deleted();"),
        ("serve", "job_stat[JobStatistics::exit_code] = ret;", "stats_binding_deleted();"),
        ("serve", "write_output_file(obj_file, client);", "output_deleted();"),
        ("serve", "icecc::p50::receive_p50_result_disposition(",
         "receive_deleted("),
        ("serve",
         "if (disposition ==\n"
         "                icecc::p50::P50CompletionDisposition::AttemptCancelOnly)",
         "if (false)"),
        ("main", "InputLifecycleAction::CloseAcceptedJob", "InputLifecycleAction::None"),
        ("main", "InputLifecycleAction::CancelJob", "InputLifecycleAction::None"),
        ("main", "p50_completion_matches_retained_lease(", "lease_check_deleted("),
        ("main", "cache_adapter->outer_immediate_turn_required()", "false"),
        ("main", "ICECC_P50_TEST_POST_TERMINAL_ATTACH", "PROBE_DELETED"),
        ("main", "legacy CompileFile admitted canonical input for job", "TRACE_DELETED"),
        ("main", "icecc::daemon_child::send_final_if_owned(",
         "final_signal_deleted("),
        ("compiler_signal", "WEXITED | WNOHANG | WNOWAIT",
         "WEXITED | WNOHANG"),
        ("compiler_signal",
         "authority.final_sent = true;\n    authority.active = false;",
         "authority.final_sent = true;"),
        ("record_h", "static_assert(kLegacyCompletionStatsWireSize == 32", "static_assert(true"),
        ("record_cpp", "flags | O_NONBLOCK", "flags"),
        ("record_cpp", "if (count != 0)", "if (false)"),
        ("record_cpp", "record.request_id == record.assignment_nonce", "true"),
        ("cache_service", "ICECC_P50_TEST_LIFECYCLE_TRACE", "TRACE_DELETED"),
        ("cache_service", "c_store_guid=%s", "c_store_guid=deleted"),
        ("cache_service", '"raw_digest\\\":\\\"%s',
         '"raw_digest_deleted\\\":\\\"%s'),
        ("cache_service", '"source_mutex_wait_ns\\\":%llu',
         '"source_mutex_wait_deleted\\\":%llu'),
        ("cache_service", '"source_mutex_service_ns\\\":%llu',
         '"source_mutex_service_deleted\\\":%llu'),
        ("cache_service", "ICECC_P50_TEST_READY_TRACE", "READY_TRACE_DELETED"),
        ("cache_service", "if (mutated && decision.collect_record)\n                    endpoint_->collect_input_garbage();",
         "if (mutated && decision.collect_record)\n                    collect_deleted();"),
        ("runtime_gate", "kill -9 \"$old_pid\"", "kill_deleted"),
        ("runtime_gate",
         'ICECC_TEST_SOCKET="$work/worker.sock" ICECC_P50_C1F1_REQUIRED=1',
         'ICECC_TEST_SOCKET="$work/worker.sock"'),
        ("runtime_gate", '-s "$worker_scheduler_host:$port_sched"',
         '-s "127.0.0.1:$port_sched"'),
        ("runtime_gate", 'I am known as $worker_scheduler_host',
         'ordinary_address_deleted'),
        ("runtime_gate",
         'ln -s "$build/client/icecc-p50-completion-test" "$work/bin/icecc"',
         'ln -s "$build/client/icecc" "$work/bin/icecc"'),
        ("runtime_gate",
         'ICECC_TEST_SOCKET="$work/client.sock" \\\n    ICECC_P50_SOURCE_RESULT_TRACE="$work/source-result.jsonl"',
         'ICECC_TEST_SOCKET="$work/client.sock" ICECC_P50_C1F1_REQUIRED=1 \\\n    ICECC_P50_SOURCE_RESULT_TRACE="$work/source-result.jsonl"'),
        ("runtime_gate",
         'retry_marker="$work/fresh-retry.barrier"',
         'retry_marker="$work/fresh-retry.barrier"\nICECC_P50_C1F1_REQUIRED=1'),
        ("runtime_gate", "--identity-trace \"$work/compile-identity.jsonl\"", ""),
        ("runtime_gate", "run_remote_cell disconnect disconnect", "disconnect_deleted"),
        ("runtime_gate", 'rm -f -- "$retry_remote_obj"', ""),
        ("runtime_gate", 'source.get("raw_digest") != first["raw_digest"]',
         "False"),
        ("runtime_gate",
         'type(second.get("port")) is not int or second["port"] != 0',
         "False"),
        ("runtime_gate", 'scheduler_payload = "\\n".join(',
         'scheduler_payload = scheduler\n# deleted normalization: "\\n".join('),
        ("runtime_gate", 'wait_for_count 1 \'remove daemon p50-f\'', "wait_deleted"),
        ("client_make", "-DICECC_P50_COMPLETION_TEST_HOOKS", ""),
    )
    for filename, old, new in mutations:
        require(old in files[filename], f"mutant anchor missing: {filename}: {old}")
        mutant = dict(files)
        mutant[filename] = files[filename].replace(old, new, 1)
        try:
            check_all(mutant)
        except GateFailure:
            continue
        raise GateFailure(f"deletion mutant survived: {filename}: {old}")


def main() -> int:
    files = {
        "client": (ROOT / "client/remote.cpp").read_text(),
        "client_make": (ROOT / "client/Makefile.am").read_text(),
        "serve": (ROOT / "daemon/serve.cpp").read_text(),
        "main": (ROOT / "daemon/main.cpp").read_text(),
        "compiler_signal": (ROOT / "daemon/compiler_group_signal.h").read_text(),
        "unit_make": (ROOT / "unittests/Makefile.am").read_text(),
        "record_h": (ROOT / "daemon/p50_completion_record.h").read_text(),
        "record_cpp": (ROOT / "daemon/p50_completion_record.cpp").read_text(),
        "cache_service": (ROOT / "cache/p50_cache_service.cpp").read_text(),
        "runtime_gate": (ROOT / "unittests/p50completionflow-run.sh").read_text(),
    }
    try:
        check_all(files)
        deletion_mutants(files)
    except (OSError, GateFailure) as error:
        print(f"p50 completion production-flow gate: FAIL: {error}", file=sys.stderr)
        return 1
    print("p50 completion production-flow gate: PASS")
    print("p50 completion deletion mutants: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
