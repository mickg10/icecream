// Test-only reference component; not part of the live P50 implementation.
#pragma once

// Typed, bounded ownership skeleton for the two BigOracle roles.  This is a
// pre-adoption guard: the role discriminator and the shared C/F namespace are
// checked before a descriptor received through SCM_RIGHTS is admitted to a
// role owner.  CompileFile/session wiring intentionally remains out of scope.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <utility>

#include "cache/protocol50.h"

namespace icecc::p50::role {

enum class RoleDiscriminator : uint8_t {
    Client = 1,
    Server = 2,
};

struct RoleLimits {
    size_t max_live_fds = 1;
};

struct RoleCounters {
    uint64_t accepted = 0;
    uint64_t rejected_wrong_role = 0;
    uint64_t rejected_namespace = 0;
    uint64_t rejected_limit = 0;
};

// The accounting state outlives an owner when an adopted descriptor is
// intentionally transferred or retained.  A raw pointer into the owner would
// become dangling as soon as that owner was destroyed.
struct RoleLiveState {
    size_t live = 0;
};

struct RoleOwnedFd {
    RoleOwnedFd() noexcept = default;
    RoleOwnedFd(RoleDiscriminator role, int fd,
                std::shared_ptr<RoleLiveState> live_counter = {}) noexcept
        : role(role), fd(fd), live_counter(std::move(live_counter)) {}
    ~RoleOwnedFd();
    RoleOwnedFd(RoleOwnedFd&& other) noexcept;
    RoleOwnedFd& operator=(RoleOwnedFd&& other) noexcept;
    RoleOwnedFd(const RoleOwnedFd&) = delete;
    RoleOwnedFd& operator=(const RoleOwnedFd&) = delete;

    [[nodiscard]] bool valid() const noexcept { return fd >= 0; }
    [[nodiscard]] int get() const noexcept { return fd; }
    [[nodiscard]] RoleDiscriminator discriminator() const noexcept { return role; }
    [[nodiscard]] int release() noexcept;

    RoleDiscriminator role = RoleDiscriminator::Client;
    int fd = -1;
    std::shared_ptr<RoleLiveState> live_counter;
};

// Both role owners intentionally carry their own state.  A limit/counter in
// one domain can never silently consume the budget of the other domain.
class ClientRoleOwner {
public:
    ClientRoleOwner(CStoreGuid c_store_guid, FStoreGuid f_store_guid,
                    RoleLimits limits = {});
    ~ClientRoleOwner();
    ClientRoleOwner(const ClientRoleOwner&) = delete;
    ClientRoleOwner& operator=(const ClientRoleOwner&) = delete;

    [[nodiscard]] bool namespace_matches(CStoreGuid c_store_guid,
                                         FStoreGuid f_store_guid) const noexcept;
    [[nodiscard]] bool admit_role(RoleDiscriminator role) noexcept;
    std::optional<RoleOwnedFd> adopt(RoleDiscriminator role, CStoreGuid c_store_guid,
                                      FStoreGuid f_store_guid, int fd) noexcept;
    [[nodiscard]] const RoleCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] size_t live_fd_count() const noexcept { return live_state_->live; }
    [[nodiscard]] CStoreGuid c_store_guid() const noexcept { return c_store_guid_; }
    [[nodiscard]] FStoreGuid f_store_guid() const noexcept { return f_store_guid_; }
    [[nodiscard]] std::shared_ptr<RoleLiveState> live_counter_state() const noexcept {
        return live_state_;
    }

private:
    CStoreGuid c_store_guid_{};
    FStoreGuid f_store_guid_{};
    RoleLimits limits_{};
    RoleCounters counters_{};
    std::shared_ptr<RoleLiveState> live_state_ = std::make_shared<RoleLiveState>();
};

class ServerRoleOwner {
public:
    ServerRoleOwner(CStoreGuid c_store_guid, FStoreGuid f_store_guid,
                    RoleLimits limits = {});
    ~ServerRoleOwner();
    ServerRoleOwner(const ServerRoleOwner&) = delete;
    ServerRoleOwner& operator=(const ServerRoleOwner&) = delete;

    [[nodiscard]] bool namespace_matches(CStoreGuid c_store_guid,
                                         FStoreGuid f_store_guid) const noexcept;
    [[nodiscard]] bool admit_role(RoleDiscriminator role) noexcept;
    std::optional<RoleOwnedFd> adopt(RoleDiscriminator role, CStoreGuid c_store_guid,
                                      FStoreGuid f_store_guid, int fd) noexcept;
    [[nodiscard]] const RoleCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] size_t live_fd_count() const noexcept { return live_state_->live; }
    [[nodiscard]] CStoreGuid c_store_guid() const noexcept { return c_store_guid_; }
    [[nodiscard]] FStoreGuid f_store_guid() const noexcept { return f_store_guid_; }
    [[nodiscard]] std::shared_ptr<RoleLiveState> live_counter_state() const noexcept {
        return live_state_;
    }

private:
    CStoreGuid c_store_guid_{};
    FStoreGuid f_store_guid_{};
    RoleLimits limits_{};
    RoleCounters counters_{};
    std::shared_ptr<RoleLiveState> live_state_ = std::make_shared<RoleLiveState>();
};

} // namespace icecc::p50::role
