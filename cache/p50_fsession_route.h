/*
    Protocol-50 F-session route admission (S2 vertical).

    A C-store GUID selects a namespace; it does not authorize endpoint
    replacement. Exact-claim CacheWire begins only by consuming a move-only
    route reservation owned by the same distributed F-session operation; a new
    socket cannot evict a live operation merely by naming the same C GUID
    (issue-16 5443966435). Cold-route predecessors carry a tagged state:
    REL_SEQ 0 / zero digest are legal when tagged cold, while identity and
    capability fields remain nonzero (5444410383 sec.7).

    This header supplies the authority objects; endpoint wiring consumes them:
      - RoutePredecessor      : tagged cold-or-established predecessor binding
      - RouteSessionLease     : the move-only reservation (one per namespace)
      - RouteAdmissionOwner   : the sidecar F-owner's per-namespace state
        machine (Idle/Reserved/Active/Committed/ResetRequired/ReconcileRequired)
        enforcing same-route exclusion, duplicate-suppression, stale-event
        fencing via a route-owner generation, and settled-successor handover.
*/
#ifndef ICECC_CACHE_P50_FSESSION_ROUTE_H
#define ICECC_CACHE_P50_FSESSION_ROUTE_H

#include "p50_endpoint_run_cancel.h" // EndpointRunIdentity
#include "p50_fsession_control.h"    // FSessionOperationIdentity

#include <array>
#include <cstdint>
#include <optional>

namespace icecc::p50::fsession {

// Tagged predecessor binding. A legal cold route has rel_seq == 0 and a zero
// digest with cold == true; an established predecessor requires both nonzero.
struct RoutePredecessor {
    bool cold = true;
    uint64_t rel_seq = 0;
    std::array<uint8_t, 16> state_digest{};

    [[nodiscard]] bool valid() const noexcept {
        if (cold)
            return rel_seq == 0 && state_digest == std::array<uint8_t, 16>{};
        bool nonzero = false;
        for (uint8_t b : state_digest)
            nonzero = nonzero || b != 0;
        return rel_seq != 0 && nonzero;
    }
    friend bool operator==(const RoutePredecessor&, const RoutePredecessor&) =
        default;
};

// Move-only route reservation. Minted only by RouteAdmissionOwner::reserve();
// the exact-claim endpoint path consumes it before CacheWire begins.
class RouteSessionLease {
public:
    RouteSessionLease(const RouteSessionLease&) = delete;
    RouteSessionLease& operator=(const RouteSessionLease&) = delete;
    RouteSessionLease(RouteSessionLease&& other) noexcept { *this = std::move(other); }
    RouteSessionLease& operator=(RouteSessionLease&& other) noexcept {
        operation_ = other.operation_;
        endpoint_run_ = other.endpoint_run_;
        predecessor_ = other.predecessor_;
        route_owner_generation_ = other.route_owner_generation_;
        admission_sequence_ = other.admission_sequence_;
        valid_ = other.valid_;
        other.valid_ = false;
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const FSessionOperationIdentity& operation() const noexcept {
        return operation_;
    }
    [[nodiscard]] const EndpointRunIdentity& endpoint_run() const noexcept {
        return endpoint_run_;
    }
    [[nodiscard]] const RoutePredecessor& predecessor() const noexcept {
        return predecessor_;
    }
    [[nodiscard]] uint64_t route_owner_generation() const noexcept {
        return route_owner_generation_;
    }
    [[nodiscard]] uint64_t admission_sequence() const noexcept {
        return admission_sequence_;
    }

private:
    friend class RouteAdmissionOwner;
    RouteSessionLease() = default;

    FSessionOperationIdentity operation_{};
    EndpointRunIdentity endpoint_run_{};
    RoutePredecessor predecessor_{};
    uint64_t route_owner_generation_ = 0;
    uint64_t admission_sequence_ = 0;
    bool valid_ = false;
};

enum class RouteAdmissionState : uint8_t {
    Idle = 0,
    Reserved,
    Active,
    Committed,          // committed but not yet settled (still excludes B)
    ResetRequired,
    ReconcileRequired,
};

enum class RouteRefusal : uint8_t {
    None = 0,
    RouteBusy,            // a different live operation owns the route
    DuplicateOperation,   // same operation already reserved/active (no 2nd run)
    WrongPredecessor,     // predecessor does not match the committed successor
    InvalidRequest,
};

// The sidecar F-owner's per-namespace route state machine. Single-writer:
// only the owning reducer calls these; endpoint/timer/disconnect callbacks
// deliver identity-bound observations that are fenced by the route-owner
// generation (a stale serial/socket can never regain authority).
class RouteAdmissionOwner {
public:
    [[nodiscard]] RouteAdmissionState state() const noexcept { return state_; }
    [[nodiscard]] uint64_t route_owner_generation() const noexcept {
        return generation_;
    }

