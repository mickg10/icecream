#pragma once

// One canonical mapping from the daemon-owned sidecar launch incarnation to
// the empty F-store GUID.  Keeping this in a dependency-light header prevents
// the supervisor and service from silently deriving different identities.

#include "p50_local_transport.h"
#include "protocol50.h"

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
// namespace above; C uses an explicit domain marker before the same immutable
// incarnation tuple.  This makes equality impossible even when both stores
// describe the same supervised process.
inline CStoreGuid c_store_guid_for_incarnation(local::Identity identity) noexcept
{
    CStoreGuid result{};
    result.bytes[0] = 0x43; // "C"
    result.bytes[1] = 0x53; // "S"
    result.bytes[2] = 0x54; // "T"
    result.bytes[3] = 0x52; // "R"
    for (size_t index = 0; index != sizeof(identity.generation); ++index)
        result.bytes[4 + index] = static_cast<uint8_t>(
            identity.generation >> (56u - static_cast<unsigned>(index) * 8u));
    for (size_t index = 0; index != sizeof(identity.attempt); ++index)
        result.bytes[8 + index] = static_cast<uint8_t>(
            identity.attempt >> (56u - static_cast<unsigned>(index) * 8u));
    return result;
}

} // namespace icecc::p50
