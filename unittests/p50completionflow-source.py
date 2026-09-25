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
    require("constexpr int kRemoteCompileResultWaitSeconds = 12 * 60;" in source and
            "std::chrono::seconds(kRemoteCompileResultWaitSeconds)" in source,
            "P50 compiler operation timeout is not tied to the result wait")
    require(flow.count("cserver->setTcpUserTimeoutUntil(") == 2,
            "P50 compiler channel does not extend both admitted-operation owners")
    ordered(flow,
            "cserver->set_p50_legacy_wire_role(P50LegacyWireRole::C);",
            "if (cache_advertised_assignment &&",
            "cserver->setTcpUserTimeoutUntil(",
            "LegacyRemoteSink input_sink(cserver);")
    result_wait = section(flow, 'log_block wait_cs("wait for cs");',
                          "check_for_failure(msg, cserver);")
    ordered(result_wait,
            "if (p50_input &&",
            "cserver->setTcpUserTimeoutUntil(",
            "msg = cserver->get_msg(kRemoteCompileResultWaitSeconds);")
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
            'const bool legacy_retry_fault =',
            'const bool strict_retry_fault =',
            'string(retry_gate) == "1"',
            'string(strict_retry_gate) == "1"',
            "status == 0 && p50_result_received",
            "job.compileInputIdentity().validPresent()",
            'getenv("ICECC_P50_TEST_FRESH_STRICT_RETRY_SECOND_FAILURE")',
            "const bool barrier_exists =",
            "const bool repeat_strict_failure =",
            "(!barrier_exists || repeat_strict_failure)",
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


