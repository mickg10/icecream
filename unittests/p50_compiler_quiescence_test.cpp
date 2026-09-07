#include "daemon/compiler_group_signal.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
#  include <sys/prctl.h>
#endif

#include <unistd.h>

namespace child = icecc::daemon_child;

namespace {

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string_view message)
{
    throw TestFailure(std::string(message));
}

void require(bool condition, std::string_view message)
{
    if (!condition)
        fail(message);
}

struct FakeOperations {
    std::deque<child::AnchorObservation> anchors;
    std::deque<child::ConsumeObservation> consumes;
    std::deque<child::GroupObservation> groups;
    std::deque<int> signal_errors;
    std::vector<int> signals;

    child::AnchorObservation observe_anchor(pid_t)
    {
        require(!anchors.empty(), "unexpected anchor observation");
        const child::AnchorObservation result = anchors.front();
        anchors.pop_front();
        return result;
    }

    child::ConsumeObservation consume_anchor(pid_t)
    {
        require(!consumes.empty(), "unexpected leader consumption");
        const child::ConsumeObservation result = consumes.front();
        consumes.pop_front();
        return result;
    }

    child::GroupObservation observe_group(pid_t)
    {
        require(!groups.empty(), "unexpected group observation");
        const child::GroupObservation result = groups.front();
        groups.pop_front();
        return result;
    }

    child::SignalResult signal_group(pid_t, int signal_number)
    {
        signals.push_back(signal_number);
        const int error = signal_errors.empty() ? 0 : signal_errors.front();
        if (!signal_errors.empty())
            signal_errors.pop_front();
        return {true, error};
    }
};

void test_retained_anchor_authorizes_exactly_term_then_final()
{
    FakeOperations operations;
    operations.anchors = {
        child::AnchorObservation::Running,
        child::AnchorObservation::ExitedWaitable,
        child::AnchorObservation::ExitedWaitable,
    };
    operations.consumes = {child::ConsumeObservation::Consumed};
    operations.groups = {
        child::GroupObservation::Present,
        child::GroupObservation::Absent,
    };
    child::SignalAuthority authority;

    require(child::send_term_if_owned(authority, 41, 41, operations).invoked,
            "owned running leader did not authorize TERM");
    require(child::observe_owned_anchor(authority, 41, operations)
                == child::AnchorObservation::ExitedWaitable,
            "exited leader was not retained waitable through TERM grace");
    require(child::send_final_if_owned(authority, 41, 41, operations).invoked,
            "retained waitable leader did not authorize final group KILL");
    require(!authority.active && authority.final_sent,
            "final signal did not retire authority monotonically");
    require(!child::settle_retired(authority, 41, 41, operations),
            "live residue was incorrectly declared settled");
    require(child::settle_retired(authority, 41, 41, operations),
            "absent group did not settle after exact leader reap");

    require(!child::send_term_if_owned(authority, 41, 41, operations).invoked,
            "retired authority re-sent TERM");
    require(!child::send_final_if_owned(authority, 41, 41, operations).invoked,
            "retired authority re-sent final KILL");
    require(operations.signals == std::vector<int>({SIGTERM, SIGKILL}),
            "signal schedule was not exactly TERM then KILL");
}

void test_lost_anchor_never_authorizes_a_numeric_signal()
{
    FakeOperations operations;
    operations.anchors = {child::AnchorObservation::NoChild};
    operations.groups = {child::GroupObservation::Absent};
    child::SignalAuthority authority;

    require(!child::send_term_if_owned(authority, 42, 42, operations).invoked,
            "lost leader authorized TERM");
    require(!authority.active && authority.leader_consumed,
            "ECHILD did not retire authority and record consumed status");
    require(!child::send_final_if_owned(authority, 42, 42, operations).invoked,
            "lost leader authorized final KILL");
    require(child::settle_retired(authority, 42, 42, operations),
            "lost leader plus absent group did not settle");
    require(operations.signals.empty(), "lost anchor emitted a signal");
}

