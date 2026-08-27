#include "p50_endpoint_run_cancel.h"

#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace icecc::p50 {

EndpointRunRegistry::EndpointRunRegistry(size_t max_live_runs)
    : max_live_runs_(max_live_runs) {
    if (max_live_runs_ == 0)
        throw std::invalid_argument("endpoint run registry bound must be nonzero");
}

std::optional<EndpointRunHandle> EndpointRunRegistry::admit(
    EndpointRunIdentity identity, sidecar::AbsoluteMonotonicDeadline deadline,
    std::shared_ptr<EndpointSocketTarget> socket, uint64_t timer_identity) {
    if (!identity.valid() || !deadline.valid() || !socket)
        return std::nullopt;
    if (active_runs_.size() >= max_live_runs_ || active_runs_.contains(identity))
        return std::nullopt;
    if (next_observation_id_ == 0 ||
        next_observation_id_ == std::numeric_limits<uint64_t>::max())
        return std::nullopt;
    const uint64_t observation = next_observation_id_++;
    if (timer_identity == 0)
        timer_identity = observation;
    Snapshot row;
    row.identity = identity;
    row.deadline = deadline;
    row.socket = std::move(socket);
    row.phase = EndpointRunPhase::Admitted;
    row.timer_identity = timer_identity;
    row.observation_id = observation;
    const auto [position, inserted] = active_runs_.emplace(identity, std::move(row));
    if (!inserted)
        return std::nullopt;
    return EndpointRunHandle{position->second.identity, position->second.deadline,
                             position->second.observation_id};
}

EndpointCancelResult EndpointRunRegistry::request_cancel(
    const EndpointCancelPermit& permit) noexcept {
    if (!permit.valid())
        return EndpointCancelResult::Stale;
    const auto position = active_runs_.find(permit.identity);
    if (position == active_runs_.end())
        return EndpointCancelResult::Stale;
    Snapshot& row = position->second;
    // Keep each comparison explicit: this is the security boundary and makes
    // it impossible for a future identity field to be accidentally omitted
    // from permit validation when the DTO grows.
    if (row.identity.sidecar_launch != permit.identity.sidecar_launch ||
        row.identity.c_store_guid != permit.identity.c_store_guid ||
        row.identity.f_store_guid != permit.identity.f_store_guid ||
        row.identity.f_session_operation != permit.identity.f_session_operation ||
        row.identity.endpoint_generation != permit.identity.endpoint_generation ||
        row.identity.endpoint_session_serial != permit.identity.endpoint_session_serial ||
        row.identity.run_sequence != permit.identity.run_sequence ||
        row.identity.socket_ownership_generation !=
            permit.identity.socket_ownership_generation ||
        row.observation_id != permit.observation_id ||
        permit.reason == EndpointCancelReason::None)
        return EndpointCancelResult::Stale;
    if (row.terminal.terminal())
        return EndpointCancelResult::AlreadyTerminal;
    if (row.cancel_requested)
        return EndpointCancelResult::AlreadyRequested;
    row.cancel_requested = true;
    if (row.socket)
        row.socket->cancel();
    return EndpointCancelResult::CancelRequested;
}

size_t EndpointRunRegistry::cancel_all_for_incarnation(
    const SidecarLaunchIdentity& incarnation,
    EndpointCancelReason reason) noexcept {
    if (!incarnation.valid() || reason == EndpointCancelReason::None)
        return 0;
    std::vector<std::shared_ptr<EndpointSocketTarget>> targets;
    for (auto& [identity, row] : active_runs_) {
        if (identity.sidecar_launch != incarnation || row.terminal.terminal() ||
            row.cancel_requested)
            continue;
        row.cancel_requested = true;
        if (row.socket)
            targets.push_back(row.socket);
    }
    // Invoke outside the map walk so a target may synchronously arrange its
    // terminal result without invalidating the registry iterator.
    for (const auto& target : targets)
        target->cancel();
    return targets.size();
}

bool EndpointRunRegistry::set_phase(const EndpointRunIdentity& identity,
                                    EndpointRunPhase phase) noexcept {
    const auto position = active_runs_.find(identity);
    if (position == active_runs_.end() || position->second.terminal.terminal())
        return false;
    position->second.phase = phase;
    return true;
}

bool EndpointRunRegistry::mark_terminal(const EndpointRunIdentity& identity,
                                        EndpointTerminalResult result) noexcept {
    if (!result.terminal())
        return false;
    const auto position = active_runs_.find(identity);
    if (position == active_runs_.end() || position->second.terminal.terminal())
        return false;
    position->second.terminal = result;
    position->second.phase = EndpointRunPhase::Terminal;
    return true;
}

std::optional<EndpointTerminalResult>
EndpointRunRegistry::consume_terminal(const EndpointRunIdentity& identity) noexcept {
    const auto position = active_runs_.find(identity);
    if (position == active_runs_.end() || !position->second.terminal.terminal())
        return std::nullopt;
    const EndpointTerminalResult result = position->second.terminal;
    active_runs_.erase(position);
    return result;
}

std::optional<EndpointRunRegistry::Snapshot>
EndpointRunRegistry::inspect(const EndpointRunIdentity& identity) const {
    const auto position = active_runs_.find(identity);
    if (position == active_runs_.end())
        return std::nullopt;
    return position->second;
}

size_t EndpointRunRegistry::timer_count() const noexcept {
    size_t count = 0;
    for (const auto& [identity, row] : active_runs_) {
        (void)identity;
        if (row.timer_identity != 0)
            ++count;
    }
    return count;
}

size_t EndpointRunRegistry::target_count() const noexcept {
    size_t count = 0;
    for (const auto& [identity, row] : active_runs_) {
        (void)identity;
        if (row.socket)
            ++count;
    }
    return count;
}

size_t EndpointRunRegistry::cancelled_live_count() const noexcept {
    size_t count = 0;
    for (const auto& [identity, row] : active_runs_) {
        (void)identity;
        if (row.cancel_requested && !row.terminal.terminal())
            ++count;
    }
    return count;
}

} // namespace icecc::p50
