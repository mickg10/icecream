#pragma once

// Launch identity contracts shared by the daemon, sidecar service, and
// lifecycle owners.  This header has no dependency on the synchronous
// Supervisor implementation.

#include "p50_incarnation_identity.h"
#include "p50_local_transport.h"
#include "protocol50.h"
#include "../services/digest128.h"

#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace icecc::p50::sidecar {

namespace detail {

inline bool canonical_absolute_lease_path(std::string_view path) noexcept {
    if (path.empty() || path.front() != '/' || path.size() > local::kMaxUnixPath ||
        path.back() == '/' || path.find('\0') != std::string_view::npos)
        return false;
    size_t begin = 1;
    while (begin < path.size()) {
        const size_t end = path.find('/', begin);
        const size_t length = end == std::string_view::npos ? path.size() - begin
                                                              : end - begin;
        if (length == 0 || (length == 1 && path[begin] == '.') ||
            (length == 2 && path[begin] == '.' && path[begin + 1] == '.'))
            return false;
        for (size_t index = begin; index != begin + length; ++index) {
            const char character = path[index];
            if (character == ' ' || character == '\t' || character == '\n' ||
                character == '\r')
                return false;
        }
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return true;
}

} // namespace detail

inline constexpr std::string_view kReadyFdEnvironment =
    "ICECC_CACHE_SERVICE_READY_FD";
inline constexpr std::string_view kListenerFdEnvironment =
    "ICECC_CACHE_SERVICE_LISTENER_FD";

enum class State : uint8_t {
    Stopped = 0,
    Starting,
    Ready,
    DegradedLegacy,
    Stopping,
};

struct LaunchIncarnation {
    local::Identity identity{};
    uint64_t store_generation = 0;
    StoreIdentityRoot store_root{};
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};

    [[nodiscard]] bool valid() const noexcept {
        return identity.generation != 0 && identity.attempt != 0 &&
               store_generation != 0 &&
               store_generation != std::numeric_limits<uint64_t>::max() &&
               store_root.valid() &&
               store_identity_guid_valid_for_role(c_store_guid.bytes,
                                                  kStoreIdentityClientRole) &&
               store_identity_guid_valid_for_role(f_store_guid.bytes,
                                                  kStoreIdentityFileRole) &&
               c_store_guid != f_store_guid &&
               c_store_guid == c_store_guid_for_root(store_root) &&
               f_store_guid == f_store_guid_for_root(store_root);
    }
    friend bool operator==(const LaunchIncarnation&,
                           const LaunchIncarnation&) = default;
};

class LaunchIdentityAllocator {
public:
    // No defaults: a missed call site must fail to compile rather than
    // silently allocate store identities with the native getrandom()-backed
    // entropy provider under Compat (M3, random), which on kernel 3.10 is a
    // runtime failure instead of a build error. Every caller must name the
    // entropy provider explicitly.
    LaunchIdentityAllocator(uint64_t generation, uint64_t first_attempt,
                            StoreIdentityEntropyProvider entropy_provider) noexcept;
    std::optional<LaunchIncarnation> allocate() noexcept;

private:
    std::mutex mutex_;
    uint64_t generation_ = 0;
    uint64_t next_attempt_ = 0;
    uint64_t next_store_generation_ = 0;
    StoreIdentityEntropyProvider entropy_provider_ = nullptr;
    std::deque<StoreIdentityRoot> recent_roots_;
};

struct ReadyLease {
    local::Identity identity{};
    uint64_t store_generation = 0;
    pid_t pid = -1;
    StoreIdentityRoot store_root{};
    uint64_t store_derivation_version = 0;
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    std::string private_directory;
    std::string socket_path;
    Digest128 socket_path_digest{};
    dev_t listener_device = 0;
    ino_t listener_inode = 0;
    dev_t directory_device = 0;
    ino_t directory_inode = 0;

    [[nodiscard]] bool valid() const noexcept {
        if (identity.generation == 0 || identity.attempt == 0 ||
            store_generation == 0 ||
            store_generation == std::numeric_limits<uint64_t>::max() ||
            pid <= 1 || !store_root.valid() ||
            store_derivation_version != kStoreIdentityDerivationVersion ||
            !store_identity_guid_valid_for_role(c_store_guid.bytes,
                                                kStoreIdentityClientRole) ||
            !store_identity_guid_valid_for_role(f_store_guid.bytes,
                                                kStoreIdentityFileRole) ||
            c_store_guid != c_store_guid_for_root(store_root) ||
            f_store_guid != f_store_guid_for_root(store_root) ||
            c_store_guid == f_store_guid ||
            !detail::canonical_absolute_lease_path(private_directory) ||
            private_directory.size() + sizeof("/cache.sock") - 1 > local::kMaxUnixPath ||
            socket_path.size() != private_directory.size() + sizeof("/cache.sock") - 1 ||
            socket_path.compare(0, private_directory.size(), private_directory) != 0 ||
            socket_path[private_directory.size()] != '/' ||
            socket_path.compare(private_directory.size() + 1, sizeof("cache.sock") - 1,
                                "cache.sock") != 0 ||
            socket_path_digest != digest128(socket_path) || listener_device == 0 ||
            listener_inode == 0 || directory_device == 0 || directory_inode == 0)
            return false;
        return true;
    }
};

struct Counters {
    uint64_t launches = 0;
    uint64_t restarts = 0;
    uint64_t exec_failures = 0;
    uint64_t readiness_timeouts = 0;
    uint64_t pre_ready_exits = 0;
    uint64_t invalid_ready_messages = 0;
    uint64_t post_ready_exits = 0;
    uint64_t shutdowns = 0;
    uint64_t forced_kills = 0;
};

} // namespace icecc::p50::sidecar
