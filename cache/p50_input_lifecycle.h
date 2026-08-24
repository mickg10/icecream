#pragma once

// Production ownership for one retained Protocol-50 compiler input.
//
// Cache identity remains exactly (C_STORE_GUID, TU_SEQ).  The fields below
// are daemon/job observations used to ensure that cancellation, replacement,
// and terminal job closure mutate only their exact logical owner.

#include "p50_input_record.h"
#include "p50_local_transport.h"

#include <compare>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace icecc::p50 {

struct InputLeaseOwner {
    uint64_t logical_job = 0;
    uint64_t assignment_epoch = 0;
    uint64_t assignment_nonce = 0;
    auto operator<=>(const InputLeaseOwner&) const = default;
};

[[nodiscard]] inline bool input_lease_owner_valid(
    InputLeaseOwner owner) noexcept {
    return owner.logical_job != 0 && owner.assignment_epoch != 0 &&
           owner.assignment_nonce != 0;
}

enum class InputLifecycleAction : uint16_t {
    None = 0,
    CancelAttempt = 1,
    CloseAcceptedJob = 2,
    CancelJob = 3,
};

[[nodiscard]] inline bool input_lifecycle_action_valid(
    InputLifecycleAction action) noexcept {
    return action == InputLifecycleAction::CancelAttempt ||
           action == InputLifecycleAction::CloseAcceptedJob ||
           action == InputLifecycleAction::CancelJob;
}

struct InputLifecycleRequest {
    local::Identity identity{};
    InputRecordKey key{};
    InputLeaseOwner owner{};
    uint64_t operation_id = 0;
    InputLifecycleAction action = InputLifecycleAction::None;
    auto operator<=>(const InputLifecycleRequest&) const = default;
};

enum class InputLifecycleApplyStatus : uint8_t {
    Applied = 0,
    AlreadyApplied,
    InvalidArgument,
    UnknownRecord,
    StaleOwner,
    ConflictingReplay,
    CapacityExceeded,
};

struct InputLifecycleApplyResult {
    InputLifecycleApplyStatus status = InputLifecycleApplyStatus::InvalidArgument;
    bool close_record = false;
    bool collect_record = false;
};

enum class InputLifecycleCommitDecision : uint8_t {
    Open = 0,
    Closed,
    CapacityExceeded,
};

// Serialized beside P50ServerEndpoint on its one owner executor.  This table
// does not own exact input bytes: it owns the bounded logical-job/attempt
// observation needed to decide whether the endpoint's InputRecordStore may be
// closed.  A committed record can exist before its CompileFile owner arrives;
// first attachment binds that record to one exact logical owner.
class InputLifecycleRegistry {
public:
    InputLifecycleRegistry(size_t max_owners, size_t max_replays);

    // Reserve lifecycle capacity before InputRecord publication.  This runs in
    // the endpoint's input_job_state selector, so capacity failure precedes
    // publication and cannot create an unowned retained record.  A terminal
    // pre-commit tombstone selects the endpoint's closed-commit path.
    [[nodiscard]] InputLifecycleCommitDecision prepare_route_commit(
        InputRecordKey key) noexcept;
    void abort_route_commit(InputRecordKey key) noexcept;

    // Completes the reservation after the endpoint has validated and committed
    // the exact route transaction.  retained is true only when an attachable
    // InputRecord was published; a closed commit is validated but not retained.
    [[nodiscard]] bool observe_route_commit(InputRecordKey key,
                                            bool retained) noexcept;

    // Reserve one compiler authorization.  A cancelled current attempt may be
    // replaced by another assignment for the same logical job without changing
    // cache identity.  A live or closed owner cannot be stolen.
    [[nodiscard]] bool begin_attachment(InputRecordKey key,
                                        InputLeaseOwner owner,
                                        uint64_t request_id);
    void finish_attachment(InputRecordKey key, InputLeaseOwner owner,
                           uint64_t request_id, bool authorized) noexcept;

    // Apply one exact replay-safe lifecycle command.  Callers perform the
    // indicated endpoint close/collection on the same executor, then call
    // finish_apply() with whether that endpoint mutation succeeded.
    [[nodiscard]] InputLifecycleApplyResult begin_apply(
        const InputLifecycleRequest& request) noexcept;
    void finish_apply(const InputLifecycleRequest& request,
                      bool endpoint_mutation_succeeded) noexcept;

    // Consulted synchronously by P50ServerEndpoint::input_job_state before an
    // exact late commit is published.  A close-before-commit tombstone forces
    // the endpoint's non-attachable closed-commit path.
    [[nodiscard]] bool job_closed(InputRecordKey key) const noexcept;

    void clear() noexcept;
    [[nodiscard]] size_t owner_count() const noexcept { return leases_.size(); }
    [[nodiscard]] size_t replay_count() const noexcept { return replays_.size(); }

private:
    struct Lease {
        std::optional<InputLeaseOwner> owner;
        std::vector<InputLeaseOwner> retired_owners;
        uint64_t attachment_request_id = 0;
        bool committed = false;
        bool route_commit_pending = false;
        bool route_commit_observed = false;
        bool attachment_pending = false;
        bool attachment_authorized = false;
        bool attempt_cancelled = false;
        bool job_closed = false;
        bool apply_pending = false;
        bool apply_created = false;
    };

    struct Replay {
        InputLifecycleRequest request{};
        InputLifecycleApplyStatus status = InputLifecycleApplyStatus::Applied;
        bool complete = false;
    };

    [[nodiscard]] static bool key_valid(InputRecordKey key) noexcept;
    [[nodiscard]] bool replay_conflicts(
        const InputLifecycleRequest& request,
        InputLifecycleApplyResult& result) const noexcept;
    [[nodiscard]] bool reserve_replay(const InputLifecycleRequest& request) noexcept;
    void complete_replay(const InputLifecycleRequest& request,
                         InputLifecycleApplyStatus status) noexcept;
    void forget_replay(uint64_t operation_id) noexcept;
    void collect_closed(InputRecordKey key) noexcept;
    void erase_lease(std::map<InputRecordKey, Lease>::iterator position) noexcept;

    size_t max_owners_ = 0;
    size_t max_replays_ = 0;
    std::map<InputRecordKey, Lease> leases_;
    std::map<uint64_t, Replay> replays_;
    std::deque<uint64_t> replay_order_;
    size_t retired_owner_count_ = 0;
};

enum class InputLifecycleStatus : uint8_t {
    Applied = 0,
    AlreadyApplied,
    StoreReplaced,
    InvalidArgument,
    PeerUnauthenticated,
    HandshakeFailed,
    MalformedResponse,
    StaleIdentity,
    UnknownRecord,
    StaleOwner,
    ConflictingReplay,
    CapacityExceeded,
    Rejected,
    Timeout,
    Disconnected,
};

const char* input_lifecycle_status_name(InputLifecycleStatus status) noexcept;

struct InputLifecycleResult {
    InputLifecycleStatus status = InputLifecycleStatus::InvalidArgument;
    InputLifecycleRequest request{};
};

class InputLifecycleClient {
public:
    // Executes one exact lifecycle operation and accepts only an authenticated
    // byte-exact echo.  The sidecar's bounded replay table makes an identical
    // retry safe after a lost response; changed operation payload is rejected.
    [[nodiscard]] static InputLifecycleResult apply(
        const std::string& socket_path, InputLifecycleRequest request,
        const local::CredentialExpectation& expected_peer,
        std::chrono::steady_clock::time_point deadline) noexcept;
};

}  // namespace icecc::p50