void test_malformed_group_identity_fails_before_syscalls()
{
    for (const std::pair<pid_t, pid_t>& identity : {
             std::pair<pid_t, pid_t>{0, 0},
             std::pair<pid_t, pid_t>{45, 0},
             std::pair<pid_t, pid_t>{45, 46},
         }) {
        FakeOperations operations;
        child::SignalAuthority authority;
        require(!child::send_term_if_owned(
                    authority, identity.first, identity.second, operations).invoked,
                "malformed process-group identity authorized TERM");
        require(!authority.active,
                "malformed process-group identity retained signal authority");
        require(operations.signals.empty() && operations.anchors.empty(),
                "malformed identity reached a signal or wait syscall");
        require(!child::settle_retired(
                    authority, identity.first, identity.second, operations),
                "malformed process-group identity settled open");
    }
}

void test_completed_and_active_slots_release_exactly_once()
{
    child::SlotAccounting completed;
    child::SlotAccounting active;
    unsigned current_slots = 2;

    require(child::release_slot_once(completed),
            "normal completion did not release its slot");
    --current_slots;
    require(!child::release_slot_once(completed),
            "completed-but-unreaped cleanup released its slot twice");
    require(child::release_slot_once(active),
            "scheduler-loss cleanup did not release the active slot");
    --current_slots;
    require(!child::release_slot_once(active),
            "repeated scheduler-loss cleanup released the active slot twice");
    require(current_slots == 0,
            "completed A plus active B left incorrect capacity accounting");
}

void test_group_disappearance_at_term_is_terminal()
{
    FakeOperations operations;
    operations.anchors = {child::AnchorObservation::Running};
    operations.signal_errors = {ESRCH};
    operations.consumes = {child::ConsumeObservation::Consumed};
    child::SignalAuthority authority;

    require(child::send_term_if_owned(authority, 43, 43, operations).invoked,
            "TERM attempt was not recorded");
    require(!authority.active && authority.group_absent,
            "ESRCH did not irreversibly retire the group identity");
    require(!child::send_final_if_owned(authority, 43, 43, operations).invoked,
            "disappeared group was signalled by number later");
    require(child::settle_retired(authority, 43, 43, operations),
            "disappeared group did not settle after leader reap");
    require(operations.signals == std::vector<int>({SIGTERM}),
            "disappeared group received an extra signal");
}

void test_lost_after_term_fails_closed_without_reauthorization()
{
    FakeOperations operations;
    operations.anchors = {
        child::AnchorObservation::Running,
        child::AnchorObservation::Unprovable,
    };
    operations.consumes = {child::ConsumeObservation::NoChild};
    operations.groups = {
        child::GroupObservation::Present,
        child::GroupObservation::Absent,
    };
    child::SignalAuthority authority;

    require(child::send_term_if_owned(authority, 44, 44, operations).invoked,
            "owned leader did not receive TERM");
    require(!child::send_final_if_owned(authority, 44, 44, operations).invoked,
            "unprovable leader authorized final KILL");
    require(!authority.active, "unprovable ownership was reauthorized");
    require(!child::settle_retired(authority, 44, 44, operations),
            "present residue did not fail closed");
    require(child::settle_retired(authority, 44, 44, operations),
            "naturally absent residue did not later settle");
    require(operations.signals == std::vector<int>({SIGTERM}),
            "lost authority emitted a post-retirement signal");
}

