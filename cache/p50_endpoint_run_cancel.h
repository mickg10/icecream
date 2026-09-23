#pragma once

// Typed, operation-scoped cancellation for Protocol-50 endpoint runs.
//
// This is a deliberately small bridge for the endpoint handoff lineage.  The
// daemon-side producers must populate SidecarLaunchIdentity from the
// structured READY/launch record and pass the actual C/F role-store GUIDs and
// FSessionOperationId.  No endpoint can manufacture authority from a socket
// descriptor, descriptor number, or object address.

#include "p50_sidecar_identity.h"
#include "services/p50_cache_session_wire.h"
#include "protocol50.h"

#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <cstddef>
#include <utility>

namespace icecc::p50 {

// Production wiring uses the complete structured READY incarnation.  The
// alias keeps the bridge named after the ruling while retaining the canonical
// sidecar DTO (including store-rooted C/F identities).
using SidecarLaunchIdentity = sidecar::LaunchIncarnation;

struct EndpointRunIdentity {
    SidecarLaunchIdentity sidecar_launch{};
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    daemon::P50FSessionOperationId f_session_operation{};
    uint64_t endpoint_generation = 0;
    uint64_t endpoint_session_serial = 0;
    uint64_t run_sequence = 0;
    uint64_t socket_ownership_generation = 0;

    [[nodiscard]] bool valid() const noexcept {
        // The bridge validates every scalar and every typed identity.  A
        // production caller must provide a complete LaunchIncarnation; this
        // check is intentionally stricter than checking its two numbers.
        return sidecar_launch.valid() && c_store_guid != CStoreGuid{} &&
               f_store_guid != FStoreGuid{} && c_store_guid != f_store_guid &&
               f_session_operation.valid() && endpoint_generation != 0 &&
               endpoint_session_serial != 0 && run_sequence != 0 &&
               socket_ownership_generation != 0;
    }
    friend bool operator==(const EndpointRunIdentity&, const EndpointRunIdentity&) =
        default;
};

enum class EndpointCancelReason : uint8_t {
    None = 0,
    CallerRequested,
    Deadline,
    ControlEof,
    OwnerFailure,
};

struct EndpointCancelPermit {
    EndpointRunIdentity identity{};
    uint64_t observation_id = 0;
    EndpointCancelReason reason = EndpointCancelReason::None;

    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && observation_id != 0 &&
               reason != EndpointCancelReason::None;
    }
    auto operator<=>(const EndpointCancelPermit&) const = default;
};

enum class EndpointCancelResult : uint8_t {
    CancelRequested = 0,
    AlreadyRequested,
    AlreadyTerminal,
    Stale,
};

enum class EndpointRunPhase : uint8_t {
    Admitted = 0,
    CacheWire,
    Materializing,
    Terminal,
};

enum class EndpointTerminalResultState : uint8_t {
    Pending = 0,
    Committed,
    Failed,
    Cancelled,
};

struct EndpointTerminalResult {
    EndpointTerminalResultState state = EndpointTerminalResultState::Pending;
    int error_value = 0;

    [[nodiscard]] bool terminal() const noexcept {
        return state != EndpointTerminalResultState::Pending;
    }
    auto operator<=>(const EndpointTerminalResult&) const = default;
};

// The target is an exact socket object, not an fd number.  It remains owned by
// the endpoint's shared asynchronous state and is invoked only after all
// permit fields have matched the registered row.
class EndpointSocketTarget {
public:
    virtual ~EndpointSocketTarget() = default;
    EndpointSocketTarget(const EndpointSocketTarget&) = delete;
    EndpointSocketTarget& operator=(const EndpointSocketTarget&) = delete;
    virtual void cancel() noexcept = 0;

protected:
    EndpointSocketTarget() = default;
};

class EndpointRunRegistry;

// Move-only authority retained by the owning FSession operation.  A handle
// never exposes its target or an fd; it can produce only an exact permit.
class EndpointRunHandle {
public:
    EndpointRunHandle() = default;
    ~EndpointRunHandle() = default;
    EndpointRunHandle(const EndpointRunHandle&) = delete;
    EndpointRunHandle& operator=(const EndpointRunHandle&) = delete;
    EndpointRunHandle(EndpointRunHandle&& other) noexcept
        : identity_(std::move(other.identity_)), deadline_(other.deadline_),
          observation_id_(other.observation_id_) {
        other.identity_ = {};
        other.deadline_ = {};
        other.observation_id_ = 0;
    }
    EndpointRunHandle& operator=(EndpointRunHandle&& other) noexcept {
        if (this != &other) {
            identity_ = std::move(other.identity_);
            deadline_ = other.deadline_;
            observation_id_ = other.observation_id_;
            other.identity_ = {};
            other.deadline_ = {};
            other.observation_id_ = 0;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return identity_.valid(); }
    [[nodiscard]] const EndpointRunIdentity& identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] const sidecar::AbsoluteMonotonicDeadline& deadline() const noexcept {
        return deadline_;
    }
    [[nodiscard]] EndpointCancelPermit permit(EndpointCancelReason reason) const noexcept {
        return EndpointCancelPermit{identity_, observation_id_, reason};
    }

private:
    EndpointRunHandle(EndpointRunIdentity identity,
                      sidecar::AbsoluteMonotonicDeadline deadline,
                      uint64_t observation_id) noexcept
        : identity_(std::move(identity)), deadline_(deadline),
          observation_id_(observation_id) {}

