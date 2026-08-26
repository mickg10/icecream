#pragma once

// Dependency-light StoreIdentity wire contract shared by launch/service and
// ordinary-message consumers. The root occupies 127 bits; the fixed role bit
// is carried in byte zero. C is root||0 and F is root||1.

#include <cstddef>
#include <cstdint>

namespace icecc::p50 {

inline constexpr uint64_t kStoreIdentityDerivationVersion = 1;
inline constexpr size_t kStoreIdentityRoleByte = 0;
inline constexpr uint8_t kStoreIdentityRoleMask = 0x80;
inline constexpr uint8_t kStoreIdentityClientRole = 0;
inline constexpr uint8_t kStoreIdentityFileRole = kStoreIdentityRoleMask;

} // namespace icecc::p50
