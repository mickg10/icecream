#pragma once

// Dependency-light StoreIdentity wire contract shared by launch/service and
// ordinary-message consumers.  The root occupies 127 bits; the fixed role
// bit is carried in byte zero.  C is root||0 and F is root||1.  Raw roots
// never belong on ordinary protocol frames.

#include <array>
#include <cstddef>
#include <cstdint>

namespace icecc::p50 {

inline constexpr uint64_t kStoreIdentityDerivationVersion = 1;
inline constexpr size_t kStoreIdentityRoleByte = 0;
inline constexpr uint8_t kStoreIdentityRoleMask = 0x80;
inline constexpr uint8_t kStoreIdentityClientRole = 0;
inline constexpr uint8_t kStoreIdentityFileRole = kStoreIdentityRoleMask;

inline bool store_identity_file_guid_matches_client(
    const std::array<uint8_t, 16> &client_guid,
    const std::array<uint8_t, 16> &file_guid) noexcept
{
    if ((client_guid[kStoreIdentityRoleByte] & kStoreIdentityRoleMask) !=
            kStoreIdentityClientRole ||
        (file_guid[kStoreIdentityRoleByte] & kStoreIdentityRoleMask) !=
            kStoreIdentityFileRole)
        return false;
    for (size_t i = 0; i != client_guid.size(); ++i) {
        if (i != kStoreIdentityRoleByte && client_guid[i] != file_guid[i])
            return false;
    }
    return file_guid[kStoreIdentityRoleByte] ==
           static_cast<uint8_t>(client_guid[kStoreIdentityRoleByte] |
                                kStoreIdentityFileRole);
}

} // namespace icecc::p50
