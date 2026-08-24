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


def check_worker(serve: str, record: str) -> None:
    flow = section(serve, "int handle_connection(", "_exit(exit_code);")
    require(flow.count("unsigned int job_stat[8];") == 1,
            "legacy statistics buffer is not one native eight-word value")
    require(flow.count("write(out_fd, job_stat, sizeof(job_stat))") == 2,
            "legacy success/catch writes are not both preserved")
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
            "unregister_child(client->child_pid)")
    require("!p50_input &&\n        read(client->pipe_from_child" in flow,
            "legacy parent read is not isolated from the P50 record")
    require("p50_observation.valid()" in flow,
            "parent consumes an unvalidated P50 child record")
    require("p50_completion_matches_retained_lease" in flow,
            "parent does not bind terminal disposition to its retained lease")
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


def deletion_mutants(files: dict[str, str]) -> None:
    mutations = (
        ("client", "p50_disposition_attempted = true;", ""),
        ("client", "send_p50_disposition(ResultDispositionMsg::Accepted)",
         "send_deleted(ResultDispositionMsg::Accepted)"),
        ("client", "p50_result_received &&\n            !p50_disposition_attempted",
         "false"),
        ("serve", "write(out_fd, job_stat, sizeof(job_stat))", "write_deleted()"),
        ("serve", "write_output_file(obj_file, client);", "output_deleted();"),
        ("serve", "icecc::p50::receive_p50_result_disposition(",
         "receive_deleted("),
        ("serve", "P50CompletionDisposition::AttemptCancelOnly", "AttemptDeleted"),
        ("main", "InputLifecycleAction::CloseAcceptedJob", "InputLifecycleAction::None"),
        ("main", "InputLifecycleAction::CancelJob", "InputLifecycleAction::None"),
        ("main", "p50_completion_matches_retained_lease(", "lease_check_deleted("),
        ("record_h", "static_assert(kLegacyCompletionStatsWireSize == 32", "static_assert(true"),
        ("record_cpp", "flags | O_NONBLOCK", "flags"),
        ("record_cpp", "if (count != 0)", "if (false)"),
        ("record_cpp", "record.request_id == record.assignment_nonce", "true"),
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
