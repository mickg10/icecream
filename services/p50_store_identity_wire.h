#pragma once

// Dependency-light StoreIdentity wire contract shared by launch/service and
// ordinary-message consumers.  The root occupies 127 bits; the fixed role
// bit is carried in byte zero.  C is root||0 and F is root||1.  Raw roots
// never belong on ordinary protocol frames.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>

namespace icecc::p50 {

namespace daemon {

// Protocol-50 wire launch identity.  This value type was originally defined
// in the retired services/p50_cache_session_wire.h (the P5CL/P5CO
// claim/outcome design; ids 0x50f00012/13 stay reserved in Msg::Value);
// production endpoint/cancellation code kept the type itself.
struct P50WireLaunchIdentity {
    uint64_t generation = 0;
    uint64_t attempt = 0;

    [[nodiscard]] bool valid() const noexcept {
        return generation != 0 && attempt != 0;
    }
    auto operator<=>(const P50WireLaunchIdentity &) const = default;
};

// Numeric value aligns with the typed F-session operation target.  The DTO
// stays dependency-free from cache/p50_control_operation.h.
enum class P50SessionOperationRole : uint16_t {
    FSession = 2,
};

struct P50FSessionOperationId {
    P50WireLaunchIdentity sidecar_launch{};
    P50SessionOperationRole role = P50SessionOperationRole::FSession;
    uint64_t operation_sequence = 0;

    [[nodiscard]] bool valid() const noexcept {
        return sidecar_launch.valid() &&
               role == P50SessionOperationRole::FSession &&
               operation_sequence != 0;
    }
    auto operator<=>(const P50FSessionOperationId &) const = default;
};

} // namespace icecc::p50::daemon

inline constexpr uint64_t kStoreIdentityDerivationVersion = 1;
inline constexpr size_t kStoreIdentityRoleByte = 0;
inline constexpr uint8_t kStoreIdentityRoleMask = 0x80;
inline constexpr uint8_t kStoreIdentityClientRole = 0;
inline constexpr uint8_t kStoreIdentityFileRole = kStoreIdentityRoleMask;

// Validate both the fixed role and the 127-bit incarnation root.  The role
// bit alone is never identity entropy, and callers may not invent new roles.
inline bool store_identity_guid_valid_for_role(
    const std::array<uint8_t, 16> &guid, uint8_t expected_role) noexcept
{
    if (expected_role != kStoreIdentityClientRole &&
        expected_role != kStoreIdentityFileRole)
        return false;
    if ((guid[kStoreIdentityRoleByte] & kStoreIdentityRoleMask) != expected_role)
        return false;
    for (size_t i = 0; i != guid.size(); ++i) {
        const uint8_t root_byte = i == kStoreIdentityRoleByte
            ? static_cast<uint8_t>(guid[i] & ~kStoreIdentityRoleMask)
            : guid[i];
        if (root_byte != 0)
            return true;
    }
    return false; // P50_ROOT_NONZERO_CHECK
}

inline bool store_identity_file_guid_matches_client(
    const std::array<uint8_t, 16> &client_guid,
    const std::array<uint8_t, 16> &file_guid) noexcept
{
    if (!store_identity_guid_valid_for_role(client_guid,
                                            kStoreIdentityClientRole) ||
        !store_identity_guid_valid_for_role(file_guid,
                                            kStoreIdentityFileRole))
        return false;
    for (size_t i = 0; i != client_guid.size(); ++i) {
        const uint8_t mask = i == kStoreIdentityRoleByte
            ? static_cast<uint8_t>(~kStoreIdentityRoleMask)
            : static_cast<uint8_t>(0xff);
        if ((client_guid[i] & mask) != (file_guid[i] & mask))
            return false;
    }
    return true;
}

} // namespace icecc::p50
