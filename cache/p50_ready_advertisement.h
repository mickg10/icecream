#pragma once

// Pure readiness-to-Login projection for the Protocol-50 cache sidecar.
//
// This component deliberately owns no process, socket, scheduler channel, or
// daemon state.  A daemon adapter supplies one observation at a time and
// applies the returned canonical transitions to LoginMsg.  Keeping the
// projection pure makes the fail-closed advertisement policy independently
// testable before production lifecycle wiring lands.

#include "p50_sidecar_supervisor.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace icecc::p50::advertisement {

struct Snapshot {
    uint32_t endpoint_port = 0;
    uint32_t protocol = 0;
    uint32_t profile_mask = 0;

    [[nodiscard]] bool absent() const noexcept;
    [[nodiscard]] bool present() const noexcept;
};

[[nodiscard]] bool operator==(const Snapshot& left,
                              const Snapshot& right) noexcept;
[[nodiscard]] bool operator!=(const Snapshot& left,
                              const Snapshot& right) noexcept;

enum class Error : uint8_t {
    None = 0,
    InvalidPublicPort,
    CounterRegression,
    CounterSaturated,
};

struct Observation {
    bool public_listener_bound = false;
    uint32_t public_listener_port = 0;
    sidecar::State supervisor_state = sidecar::State::Stopped;
    bool private_relationship_authenticated = false;
    // The daemon adapter owns this cumulative value across Supervisor object
    // recreation.  A per-instance counter reset is therefore a fail-closed
    // regression, not a new baseline.
    uint64_t cumulative_post_ready_exits = 0;
    // The daemon adapter sets this from the immutable current READY lease.
    // A stale/replaced pathname, F_STORE_GUID, PID, or listener inode must
    // withdraw capability even if an old relationship still authenticates.
    bool current_lease_matches = true;
};

// One observation can produce two transitions only when a crash and recovery
// have both happened between polls: absent must be published before present.
struct Update {
    std::array<Snapshot, 2> transitions{};
    size_t count = 0;
    Error error = Error::None;
};

class Controller {
public:
    [[nodiscard]] Update observe(const Observation& observation) noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept { return current_; }

private:
    static void append(Update& update, const Snapshot& snapshot) noexcept;

    Snapshot current_{};
    uint64_t highest_post_ready_exits_ = 0;
    bool counter_observed_ = false;
};

} // namespace icecc::p50::advertisement
