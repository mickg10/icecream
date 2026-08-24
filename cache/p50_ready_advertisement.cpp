#include "p50_ready_advertisement.h"

#include "services/comm.h"

#include <cassert>
#include <limits>

namespace icecc::p50::advertisement {

namespace {

constexpr Snapshot kAbsent{};

Snapshot present_snapshot(uint32_t port) noexcept
{
    return Snapshot{port, CACHE_WIRE_PROTOCOL_V1, CACHE_PROFILE_ZSTD_TU};
}

} // namespace

bool Snapshot::absent() const noexcept
{
    return cache_advertisement_is_wholly_absent(endpoint_port, protocol,
                                                 profile_mask);
}

bool Snapshot::present() const noexcept
{
    return cache_advertisement_is_valid_present(endpoint_port, protocol,
                                                 profile_mask);
}

bool operator==(const Snapshot& left, const Snapshot& right) noexcept
{
    return left.endpoint_port == right.endpoint_port
        && left.protocol == right.protocol
        && left.profile_mask == right.profile_mask;
}

bool operator!=(const Snapshot& left, const Snapshot& right) noexcept
{
    return !(left == right);
}

void Controller::append(Update& update, const Snapshot& snapshot) noexcept
{
    assert(update.count < update.transitions.size());
    update.transitions[update.count++] = snapshot;
}

Update Controller::observe(const Observation& observation) noexcept
{
    Update update;
    bool crashed = false;

    // Supervisor counters increment saturating.  At UINT64_MAX another exit
    // is observationally indistinguishable from no exit, so presence can no
    // longer be proved safe.  Record MAX as the permanent high-water mark and
    // fail closed until a new Controller/daemon generation is constructed.
    if (observation.cumulative_post_ready_exits
        == std::numeric_limits<uint64_t>::max()) {
        highest_post_ready_exits_ = observation.cumulative_post_ready_exits;
        counter_observed_ = true;
        update.error = Error::CounterSaturated;
    } else if (!counter_observed_) {
        highest_post_ready_exits_ = observation.cumulative_post_ready_exits;
        counter_observed_ = true;
    } else if (observation.cumulative_post_ready_exits
               < highest_post_ready_exits_) {
        update.error = Error::CounterRegression;
    } else if (observation.cumulative_post_ready_exits
               > highest_post_ready_exits_) {
        highest_post_ready_exits_ = observation.cumulative_post_ready_exits;
        crashed = true;
    }

    if (update.error == Error::None && observation.public_listener_bound
        && (observation.public_listener_port == 0
            || observation.public_listener_port
                > std::numeric_limits<uint16_t>::max())) {
        update.error = Error::InvalidPublicPort;
    }

    // A post-READY exit is an edge, not merely a level.  If the adapter's
    // poll also observes a recovered READY relationship, subscribers must
    // still see absence before the replacement incarnation is advertised.
    if (crashed && current_.present()) {
        append(update, kAbsent);
        current_ = kAbsent;
    }

    if (update.error != Error::None) {
        if (current_ != kAbsent) {
            append(update, kAbsent);
            current_ = kAbsent;
        }
        return update;
    }

    const bool ready = observation.supervisor_state == sidecar::State::Ready;
    const bool authenticated = observation.private_relationship_authenticated;
    const bool advertise = observation.public_listener_bound && ready
        && authenticated;
    const Snapshot target = advertise
        ? present_snapshot(observation.public_listener_port)
        : kAbsent;

    if (target != current_) {
        append(update, target);
        current_ = target;
    }
    return update;
}

} // namespace icecc::p50::advertisement
