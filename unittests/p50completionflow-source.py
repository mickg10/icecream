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


def check_client(source: str) -> None:
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


def check_cache_service(source: str) -> None:
    ready_trace = section(source, "void append_ready_test_trace(",
                          "void append_terminal_lifecycle_test_trace(")
    for token in ("ICECC_P50_C1F1_REQUIRED", "ICECC_P50_TEST_READY_TRACE",
                  "O_APPEND", "O_CLOEXEC", "::write(fd"):
        require(token in ready_trace, f"sidecar READY evidence omits {token}")
    ready = section(source, "bool write_ready_lease(",
                    "bool capture_listener_identity(")
    ordered(ready, "const bool written = write_exact(fd, message);",
            "if (written)", "append_ready_test_trace(message);",
            "return written;")
    helper = section(source, "void append_terminal_lifecycle_test_trace(",
                     "volatile sig_atomic_t g_stop_requested")
    for token in ("ICECC_P50_C1F1_REQUIRED",
                  "ICECC_P50_TEST_LIFECYCLE_TRACE",
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
                  "did not retain incomplete rows as HOLD"):
        require(token in source, f"real terminal/reclaim matrix omits {token}")
    ordered(source,
            "run_remote_cell malformed malformed",
            "restart_cache_sidecar",
            "run_remote_cell disconnect disconnect",
            "restart_cache_sidecar",
            "PASS: real P50 Accepted/DefinitiveCancel/malformed/disconnect")


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
    check_client(files["client"])
    check_worker(files["serve"], files["record_h"] + files["record_cpp"])
    check_parent(files["main"])
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
        ("record_h", "static_assert(kLegacyCompletionStatsWireSize == 32", "static_assert(true"),
        ("record_cpp", "flags | O_NONBLOCK", "flags"),
        ("record_cpp", "if (count != 0)", "if (false)"),
        ("record_cpp", "record.request_id == record.assignment_nonce", "true"),
        ("cache_service", "ICECC_P50_TEST_LIFECYCLE_TRACE", "TRACE_DELETED"),
        ("cache_service", "ICECC_P50_TEST_READY_TRACE", "READY_TRACE_DELETED"),
        ("cache_service", "if (mutated && decision.collect_record)\n                    endpoint_->collect_input_garbage();",
         "if (mutated && decision.collect_record)\n                    collect_deleted();"),
        ("runtime_gate", "kill -9 \"$old_pid\"", "kill_deleted"),
        ("runtime_gate", "--identity-trace \"$work/compile-identity.jsonl\"", ""),
        ("runtime_gate", "run_remote_cell disconnect disconnect", "disconnect_deleted"),
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
        "serve": (ROOT / "daemon/serve.cpp").read_text(),
        "main": (ROOT / "daemon/main.cpp").read_text(),
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
