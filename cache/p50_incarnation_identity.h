#pragma once

// One canonical mapping from the daemon-owned sidecar launch incarnation to
// the empty F-store GUID.  Keeping this in a dependency-light header prevents
// the supervisor and service from silently deriving different identities.

#include "p50_local_transport.h"
#include "protocol50.h"

#include <cstdint>

namespace icecc::p50 {

inline FStoreGuid f_store_guid_for_incarnation(local::Identity identity) noexcept
{
    FStoreGuid result{};
    for (size_t index = 0; index != sizeof(identity.generation); ++index)
        result.bytes[index] = static_cast<uint8_t>(
            identity.generation >> (56u - static_cast<unsigned>(index) * 8u));
    for (size_t index = 0; index != sizeof(identity.attempt); ++index)
        result.bytes[sizeof(identity.generation) + index] = static_cast<uint8_t>(
            identity.attempt >> (56u - static_cast<unsigned>(index) * 8u));
    return result;
}

// C and F are deliberately separate domains.  F remains the launch/attempt
// namespace above; C is the exact reversible full-tuple law below.  A fixed
// nonzero domain mask is XORed into only the attempt word, so the mapping is
// bijective (no probabilistic collision claim) and C can never equal F.
inline constexpr uint64_t kCIncarnationAttemptDomainMask = 0x4353545200000001ULL;

inline CStoreGuid c_store_guid_for_incarnation(local::Identity identity) noexcept
{
    CStoreGuid result{};
    for (size_t index = 0; index != sizeof(identity.generation); ++index)
        result.bytes[index] = static_cast<uint8_t>(
            identity.generation >> (56u - static_cast<unsigned>(index) * 8u));
    const uint64_t domain_attempt = identity.attempt ^ kCIncarnationAttemptDomainMask;
    for (size_t index = 0; index != sizeof(domain_attempt); ++index)
        result.bytes[sizeof(identity.generation) + index] = static_cast<uint8_t>(
            domain_attempt >> (56u - static_cast<unsigned>(index) * 8u));
    return result;
}

} // namespace icecc::p50