    EndpointRunIdentity identity_{};
    sidecar::AbsoluteMonotonicDeadline deadline_{};
    uint64_t observation_id_ = 0;
    friend class EndpointRunRegistry;
};

class EndpointRunRegistry {
public:
    struct Snapshot {
        EndpointRunIdentity identity{};
        sidecar::AbsoluteMonotonicDeadline deadline{};
        std::shared_ptr<EndpointSocketTarget> socket;
        EndpointRunPhase phase = EndpointRunPhase::Admitted;
        uint64_t timer_identity = 0;
        uint64_t observation_id = 0;
        bool cancel_requested = false;
        EndpointTerminalResult terminal{};
    };

    explicit EndpointRunRegistry(size_t max_live_runs = 64);
    EndpointRunRegistry(const EndpointRunRegistry&) = delete;
    EndpointRunRegistry& operator=(const EndpointRunRegistry&) = delete;

    [[nodiscard]] std::optional<EndpointRunHandle> admit(
        EndpointRunIdentity identity,
        sidecar::AbsoluteMonotonicDeadline deadline,
        std::shared_ptr<EndpointSocketTarget> socket,
        uint64_t timer_identity = 0);

    [[nodiscard]] EndpointCancelResult request_cancel(
        const EndpointCancelPermit& permit) noexcept;

    // Shutdown/failure is the one intentionally broader operation.  It still
    // requires the exact launch incarnation and cannot be reached through an
    // ordinary EndpointCancelPermit.
    size_t cancel_all_for_incarnation(
        const SidecarLaunchIdentity& incarnation,
        EndpointCancelReason reason = EndpointCancelReason::OwnerFailure) noexcept;

    bool set_phase(const EndpointRunIdentity& identity, EndpointRunPhase phase) noexcept;
    bool mark_terminal(const EndpointRunIdentity& identity,
                       EndpointTerminalResult result) noexcept;
    [[nodiscard]] std::optional<EndpointTerminalResult>
    consume_terminal(const EndpointRunIdentity& identity) noexcept;

    [[nodiscard]] std::optional<Snapshot>
    inspect(const EndpointRunIdentity& identity) const;
    [[nodiscard]] size_t live_count() const noexcept { return active_runs_.size(); }
    [[nodiscard]] size_t timer_count() const noexcept;
    [[nodiscard]] size_t target_count() const noexcept;
    [[nodiscard]] size_t cancelled_live_count() const noexcept;

private:
    struct IdentityLess {
        bool operator()(const EndpointRunIdentity& left,
                        const EndpointRunIdentity& right) const noexcept {
            if (left.sidecar_launch.identity != right.sidecar_launch.identity)
                return left.sidecar_launch.identity < right.sidecar_launch.identity;
            if (left.sidecar_launch.store_generation !=
                right.sidecar_launch.store_generation)
                return left.sidecar_launch.store_generation <
                       right.sidecar_launch.store_generation;
            if (left.sidecar_launch.store_root.bytes !=
                right.sidecar_launch.store_root.bytes)
                return left.sidecar_launch.store_root.bytes <
                       right.sidecar_launch.store_root.bytes;
            if (left.c_store_guid.bytes != right.c_store_guid.bytes)
                return left.c_store_guid.bytes < right.c_store_guid.bytes;
            if (left.f_store_guid.bytes != right.f_store_guid.bytes)
                return left.f_store_guid.bytes < right.f_store_guid.bytes;
            if (left.f_session_operation != right.f_session_operation)
                return left.f_session_operation < right.f_session_operation;
            if (left.endpoint_generation != right.endpoint_generation)
                return left.endpoint_generation < right.endpoint_generation;
            if (left.endpoint_session_serial != right.endpoint_session_serial)
                return left.endpoint_session_serial < right.endpoint_session_serial;
            if (left.run_sequence != right.run_sequence)
                return left.run_sequence < right.run_sequence;
            return left.socket_ownership_generation <
                   right.socket_ownership_generation;
        }
    };
    std::map<EndpointRunIdentity, Snapshot, IdentityLess> active_runs_;
    size_t max_live_runs_ = 0;
    uint64_t next_observation_id_ = 1;
};

} // namespace icecc::p50