def check_wrapper_retry(main_source: str, remote_source: str,
                        client_header: str) -> None:
    retry = section(
        main_source,
        'invocation_timing_mark_enqueue("remote");',
        "invocation_timing_mark_finish(ret);")
    ordered(
        retry,
        'const bool strict_p50 =',
        'getenv("ICECC_P50_C1F1_REQUIRED") != nullptr',
        'const bool remote_required_retry =',
        'getenv("ICECC_REMOTE_REQUIRED") != nullptr',
        "bool p50_retry_attempted = false;",
        "string p50_retry_avoid_host;",
        "uint32_t p50_retry_avoid_port = 0;",
        "build_remote(job, local_daemon, envs, rate,",
        "!p50_retry_attempted || strict_p50",
        "p50_retry_avoid_host,",
        "p50_retry_avoid_port",
        "if (error.errorCode != 106)",
        "throw;",
        "const P50RetryDiagnostic& diagnostic =",
        "if (p50_retry_attempted)",
        '"suppressed_one_shot"',
        "strict_p50 && !error.hasRetryAvoidEndpoint()",
        '"P50 assignment failed; requesting one fresh strict-P50 remote assignment; avoiding failed endpoint "',
        '"P50 assignment failed; requesting one fresh legacy remote assignment"',
        "const bool retry_proxy_preclosed =",
        "local_daemon->at_eof()",
        "const bool retry_end_sent = !retry_proxy_preclosed &&",
        "local_daemon->send_msg(EndMsg())",
        "local_daemon->get_msg(30, true)",
        "(!strict_p50 && remote_required_retry &&",
        "retry_proxy_preclosed)",
        "retry_end_sent && retry_end_reply == nullptr",
        "delete local_daemon;",
        '"Error 24 - local daemon did not settle P50 retry predecessor"',
        "local_daemon = get_local_daemon();",
        "p50_retry_avoid_host = error.retryAvoidHost;",
        "p50_retry_avoid_port = error.retryAvoidPort;",
        "p50_retry_attempted = true;")
    require(retry.count("p50_retry_attempted = true;") == 1,
            "wrapper does not bound P50 reassignment to one attempt")
    require(
        '"P50 legacy retry predecessor proxy was already settled by local daemon; reconnecting"'
        in retry,
        "wrapper does not expose the authenticated scheduler-loss retry branch")
    require(retry.count("!strict_p50 && remote_required_retry &&") == 2,
            "preclosed proxy recovery is not confined to remote-required legacy retry")
    require("p50_legacy_retry" not in retry,
            "wrapper retained the strict-refusing legacy-only retry state")
    require("p50_completion_test_wait_before_retry_getcs(" in main_source and
            'getenv("ICECC_P50_TEST_STRICT_RETRY_BEFORE_GETCS_BARRIER")' in
                retry and
            "Error 28 - strict retry test barrier failed" in retry,
            "check-only wrapper cannot deterministically pause a real loss before fresh GetCS")

    require("const std::string retryAvoidHost;" in client_header and
            "const uint32_t retryAvoidPort;" in client_header and
            "hasRetryAvoidEndpoint() const noexcept" in client_header and
            "p50_cache_retry_avoid_is_present(" in client_header,
            "remote_error does not carry one immutable validated failed endpoint")

    remote_start = remote_source.find("int build_remote(")
    require(remote_start >= 0, "missing build_remote for strict retry check")
    remote = remote_source[remote_start:]
    ordered(
        remote,
        "bool request_p50,",
        "const std::string &retry_avoid_host,",
        "uint32_t retry_avoid_port)",
        "job.clearCompileInputIdentity();",
        "p50_cache_retry_avoid_is_present(",
        "retry_avoid_port, retry_avoid_host",
        "getcs.cache_retry_avoid_port = retry_avoid_port;",
        "getcs.cache_retry_avoid_host = retry_avoid_host;",
        "local_daemon->send_msg(getcs)")
    remote_error_catch = section(
        remote, "} catch (const remote_error &error) {",
        "} catch (const client_error &error) {")
    ordered(
        remote_error_catch,
        "const bool p50_assignment = usecs->hasCacheAdvertisement();",
        "const string failed_host = usecs->hostname;",
        "const uint32_t failed_port = usecs->port;",
        "publish_p50_observation();",
        "delete usecs;",
        "error.errorCode == 106 && p50_assignment",
        "p50_cache_retry_avoid_is_present(",
        "failed_port, failed_host",
        "error.errorCode, error.what(), failed_host, failed_port")
    transport_catch = section(
        remote, "} catch (const client_error &error) {", "} catch(...) {")
    ordered(
        transport_catch,
        "const bool p50_assignment = usecs->hasCacheAdvertisement();",
        "const string failed_host = usecs->hostname;",
        "const uint32_t failed_port = usecs->port;",
        "publish_p50_observation();",
        "delete usecs;",
        "if (p50_assignment && p50_transport_failure)",
        "106,",
        "failed_host, failed_port")
    reject = section(
        remote,
        "/* maybe_build_local() precedes the remote handoff checks below.",
        "int ret;")
    ordered(
        reject,
        "request_p50",
        'getenv("ICECC_P50_C1F1_REQUIRED") != nullptr',
        "!usecs->hasCacheAdvertisement()",
        "delete usecs;",
        "throw remote_error(",
        '"Error 105 - strict all-P50 assignment has no cache handoff"')
    require(remote.find(reject) < remote.find("maybe_build_local("),
            "strict no-cache assignment is rejected after local compilation")


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
    child_completion = section(source, "static bool complete_child_registration(",
                               "/* The exact quiescence barrier")
    require("waitpid(" not in child_completion and
            "record->second.completion_observed" in child_completion and
            "record->second.completion_observed = true;" in child_completion and
            "record->second.state" not in child_completion and
            "record->second.kind != ChildRecord::COMPILER" in child_completion and
            "release_slot_once(record->second.slot)" in child_completion,
            "normal completion is not a monotonic fact independent of OS cleanup, "
            "or omits once-only compiler slot release")
    require("bool completion_observed;" in source and
            "ChildRecord::COMPLETION_OBSERVED" not in source,
            "compiler completion is still encoded in the mutable signal phase")
    require("!p50_input &&\n        read(client->pipe_from_child" in flow,
            "legacy parent read is not isolated from the P50 record")
    require("p50_observation.valid()" in flow,
            "parent consumes an unvalidated P50 child record")
    require("p50_completion_matches_retained_lease" in flow,
            "parent does not bind terminal disposition to its retained lease")
    require("cache_adapter->outer_immediate_turn_required()" in source,
            "daemon poll owner can sleep through finite sidecar work")
    schedulerless = section(
        source,
        "if (!scheduler_cache_owner || scheduler == nullptr) {",
        "icecc::p50::advertisement::Update update;",
    )
    ordered(
        schedulerless,
        "cache_adapter->outer_set_scheduler_owner(false);",
        "invalidate_p50_source_waiters_for_lease()",
        "if (cache_adapter_start_attempted)",
        "cache_adapter->outer_begin_turn(",
        "return;",
    )
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


