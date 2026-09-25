#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace icecc::p50::diagnostics {

inline bool enabled_from_environment() noexcept
{
    const char *value = std::getenv("ICECC_P50_DIAGNOSTICS");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

inline uint64_t monotonic_milliseconds() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

template <typename Clock, typename Duration>
inline uint64_t elapsed_milliseconds(
    const std::chrono::time_point<Clock, Duration> &start,
    const std::chrono::time_point<Clock, Duration> &end) noexcept
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    return elapsed.count() < 0 ? 0 : static_cast<uint64_t>(elapsed.count());
}

enum class Outcome {
    Incomplete,
    Success,
    Failure,
    Exception,
};

enum class Stage {
    NotStarted,
    CompilerConnect,
    EnvironmentReady,
    LocalSlotWait,
    LocalCppPrepare,
    CLeaseToFd,
    FArmToArmed,
    ArmedToControlBegin,
    ControlWait,
    CompilerResultWait,
    Complete,
};

inline const char *stage_name(Stage stage) noexcept
{
    switch (stage) {
    case Stage::NotStarted: return "not_started";
    case Stage::CompilerConnect: return "compiler_connect";
    case Stage::EnvironmentReady: return "environment_ready";
    case Stage::LocalSlotWait: return "local_slot_wait";
    case Stage::LocalCppPrepare: return "local_cpp_prepare";
    case Stage::CLeaseToFd: return "c_lease_to_fd";
    case Stage::FArmToArmed: return "f_arm_to_armed";
    case Stage::ArmedToControlBegin: return "armed_to_control_begin";
    case Stage::ControlWait: return "control_wait";
    case Stage::CompilerResultWait: return "compiler_result_wait";
    case Stage::Complete: return "complete";
    }
    return "not_started";
}

struct RemoteAttemptRecord {
    bool enabled = false;
    uint32_t schema_version = 1;
    uint32_t job_id = 0;
    uint64_t assignment_nonce = 0;
    std::optional<uint32_t> profile_mask;
    Outcome outcome = Outcome::Incomplete;
    std::optional<uint32_t> error_code;
    uint64_t start_ms = 0;
    uint64_t end_ms = 0;
    Stage stage = Stage::NotStarted;

    std::optional<uint64_t> compiler_connect_ms;
    std::optional<uint64_t> environment_ready_ms;
    std::optional<uint64_t> local_slot_wait_ms;
    std::optional<uint64_t> local_cpp_prepare_ms;
    std::optional<uint64_t> lease_send_to_fd_ms;
    std::optional<uint64_t> arm_send_to_armed_ms;
    std::optional<uint64_t> lease_send_to_armed_ms;
    std::optional<uint64_t> armed_to_control_begin_ms;
    std::optional<uint64_t> control_wait_ms;
    std::optional<uint64_t> compiler_result_wait_ms;
};

inline void append_number(std::string &out, const std::optional<uint64_t> &value)
{
    if (value)
        out += std::to_string(*value);
    else
        out += "null";
}

inline std::string format_remote_attempt(const RemoteAttemptRecord &record)
{
    if (!record.enabled)
        return {};
    const char *outcome = "incomplete";
    switch (record.outcome) {
    case Outcome::Incomplete: outcome = "incomplete"; break;
    case Outcome::Success: outcome = "success"; break;
    case Outcome::Failure: outcome = "failure"; break;
    case Outcome::Exception: outcome = "exception"; break;
    }

    std::string out = "P50_REMOTE_PHASE {\"schema_version\":" +
        std::to_string(record.schema_version) + ",\"job_id\":" +
        std::to_string(record.job_id) + ",\"assignment_nonce\":" +
        std::to_string(record.assignment_nonce) + ",\"profile_mask\":";
    if (record.profile_mask)
        out += std::to_string(*record.profile_mask);
    else
        out += "null";
    out += ",\"stage\":\"";
    out += stage_name(record.stage);
    out += "\",\"outcome\":\"";
    out += outcome;
    out += "\",\"error_code\":";
    if (record.error_code)
        out += std::to_string(*record.error_code);
    else
        out += "null";
    out += ",\"start_ms\":" + std::to_string(record.start_ms) +
           ",\"end_ms\":" + std::to_string(record.end_ms) +
           ",\"compiler_connect_ms\":";
    append_number(out, record.compiler_connect_ms);
    out += ",\"environment_ready_ms\":";
    append_number(out, record.environment_ready_ms);
    out += ",\"local_slot_wait_ms\":";
    append_number(out, record.local_slot_wait_ms);
    out += ",\"local_cpp_prepare_ms\":";
    append_number(out, record.local_cpp_prepare_ms);
    out += ",\"lease_send_to_fd_ms\":";
    append_number(out, record.lease_send_to_fd_ms);
    out += ",\"arm_send_to_armed_ms\":";
    append_number(out, record.arm_send_to_armed_ms);
    out += ",\"lease_send_to_armed_ms\":";
    append_number(out, record.lease_send_to_armed_ms);
    out += ",\"armed_to_control_begin_ms\":";
    append_number(out, record.armed_to_control_begin_ms);
    out += ",\"control_wait_ms\":";
    append_number(out, record.control_wait_ms);
    out += ",\"compiler_result_wait_ms\":";
    append_number(out, record.compiler_result_wait_ms);
    out += "}";
    return out;
}

} // namespace icecc::p50::diagnostics
