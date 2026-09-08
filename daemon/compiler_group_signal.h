#pragma once

#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>

namespace icecc::daemon_child {

enum class AnchorObservation {
    Running,
    ExitedWaitable,
    NoChild,
    Unprovable,
};

enum class ConsumeObservation {
    Pending,
    Consumed,
    NoChild,
    Unprovable,
};

enum class GroupObservation {
    Present,
    Absent,
    Unprovable,
};

struct SignalResult {
    bool invoked = false;
    int error = 0;
};

struct SignalAuthority {
    bool active = true;
    bool term_sent = false;
    bool final_sent = false;
    bool leader_consumed = false;
    bool group_absent = false;
};

struct SlotAccounting {
    bool active = true;
};

/* A compiler signal is retryable process loss only when it exactly matches a
   termination signal caught by the owning daemon.  WIFSIGNALED alone also
   covers genuine compiler crashes; a merely concurrent daemon shutdown must
   not turn SIGSEGV/SIGABRT into retryable process loss.  Numeric statuses
   (including 105) remain genuine compiler exits. */
inline bool compiler_wait_status_is_worker_process_loss(
    int wait_status, int daemon_shutdown_signal) noexcept
{
    if (!WIFSIGNALED(wait_status))
        return false;
    switch (daemon_shutdown_signal) {
    case SIGTERM:
    case SIGINT:
    case SIGALRM:
        return WTERMSIG(wait_status) == daemon_shutdown_signal;
    default:
        return false;
    }
}

struct CleanupAdvance {
    AnchorObservation anchor = AnchorObservation::Unprovable;
    SignalResult final_signal;
    bool settled = false;
};

inline bool release_slot_once(SlotAccounting& accounting) noexcept
{
    if (!accounting.active)
        return false;
    accounting.active = false;
    return true;
}

class PosixSignalOperations {
public:
    AnchorObservation observe_anchor(pid_t leader) const noexcept
    {
        siginfo_t information{};
        int result;
        do {
            errno = 0;
            result = ::waitid(P_PID, static_cast<id_t>(leader), &information,
                              WEXITED | WNOHANG | WNOWAIT);
        } while (result < 0 && errno == EINTR);
        if (result == 0 && information.si_pid == 0)
            return AnchorObservation::Running;
        if (result == 0 && information.si_pid == leader)
            return AnchorObservation::ExitedWaitable;
        if (result < 0 && errno == ECHILD)
            return AnchorObservation::NoChild;
        return AnchorObservation::Unprovable;
    }

    ConsumeObservation consume_anchor(pid_t leader) const noexcept
    {
        int status = 0;
        pid_t result;
        do {
            result = ::waitpid(leader, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == leader)
            return ConsumeObservation::Consumed;
        if (result == 0)
            return ConsumeObservation::Pending;
        if (result < 0 && errno == ECHILD)
            return ConsumeObservation::NoChild;
        return ConsumeObservation::Unprovable;
    }

    GroupObservation observe_group(pid_t pgid) const noexcept
    {
        if (::kill(-pgid, 0) == 0 || errno == EPERM)
            return GroupObservation::Present;
        if (errno == ESRCH)
            return GroupObservation::Absent;
        return GroupObservation::Unprovable;
    }

    SignalResult signal_group(pid_t pgid, int signal_number) const noexcept
    {
        errno = 0;
        const int result = ::kill(-pgid, signal_number);
        return SignalResult{true, result == 0 ? 0 : errno};
    }
};

template <typename Operations>
AnchorObservation observe_owned_anchor(SignalAuthority& authority,
                                        pid_t leader,
                                        Operations& operations) noexcept
{
    if (!authority.active)
        return AnchorObservation::Unprovable;
    if (leader <= 0) {
        authority.active = false;
        return AnchorObservation::Unprovable;
    }
    const AnchorObservation observed = operations.observe_anchor(leader);
    if (observed == AnchorObservation::NoChild) {
        authority.active = false;
        authority.leader_consumed = true;
    } else if (observed == AnchorObservation::Unprovable) {
        authority.active = false;
    }
    return observed;
}

template <typename Operations>
SignalResult send_term_if_owned(SignalAuthority& authority,
                                pid_t leader,
                                pid_t pgid,
                                Operations& operations) noexcept
{
    if (!authority.active || authority.term_sent || authority.final_sent
            || authority.group_absent)
        return {};
    // A retained leader anchors its process-group number only when it is the
    // group leader.  Any malformed registry identity fails closed before a
    // negative/zero kill target can address an unrelated process set.
    if (leader <= 0 || pgid <= 0 || leader != pgid) {
        authority.active = false;
        return {};
    }
    const AnchorObservation observed =
        observe_owned_anchor(authority, leader, operations);
    if (observed != AnchorObservation::Running
            && observed != AnchorObservation::ExitedWaitable)
        return {};
    const SignalResult result = operations.signal_group(pgid, SIGTERM);
    authority.term_sent = true;
    if (result.error == ESRCH) {
        authority.group_absent = true;
        authority.active = false;
    }
    return result;
}

template <typename Operations>
SignalResult send_final_if_owned(SignalAuthority& authority,
                                 pid_t leader,
                                 pid_t pgid,
                                 Operations& operations) noexcept
{
    if (!authority.active || authority.final_sent || authority.group_absent)
        return {};
    if (leader <= 0 || pgid <= 0 || leader != pgid) {
        authority.active = false;
        return {};
    }
    const AnchorObservation observed =
        observe_owned_anchor(authority, leader, operations);
    if (observed != AnchorObservation::Running
            && observed != AnchorObservation::ExitedWaitable)
        return {};
    const SignalResult result = operations.signal_group(pgid, SIGKILL);
    authority.final_sent = true;
    authority.active = false;
    if (result.error == ESRCH)
        authority.group_absent = true;
    return result;
}

template <typename Operations>
bool settle_retired(SignalAuthority& authority,
                    pid_t leader,
                    pid_t pgid,
                    Operations& operations) noexcept
{
    if (authority.active)
        return false;
    if (leader <= 0 || pgid <= 0 || leader != pgid)
        return false;
    if (!authority.leader_consumed) {
        const ConsumeObservation observed = operations.consume_anchor(leader);
        if (observed == ConsumeObservation::Consumed
                || observed == ConsumeObservation::NoChild)
            authority.leader_consumed = true;
        else
            return false;
    }
    if (!authority.group_absent) {
        const GroupObservation observed = operations.observe_group(pgid);
        if (observed == GroupObservation::Absent)
            authority.group_absent = true;
        else
            return false;
    }
    return true;
}

/* One ordinary event-loop turn for a compiler group whose direct leader may
   have exited.  Completion/slot accounting deliberately lives outside this
   helper: a final signal retires only OS signal authority, never the logical
   completion fact.  Later turns continue settle_retired() without restoring
   authority or emitting another signal. */
template <typename Operations>
CleanupAdvance advance_exited_group_cleanup(SignalAuthority& authority,
                                             pid_t leader,
                                             pid_t pgid,
                                             Operations& operations) noexcept
{
    CleanupAdvance result;
    if (authority.active) {
        result.anchor = observe_owned_anchor(authority, leader, operations);
        if (result.anchor == AnchorObservation::ExitedWaitable) {
            result.final_signal = send_final_if_owned(
                authority, leader, pgid, operations);
        }
    }
    if (!authority.active) {
        result.settled = settle_retired(
            authority, leader, pgid, operations);
    }
    return result;
}

}  // namespace icecc::daemon_child