#if defined(__linux__)
bool write_exact_child(int fd, const void* data, size_t size) noexcept
{
    const char* cursor = static_cast<const char*>(data);
    while (size != 0) {
        const ssize_t count = ::write(fd, cursor, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        cursor += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

bool read_exact_bounded(int fd, void* data, size_t size,
                        std::chrono::milliseconds timeout)
{
    char* cursor = static_cast<char*>(data);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (size != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        const int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count());
        pollfd descriptor{fd, POLLIN | POLLHUP | POLLERR, 0};
        int ready;
        do {
            ready = ::poll(&descriptor, 1, std::max(1, remaining));
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0)
            return false;
        const ssize_t count = ::read(fd, cursor, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        cursor += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

bool reap_exact_bounded(pid_t pid, int& status,
                        std::chrono::milliseconds timeout) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        pid_t result;
        do {
            result = ::waitpid(pid, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == pid)
            return true;
        if (result < 0 && errno != ECHILD)
            return false;
        ::usleep(1000);
    }
    return false;
}

struct OwnedRealGroup {
    pid_t leader = -1;
    pid_t descendant = -1;
    child::SignalAuthority authority;
    child::PosixSignalOperations operations;

    ~OwnedRealGroup()
    {
        if (leader <= 0)
            return;
        // This uses the same retained-child rule as production.  A failed test
        // never converts PID/PGID existence into permission to signal.
        if (authority.active)
            (void)child::send_final_if_owned(
                authority, leader, leader, operations);
        int status = 0;
        if (descendant > 0
                && reap_exact_bounded(descendant, status,
                                      std::chrono::seconds(5)))
            descendant = -1;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline
                && !child::settle_retired(
                    authority, leader, leader, operations))
            ::usleep(1000);
    }
};

void test_real_waitable_leader_anchors_descendant_kill()
{
    require(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0,
            "could not become a child subreaper");
    int ready[2] = {-1, -1};
    require(::pipe(ready) == 0, "could not create readiness pipe");

    OwnedRealGroup owned;
    owned.leader = ::fork();
    require(owned.leader >= 0, "could not fork group leader");
    if (owned.leader == 0) {
        (void)::close(ready[0]);
        if (::setpgid(0, 0) != 0)
            _exit(90);
        const pid_t descendant = ::fork();
        if (descendant < 0)
            _exit(91);
        if (descendant == 0) {
            (void)::signal(SIGTERM, SIG_IGN);
            const pid_t self = ::getpid();
            if (!write_exact_child(ready[1], &self, sizeof(self)))
                _exit(92);
            (void)::close(ready[1]);
            for (;;)
                ::pause();
        }
        (void)::close(ready[1]);
        for (;;)
            ::pause();
    }

    (void)::close(ready[1]);
    require(read_exact_bounded(
                ready[0], &owned.descendant, sizeof(owned.descendant),
                std::chrono::seconds(5)),
            "real-process readiness was not boundedly observed");
    (void)::close(ready[0]);
    require(owned.descendant > 0 && ::getpgid(owned.descendant) == owned.leader,
            "descendant did not enter the leader process group");

    require(child::send_term_if_owned(
                owned.authority, owned.leader, owned.leader,
                owned.operations).invoked,
            "real leader did not authorize TERM");

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    child::AnchorObservation observation = child::AnchorObservation::Running;
    while (std::chrono::steady_clock::now() < deadline) {
        observation = child::observe_owned_anchor(
            owned.authority, owned.leader, owned.operations);
        if (observation != child::AnchorObservation::Running)
            break;
        ::usleep(1000);
    }
    require(observation == child::AnchorObservation::ExitedWaitable,
            "TERM leader did not become a retained waitable anchor");
    require(::kill(owned.descendant, 0) == 0,
            "TERM-ignoring descendant did not survive the grace phase");
    require(child::send_final_if_owned(
                owned.authority, owned.leader, owned.leader,
                owned.operations).invoked,
            "waitable leader did not authorize real final group KILL");
    require(!owned.authority.active && owned.authority.final_sent,
            "real final signal did not retire authority");

    int descendant_status = 0;
    require(reap_exact_bounded(
                owned.descendant, descendant_status, std::chrono::seconds(5)),
            "could not boundedly reap killed descendant through subreaper");
    owned.descendant = -1;
    require(WIFSIGNALED(descendant_status) && WTERMSIG(descendant_status) == SIGKILL,
            "descendant was not killed by the final group signal");
    require(child::settle_retired(
                owned.authority, owned.leader, owned.leader,
                owned.operations),
            "real leader/group did not settle after exact reaps");
    require(owned.authority.leader_consumed && owned.authority.group_absent,
            "real settlement omitted leader consumption or group absence");
    require(!child::send_final_if_owned(
                owned.authority, owned.leader, owned.leader,
                owned.operations).invoked,
            "real retired identity authorized a later signal");
    owned.leader = -1;
}
#endif

}  // namespace

int main()
{
    try {
        test_retained_anchor_authorizes_exactly_term_then_final();
        test_lost_anchor_never_authorizes_a_numeric_signal();
        test_malformed_group_identity_fails_before_syscalls();
        test_completed_and_active_slots_release_exactly_once();
        test_group_disappearance_at_term_is_terminal();
        test_lost_after_term_fails_closed_without_reauthorization();
#if defined(__linux__)
        test_real_waitable_leader_anchors_descendant_kill();
#endif
    } catch (const std::exception& error) {
        std::cerr << "p50_compiler_quiescence_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "p50 compiler quiescence tests: PASS\n";
    return 0;
}