    // Reserve the route for one operation before CacheWire. Refuses while any
    // other operation is live (Reserved/Active/Committed-unsettled/Reconcile);
    // refuses a duplicate reservation for the same operation (no second run);
    // requires the predecessor to match the route cursor (the committed
    // successor after settlement, or tagged-cold on a fresh route).
    [[nodiscard]] std::optional<RouteSessionLease>
    reserve(const FSessionOperationIdentity& operation,
            const EndpointRunIdentity& endpoint_run,
            const RoutePredecessor& predecessor, RouteRefusal& refusal);

    // Reserved -> Active (the endpoint consumed the lease and CacheWire began).
    [[nodiscard]] bool activate(const RouteSessionLease& lease);
    // Active -> Committed: record the committed successor (still excludes B).
    [[nodiscard]] bool
    commit(const RouteSessionLease& lease, const RoutePredecessor& successor);
    // Committed -> Idle: terminal settlement releases the route; the successor
    // becomes the route cursor and the owner generation advances (stale fence).
    [[nodiscard]] bool settle_and_release(const RouteSessionLease& lease);
    // Any live state -> ReconcileRequired (e.g. control loss after commit).
    [[nodiscard]] bool mark_reconcile_required(const RouteSessionLease& lease);
    // Active -> ResetRequired when endpoint work touched the route context
    // before a durable commit; reset must complete before the route can be
    // reused.  A committed operation uses reconciliation instead.
    [[nodiscard]] bool mark_reset_required(const RouteSessionLease& lease);
    // ReconcileRequired/ResetRequired -> Idle after the owner resolves it; the
    // generation advances so late events from the old operation are stale.
    void resolve_and_release() noexcept;

    // Identity-bound stale-event check for late disconnect/timer/cancel/codec/
    // terminal callbacks: true only if the event names the CURRENT generation
    // and the current live operation.
    [[nodiscard]] bool
    event_current(uint64_t route_owner_generation,
                  const FSessionOperationIdentity& operation) const noexcept;

private:
    [[nodiscard]] bool lease_current(const RouteSessionLease& lease) const noexcept;

    RouteAdmissionState state_ = RouteAdmissionState::Idle;
    uint64_t generation_ = 1;
    uint64_t next_admission_sequence_ = 1;
    std::optional<FSessionOperationIdentity> live_operation_;
    // Sequential-ABA fence with PROVEN non-revival depth: retired operations
    // are fenced by a monotonic same-launch frontier (allocators mint
    // operation sequences monotonically per sidecar launch), so EVERY earlier
    // operation of the current launch is inadmissible -- not merely the most
    // recent one (root probe control 7). A launch rotation starts a fresh
    // frontier.
    daemon::P50WireLaunchIdentity retired_launch_{};
    uint64_t retired_frontier_sequence_ = 0;
    RoutePredecessor cursor_{}; // tagged-cold initially; committed successor after
};

} // namespace icecc::p50::fsession

#endif // ICECC_CACHE_P50_FSESSION_ROUTE_H