def check_cache_fd_replacement(main_source: str, comm_header: str,
                               comm_source: str) -> None:
    require("class P50CacheFdReplyTicket" in comm_header and
            "P50CacheFdReplyTicket(const P50CacheFdReplyTicket &) = delete;" in
                comm_header and
            "P50CacheFdReplyTicket &operator=(const P50CacheFdReplyTicket &) = delete;" in
                comm_header,
            "deferred descriptor reply authority is not move-only")

    take = section(
        comm_source,
        "P50CacheFdReplyTicket MsgChannel::take_p50_cache_fd_reply_ticket(",
        "bool MsgChannel::send_p50_cache_fd_reply(\n    P50CacheFdReplyTicket &&ticket")
    ordered(take,
            "!p50_fd_reply_arm_consumed",
            "p50_fd_request_ready",
            "p50_fd_socket_idle(fd)",
            "p50_channel_generation, p50_decoded_frame_sequence, framesQueued()",
            "p50_fd_request_ready = false;",
            "p50_last_fd_request = {};")

    send = section(
        comm_source,
        "bool MsgChannel::send_p50_cache_fd_reply(\n    P50CacheFdReplyTicket &&ticket",
        "bool MsgChannel::send_p50_cache_fd_reply(\n    const P50CacheSessionFdRequestMsg &request")
    for token in (
            "ticket.channel_generation_ == p50_channel_generation",
            "ticket.decoded_frame_sequence_ == p50_decoded_frame_sequence",
            "ticket.outbound_frame_sequence_ == framesQueued()",
            "ticket.outbound_frame_sequence_ == framesFlushed()",
            "protocol_supports_p50_r1_bridge(protocol) && !eof &&\n        instate == NEED_LEN",
            "pending_frame_ends.empty()",
            "ticket.invalidate();",
            "p50_fd_socket_idle(fd)"):
        require(token in send,
                f"deferred descriptor ticket consumption omits {token}")

    resume = section(
        main_source,
        "void Daemon::resume_deferred_p50_cache_fd_requests() noexcept",
        "void Daemon::shutdown_cache_adapter() noexcept")
    ordered(resume,
            "if (!service_ready && cache_sidecar_recovery_in_progress())",
            "std::vector<MsgChannel *> deferred;",
            "deferred.push_back(entry.first);",
            "const auto found = clients.find(channel);",
            "std::move(*client->deferred_p50_cache_fd_request)",
            "client->deferred_p50_cache_fd_request.reset();",
            "handle_p50_cache_session_fd_request(")
    require(main_source.count("resume_deferred_p50_cache_fd_requests();") == 1,
            "sidecar poll does not resume deferred descriptor requests exactly once")

    handler = section(
        main_source,
        "bool Daemon::handle_p50_cache_session_fd_request(",
        "bool Daemon::handle_cache_session(")
    ordered(handler,
            "take_p50_cache_fd_reply_ticket(*msg)",
            "scheduler_owns_getcs_assignment(client)",
            "assignment_rebind_eligible && cache_sidecar_recovery_in_progress()",
            "client->deferred_p50_cache_fd_request.emplace(",
            "if (cache_adapter == nullptr || !cache_adapter->authenticated())",
            "if (!assignment_rebind_eligible)",
            "handoff.readyLease = *ready_lease;",
            "handoff.routeStateGeneration = cache_route_state_generation;",
            "std::move(reply_ticket), control_identity, transfer_fd, deadline")
    require("handoff.cachePort =" not in handler and
            "handoff.assignmentEpoch =" not in handler and
            "handoff.assignmentNonce =" not in handler,
            "C-side replacement mutates immutable F endpoint or assignment identity")


