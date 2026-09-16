#pragma once

#include <cstdint>

namespace icecc::p50::daemon {

// A wrapper's explicit cache offer is the authority for whether its one-job
// GetCS can benefit from the successor sidecar lease.  remote_required is a
// separate placement policy: strict P50 retries deliberately leave it zero.
inline constexpr bool should_defer_cache_capable_getcs(
    uint32_t count, uint32_t requested_profile_mask, bool sidecar_ready,
    bool recovery_in_progress) noexcept
{
    return count == 1 && requested_profile_mask != 0 && !sidecar_ready &&
           recovery_in_progress;
}

} // namespace icecc::p50::daemon
