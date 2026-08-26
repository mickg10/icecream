#pragma once

// Store identity is deliberately independent of the daemon control launch
// identity (generation/attempt). A fresh 127-bit root comes from the kernel
// CSPRNG for every launch; the fixed high role bit derives the C and F GUIDs.

#include "p50_local_transport.h"
#include "protocol50.h"
#include "../services/p50_store_identity_wire.h"

#include <array>
#include <cerrno>
#include <cstdint>
#if defined(__linux__)
#include <sys/random.h>
#endif
#include <unistd.h>

namespace icecc::p50 {

inline constexpr uint8_t kStoreIdentityRoleBit = kStoreIdentityRoleMask;

struct StoreIdentityRoot {
    std::array<uint8_t, 16> bytes{};

    [[nodiscard]] bool valid() const noexcept {
        bool nonzero = false;
        for (const uint8_t byte : bytes)
            nonzero = nonzero || byte != 0;
        return nonzero && (bytes[0] & kStoreIdentityRoleBit) == 0;
    }

    friend bool operator==(const StoreIdentityRoot&, const StoreIdentityRoot&) = default;
};

using StoreIdentityEntropyProvider = ssize_t (*)(void*, size_t, unsigned) noexcept;

inline ssize_t system_store_identity_entropy(void* buffer, size_t size,
                                             unsigned flags) noexcept {
#if defined(__linux__)
    return ::getrandom(buffer, size, flags);
#else
    (void)buffer;
    (void)size;
    (void)flags;
    return -1;
#endif
}

inline bool fresh_store_identity_root_with_provider(
    StoreIdentityRoot& root, StoreIdentityEntropyProvider provider) noexcept {
    root = {};
    constexpr int kMaxGetrandomRetries = 4;
    if (provider == nullptr)
        return false;
    for (int retry = 0; retry != kMaxGetrandomRetries; ++retry) {
        const ssize_t result = provider(root.bytes.data(), root.bytes.size(), 0);
        if (result == static_cast<ssize_t>(root.bytes.size())) {
            root.bytes[0] &= static_cast<uint8_t>(~kStoreIdentityRoleBit);
            return root.valid();
        }
        if (result > 0) {
            root = {};
            return false;
        }
        if (result < 0 && errno == EINTR)
            continue;
        root = {};
        return false;
    }
    root = {};
    return false;
}

inline bool fresh_store_identity_root(StoreIdentityRoot& root) noexcept {
    return fresh_store_identity_root_with_provider(root, system_store_identity_entropy);
}

template <typename Guid>
inline Guid store_guid_for_root(StoreIdentityRoot root, uint8_t role_bit) noexcept {
    Guid result{};
    root.bytes[0] &= static_cast<uint8_t>(~kStoreIdentityRoleBit);
    root.bytes[0] |= role_bit;
    result.bytes = root.bytes;
    return result;
}

inline FStoreGuid f_store_guid_for_root(StoreIdentityRoot root) noexcept {
    // The role suffix is part of the wire identity: F is root||1.
    return store_guid_for_root<FStoreGuid>(root, kStoreIdentityFileRole);
}

inline CStoreGuid c_store_guid_for_root(StoreIdentityRoot root) noexcept {
    // The role suffix is part of the wire identity: C is root||0.
    return store_guid_for_root<CStoreGuid>(root, kStoreIdentityClientRole);
}

inline StoreIdentityRoot store_identity_root_from_f_guid(FStoreGuid guid) noexcept {
    StoreIdentityRoot root{};
    root.bytes = guid.bytes;
    root.bytes[0] &= static_cast<uint8_t>(~kStoreIdentityRoleBit);
    return root;
}

} // namespace icecc::p50