def check_compiler_quiescence(source: str, helper: str, test_source: str,
                              workit: str, workit_header: str,
                              makefile: str, daemon_makefile: str) -> None:
    flow = section(source, "/* The exact quiescence barrier",
                   "void Daemon::handle_old_request")
    require('#include "compiler_group_signal.h"' in source,
            "daemon does not use the shared compiler signal-authority primitive")
    for token in ("send_term_if_owned(", "observe_owned_anchor(",
                  "send_final_if_owned(", "settle_retired(",
                  '"session quiescence TERM compiler pid="',
                  '"session quiescence KILL compiler pid="',
                  '"session quiescence settled compiler pid="'):
        require(token in flow, f"quiescence omits shared primitive {token}")
    require("kill(-rec.pgid" not in flow and "kill(rec.pid" not in flow,
            "quiescence bypasses exact child-anchor signal authority")
    reaper = section(source, "if (!child_registry.empty()) {",
                     "/* Push queued state records")
    require("const bool lifecycle_complete =\n"
            "                iterator->second.completion_observed;" in reaper,
            "generic reaper does not preserve monotonic completion across signals")
    require(reaper.count("advance_exited_group_cleanup(") == 2,
            "generic unfinished/completed cleanup does not share the two-turn helper")
    for token in ("orphaned compiler cleanup KILL pid=",
                  "completed compiler cleanup KILL pid=",
                  "completed compiler cleanup settled pid=",
                  "release_slot_once(rec.slot)"):
        require(token in source, f"compiler lifecycle omits {token}")
    compiler_pipe_events = (
        "pollfd_is_set(pollfds, client->pipe_from_child,\n"
        "                                         POLLIN | POLLHUP | POLLERR)")
    require(compiler_pipe_events in source,
            "compiler completion ignores EOF/HUP and can strand descendants")

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
    require("release_slot_once(SlotAccounting& accounting)" in helper,
            "compiler slot accounting is not a shared once-only primitive")
    require("compiler_wait_status_is_worker_process_loss(" in helper and
            "if (!WIFSIGNALED(wait_status))" in helper and
            "case SIGTERM:" in helper and "case SIGINT:" in helper and
            "case SIGALRM:" in helper and
            "return WTERMSIG(wait_status) == daemon_shutdown_signal;" in helper,
            "worker-loss classifier does not require an exact caught shutdown signal")
    require("extern volatile sig_atomic_t workit_daemon_shutdown_signal" in
            workit_header and
            "workit_daemon_shutdown_signal = whichsig;" in source,
            "exact daemon termination signal is not carried into its compile worker")
    workit_loss = section(
        workit,
        "compiler_wait_status_is_worker_process_loss(",
        "if (shell_exit_status(status) != 0)")
    ordered(
        workit_loss,
        "status, workit_daemon_shutdown_signal",
        "rmsg.status = EXIT_GONE;",
        "job_stat[JobStatistics::exit_code] = EXIT_GONE;",
        "return EXIT_GONE;")
    cleanup_helper = section(
        helper,
        "CleanupAdvance advance_exited_group_cleanup(",
        "\n}\n\n}  // namespace icecc::daemon_child")
    ordered(cleanup_helper,
            "if (authority.active)",
            "observe_owned_anchor(authority, leader, operations)",
            "AnchorObservation::ExitedWaitable",
            "send_final_if_owned(",
            "if (!authority.active)",
            "settle_retired(")
    require(test_source.count(
                "test_completed_cleanup_settles_on_a_later_event_loop_turn") == 2 and
            test_source.count("advance_exited_group_cleanup(") >= 3 and
            "!second.final_signal.invoked && second.settled" in test_source and
            "later cleanup turn released the completed slot twice" in test_source,
            "compiled regression does not exercise later-turn production cleanup")
    require(test_source.count(
                "test_worker_process_loss_requires_both_shutdown_and_signal") == 2 and
            test_source.count(
                "compiler_wait_status_is_worker_process_loss(") >= 9 and
            "coincident compiler crash became retryable process loss" in
                test_source and
            "unaccompanied compiler KILL became retryable process loss" in
                test_source and
            "genuine numeric compiler exit 105 became retryable process loss" in
                test_source,
            "compiled regression does not distinguish exact shutdown loss from crashes or rc=105")
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
    require("compiler_group_signal.h" in daemon_makefile,
            "compiler signal-authority header is absent from daemon distribution")


def check_cache_service(source: str) -> None:
    trace_writer = section(source, "void append_test_trace(",
                           "void append_ready_test_trace(")
    for token in ("::getenv(environment)", "O_APPEND", "O_CLOEXEC", "::write(fd"):
        require(token in trace_writer, f"sidecar trace writer omits {token}")
    ready_trace = section(source, "void append_ready_test_trace(",
                          "void append_fingerprint_test_trace(")
    require('append_test_trace("ICECC_P50_TEST_READY_TRACE", message);' in ready_trace,
            "sidecar READY evidence must use its dedicated trace path")
    require("ICECC_P50_C1F1_REQUIRED" not in ready_trace + trace_writer,
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
                  'icecream-p50-source-result-v3',
                  '"source_mutex_wait_ns\\\":%llu',
                  '"source_mutex_service_ns\\\":%llu',
                  '"terminal_error_code\\\":%u',
                  '"terminal_error_name\\\":%s',
                  "ErrorCode::WIRE_REVISION_MISMATCH"):
        require(token in source_trace,
                f"C-side source-result trace omits {token}")
    transfer = section(source, "SidecarRuntime::transfer_source_on_owner(",
                       "bool SidecarRuntime::bind_route_endpoint_identity(")
    for token in (
        "acquire_source_address(endpoint_key, transfer_deadline)",
        "owner_preflight_source_endpoint(endpoint_key,",
        "source_fd_size(",
        "acquire_source_credit(reserved_raw_bytes, transfer_deadline)",
        "acquire_source_incarnations(",
        "read_source_fd(",
        "reserved_raw_bytes, transfer_deadline, stop_requested_",
        "append_source_result_trace(",
        "completion->set_value(value)",
    ):
        require(token in transfer,
                f"bounded independent source admission omits {token}")
    ordered(transfer,
            "acquire_source_address(endpoint_key, transfer_deadline)",
            "owner_preflight_source_endpoint(endpoint_key,",
            "source_fd_size(",
            "const int first_fd = open_armed(",
            "const auto source_bytes = read_source_fd(",
            "append_source_result_trace(",
            "completion->set_value(value)")
    require("source_transfer_mutex_" not in source and
            "source_transfer_lock" not in transfer,
            "source transfer still has a process-wide blocking gate")
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
    require('temp_root=${ICEFARM_TMPDIR:-${TMPDIR:-/tmp}}' in source and
            'work=$(mktemp -d "$temp_root/p5c.XXXXXX")' in source,
            "completion-flow work is not redirectable away from root /tmp")
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

    strict_retry_launch = section(
        source,
        'strict_retry_marker="$work/fresh-strict-retry.barrier"',
        "strict_retry_wrapper_pid=$!")
    require("ICECC_P50_C1F1_REQUIRED=1" in strict_retry_launch and
            "ICECC_P50_TEST_FRESH_STRICT_RETRY=1" in strict_retry_launch,
            "fresh strict retry wrapper does not exercise the strict policy")
    bounded_retry_launch = section(
        source,
        'bounded_marker="$work/bounded-strict-retry.barrier"',
        "bounded_wrapper_pid=$!")
    require("ICECC_P50_C1F1_REQUIRED=1" in bounded_retry_launch and
            "ICECC_P50_TEST_FRESH_STRICT_RETRY_SECOND_FAILURE=1" in
                bounded_retry_launch,
            "twice-failed strict wrapper does not exercise the bounded policy")
    require('test "${bounded_new_count:-0}" -eq 2' in source,
            "twice-failed strict wrapper does not reject a third assignment")
    scheduler_loss_worker = section(
        source,
        "scheduler_loss_ready_before=",
        'scheduler_loss_log_offset=$(stat -c %s "$work/f-loss.log")')
    require("ICECC_P50_C1F1_REQUIRED" not in scheduler_loss_worker and
            'ICECC_TEST_SOCKET="$work/worker-loss.sock"' in
                scheduler_loss_worker and
            'port_worker_loss' in scheduler_loss_worker and
            "normal scheduler-loss worker did not advertise READY" in
                scheduler_loss_worker,
            "scheduler-loss regression does not replace strict F with a normal cache-capable worker")
    inflight_launch = section(
        source,
        'inflight_identity="$work/inflight-worker-loss-compile-identity.jsonl"',
        "inflight_wrapper_pid=$!")
    require("ICECC_P50_C1F1_REQUIRED=1" in inflight_launch and
            'timeout "$timeout_s" "$work/bin/icecc"' in inflight_launch and
            "ICECC_P50_TEST_STRICT_RETRY_BEFORE_GETCS_BARRIER" in
                inflight_launch and
            "ICECC_P50_TEST_DISPOSITION" not in inflight_launch,
            "in-flight worker-loss cell does not exercise real strict policy before its test barrier")

    injected_fault_launch = section(
        source,
        "    malformed|disconnect)",
        "    *)")
    require('timeout "$timeout_s" "$work/bin/icecc"' in injected_fault_launch,
            "malformed/disconnect cells do not use the isolated check-only wrapper")
    require('timeout "$timeout_s" "$build/client/icecc"' not in injected_fault_launch,
            "malformed/disconnect fault injection still invokes the production wrapper")

    delayed_result = section(
        source,
        "delayed_wait_seconds=35",
        "run_remote_cell definitive definitive")
    for token in (
            'timeout "$timeout_s" "$build/client/icecc"',
            'grep -F \'final arguments:\'',
            'kill -STOP "-$delayed_compile_pgid"',
            'sleep "$delayed_wait_seconds"',
            'kill -0 "$delayed_wrapper_pid"',
            'kill -CONT "-$delayed_compile_pgid"',
            'production delayed-result object differs from exact local reference',
            'P50 compiler connection uses absolute 20-second deadline'):
        require(token in delayed_result,
                f"actual-client delayed-result regression omits {token}")

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
                  'strings "$build/client/icecc-p50-completion-test"',
                  'ICECC_P50_TEST_FRESH_STRICT_RETRY=1',
                  'ICECC_P50_TEST_FRESH_STRICT_RETRY_SECOND_FAILURE=1',
                  'rm -f -- "$strict_retry_remote_obj"',
                  'P50 assignment failed; requesting one fresh strict-P50 remote assignment; avoiding failed endpoint',
                  'strict retry reused the first assignment nonce',
                  'strict assignment lacks one exact P29V1 source witness',
                  'ICECC_P50_TEST_STRICT_RETRY_BEFORE_GETCS_BARRIER',
                  "inflight_retry_armed=1",
                  ': >"$inflight_retry_release"',
                  "inflight_compiler_started=1",
                  "grep -F 'final arguments:'",
                  'kill -TERM "$inflight_first_worker_pid"',
                  'worker shutdown interrupted remote compiler; closing result stream',
                  "len(job_ids) != 2",
                  "production in-flight strict retry differs from exact local reference",
                  'twice-failed strict retry exited $bounded_rc instead of 100',
                  'bounded strict retry minted ${bounded_new_count:-0} scheduler jobs',
                  'scheduler_loss_clears_before=',
                  'ICECC_REMOTE_REQUIRED=1 ICECC_VERSION="$envtar"',
                  'kill -STOP "$worker_pid"',
                  'kill -TERM "$sched_pid"',
                  'scheduler loss did not enter the preclosed legacy retry branch',
                  'P50 legacy retry predecessor proxy was already settled by local daemon; reconnecting',
                  'replacement scheduler exited during startup',
                  'scheduler-loss legacy retry differs from exact local reference',
                  'P29V1 source committed for P50 CompileFile',
                  'legacy wire identity bound for job',
                  'scheduler-loss retry crossed a local or strict-P50 path'):
        require(token in source, f"real terminal/reclaim matrix omits {token}")
    ordered(source,
            "run_remote_cell accepted accepted",
            "delayed_wait_seconds=35",
            'kill -STOP "-$delayed_compile_pgid"',
            'sleep "$delayed_wait_seconds"',
            'kill -CONT "-$delayed_compile_pgid"',
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
            'strict_retry_marker="$work/fresh-strict-retry.barrier"',
            'kill -TERM "$strict_first_worker_pid"',
            ': >"$strict_retry_release"',
            'inflight_identity="$work/inflight-worker-loss-compile-identity.jsonl"',
            "inflight_compiler_started=1",
            'kill -TERM "$inflight_first_worker_pid"',
            'worker shutdown interrupted remote compiler; closing result stream',
            "len(job_ids) != 2",
            'bounded_marker="$work/bounded-strict-retry.barrier"',
            'ICECC_P50_TEST_FRESH_STRICT_RETRY_SECOND_FAILURE=1',
            ': >"$bounded_release"',
            'scheduler_loss_log_offset=',
            'kill -STOP "$worker_pid"',
            'kill -TERM "$sched_pid"',
            'scheduler_loss_cleared=1',
            'kill -CONT "$worker_pid"',
            'scheduler_loss_preclosed=1',
            '"$build/scheduler/icecc-scheduler"',
            'wait "$scheduler_loss_wrapper_pid"',
            "PASS: real P50 terminal lifecycle plus worker/scheduler loss and bounded retries")


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
    check_wrapper_retry(files["client_main"], files["client"], files["client_h"])
    check_worker(files["serve"], files["record_h"] + files["record_cpp"])
    check_parent(files["main"])
    check_cache_fd_replacement(files["main"], files["comm_h"],
                               files["comm_cpp"])
    check_compiler_quiescence(files["main"], files["compiler_signal"],
                              files["compiler_test"], files["workit"],
                              files["workit_h"], files["unit_make"],
                              files["daemon_make"])
    check_record(files["record_h"], files["record_cpp"])
    check_cache_service(files["cache_service"])
    check_runtime_gate(files["runtime_gate"])


def deletion_mutants(files: dict[str, str]) -> None:
    mutations = (
        ("client_main", "!p50_retry_attempted || strict_p50",
         "!p50_retry_attempted"),
        ("client_main", "if (error.errorCode != 106)",
         "if (false)"),
        ("client_main", "if (p50_retry_attempted) {", "if (false) {"),
        ("client_main", "strict_p50 && !error.hasRetryAvoidEndpoint()",
         "false"),
        ("client_main", "local_daemon->get_msg(30, true)",
         "retry_predecessor_wait_deleted()"),
        ("client_main", "local_daemon->at_eof()",
         "retry_predecessor_eof_deleted()"),
        ("client_main", "p50_retry_avoid_host = error.retryAvoidHost;", ""),
        ("client", "getcs.cache_retry_avoid_port = retry_avoid_port;", ""),
        ("client", "error.errorCode, error.what(), failed_host, failed_port",
         "error.errorCode, error.what()"),
        ("client", "!usecs->hasCacheAdvertisement()", "false"),
        ("client", "(!barrier_exists || repeat_strict_failure)",
         "(!barrier_exists)"),
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
        ("client", "cserver->setTcpUserTimeoutUntil(", "timeout_extension_deleted("),
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
        ("main", "record->second.completion_observed = true;",
         "completion_observed_deleted();"),
        ("main",
         "const bool lifecycle_complete =\n"
         "                iterator->second.completion_observed;",
         "const bool lifecycle_complete = false;"),
        ("main", "icecc::daemon_child::advance_exited_group_cleanup(",
         "cleanup_advance_deleted("),
        ("main", "icecc::daemon_child::send_final_if_owned(",
         "final_signal_deleted("),
        ("main", '"session quiescence KILL compiler pid="',
         '"session quiescence KILL deleted pid="'),
        ("main", '"orphaned compiler cleanup KILL pid="',
         '"orphaned compiler cleanup deleted pid="'),
        ("main", "release_slot_once(rec.slot)", "release_slot_deleted(rec.slot)"),
        ("main", "workit_daemon_shutdown_signal = whichsig;", ""),
        ("main",
         "const bool assignment_rebind_eligible =\n"
         "        scheduler_owns_getcs_assignment(client) &&",
         "const bool assignment_rebind_eligible =\n        true &&"),
        ("main",
         "assignment_rebind_eligible && cache_sidecar_recovery_in_progress()",
         "assignment_rebind_eligible"),
        ("main", "handoff.readyLease = *ready_lease;", ""),
        ("main", "resume_deferred_p50_cache_fd_requests();", ""),
        ("comm_cpp",
         "const bool ready = ticket.valid() && transfer_fd >= 0 && transfer_fd != fd &&\n"
         "        ticket.channel_generation_ == p50_channel_generation &&",
         "const bool ready = ticket.valid() && transfer_fd >= 0 && transfer_fd != fd &&\n"
         "        true &&"),
        ("comm_cpp",
         "ticket.channel_generation_ == p50_channel_generation &&\n"
         "        ticket.decoded_frame_sequence_ == p50_decoded_frame_sequence",
         "ticket.channel_generation_ == p50_channel_generation &&\n"
         "        true"),
        ("comm_cpp", "ticket.outbound_frame_sequence_ == framesQueued()",
         "true"),
        ("client_main",
         'getenv("ICECC_P50_TEST_STRICT_RETRY_BEFORE_GETCS_BARRIER")',
         'getenv("ICECC_P50_TEST_STRICT_RETRY_BARRIER_DELETED")'),
        ("workit",
         "status, workit_daemon_shutdown_signal",
         "status, 0"),
        ("workit", "return EXIT_GONE;", "return 0;"),
        ("compiler_signal",
         "return WTERMSIG(wait_status) == daemon_shutdown_signal;",
         "return true;"),
        ("compiler_signal", "case SIGALRM:", "case SIGSEGV:"),
        ("main",
         "pollfd_is_set(pollfds, client->pipe_from_child,\n"
         "                                         POLLIN | POLLHUP | POLLERR)",
         "pollfd_is_set(pollfds, client->pipe_from_child, POLLIN)"),
        ("compiler_signal", "release_slot_once(SlotAccounting& accounting)",
         "release_slot_deleted(SlotAccounting& accounting)"),
        ("compiler_signal", "CleanupAdvance advance_exited_group_cleanup(",
         "CleanupAdvance cleanup_advance_deleted("),
        ("compiler_signal", "WEXITED | WNOHANG | WNOWAIT",
         "WEXITED | WNOHANG"),
        ("compiler_signal",
         "authority.final_sent = true;\n    authority.active = false;",
         "authority.final_sent = true;"),
        ("compiler_test",
         "test_completed_cleanup_settles_on_a_later_event_loop_turn();", ""),
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
         'work=$(mktemp -d "$temp_root/p5c.XXXXXX")',
         'work=$(mktemp -d /tmp/p5c.XXXXXX)'),
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
        ("runtime_gate", 'kill -STOP "-$delayed_compile_pgid"', "stop_deleted"),
        ("runtime_gate", 'sleep "$delayed_wait_seconds"', "sleep_deleted"),
        ("runtime_gate", 'kill -CONT "-$delayed_compile_pgid"', "continue_deleted"),
        ("runtime_gate", 'rm -f -- "$retry_remote_obj"', ""),
        ("runtime_gate", 'ICECC_P50_TEST_FRESH_STRICT_RETRY=1', ""),
        ("runtime_gate",
         'ICECC_P50_TEST_FRESH_STRICT_RETRY_SECOND_FAILURE=1', ""),
        ("runtime_gate", 'rm -f -- "$strict_retry_remote_obj"', ""),
        ("runtime_gate",
         "ICECC_P50_TEST_STRICT_RETRY_BEFORE_GETCS_BARRIER",
         "ICECC_P50_TEST_STRICT_RETRY_BARRIER_DELETED"),
        ("runtime_gate", "inflight_compiler_started=1", "inflight_compiler_started=0"),
        ("runtime_gate", 'kill -TERM "$inflight_first_worker_pid"', "kill_deleted"),
        ("runtime_gate",
         "worker shutdown interrupted remote compiler; closing result stream",
         "worker shutdown classification deleted"),
        ("runtime_gate", "len(job_ids) != 2", "len(job_ids) < 2"),
        ("runtime_gate",
         'test "${bounded_new_count:-0}" -eq 2',
         'test "${bounded_new_count:-0}" -ge 2'),
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
        "client_h": (ROOT / "client/client.h").read_text(),
        "client_main": (ROOT / "client/main.cpp").read_text(),
        "client_make": (ROOT / "client/Makefile.am").read_text(),
        "serve": (ROOT / "daemon/serve.cpp").read_text(),
        "main": (ROOT / "daemon/main.cpp").read_text(),
        "comm_h": (ROOT / "services/comm.h").read_text(),
        "comm_cpp": (ROOT / "services/comm.cpp").read_text(),
        "compiler_signal": (ROOT / "daemon/compiler_group_signal.h").read_text(),
        "compiler_test": (ROOT / "unittests/p50_compiler_quiescence_test.cpp").read_text(),
        "workit": (ROOT / "daemon/workit.cpp").read_text(),
        "workit_h": (ROOT / "daemon/workit.h").read_text(),
        "unit_make": (ROOT / "unittests/Makefile.am").read_text(),
        "daemon_make": (ROOT / "daemon/Makefile.am").read_text(),
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
