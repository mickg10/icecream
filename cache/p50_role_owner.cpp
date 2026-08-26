#include "p50_role_owner.h"

#include <unistd.h>

namespace icecc::p50::role {
namespace {

bool valid_namespace(CStoreGuid c_store_guid, FStoreGuid f_store_guid) noexcept {
    return c_store_guid != CStoreGuid{} && f_store_guid != FStoreGuid{} &&
           c_store_guid != f_store_guid;
}

template <typename Owner>
std::optional<RoleOwnedFd> adopt_impl(Owner& owner, RoleDiscriminator expected,
                                      RoleDiscriminator role, CStoreGuid c_store_guid,
                                      FStoreGuid f_store_guid, int fd) noexcept {
    if (fd < 0)
        return std::nullopt;
    if (role != expected || !owner.namespace_matches(c_store_guid, f_store_guid) ||
        !owner.admit_role(role)) {
        (void)::close(fd);
        return std::nullopt;
    }
    return RoleOwnedFd{role, fd, owner.live_counter_ptr()};
}

} // namespace

RoleOwnedFd::~RoleOwnedFd() {
    if (fd >= 0)
        (void)::close(fd);
    if (live_counter != nullptr && *live_counter != 0)
        --*live_counter;
}

RoleOwnedFd::RoleOwnedFd(RoleOwnedFd&& other) noexcept
    : role(other.role), fd(other.fd), live_counter(other.live_counter) {
    other.fd = -1;
    other.live_counter = nullptr;
}

RoleOwnedFd& RoleOwnedFd::operator=(RoleOwnedFd&& other) noexcept {
    if (this != &other) {
        if (fd >= 0)
            (void)::close(fd);
        if (live_counter != nullptr && *live_counter != 0)
            --*live_counter;
        role = other.role;
        fd = other.fd;
        live_counter = other.live_counter;
        other.fd = -1;
        other.live_counter = nullptr;
    }
    return *this;
}

int RoleOwnedFd::release() noexcept {
    const int result = fd;
    fd = -1;
    return result;
}

ClientRoleOwner::ClientRoleOwner(CStoreGuid c_store_guid, FStoreGuid f_store_guid,
                                 RoleLimits limits) noexcept
    : c_store_guid_(c_store_guid), f_store_guid_(f_store_guid), limits_(limits) {}

ClientRoleOwner::~ClientRoleOwner() = default;

bool ClientRoleOwner::namespace_matches(CStoreGuid c_store_guid,
                                        FStoreGuid f_store_guid) const noexcept {
    return valid_namespace(c_store_guid_, f_store_guid_) &&
           c_store_guid == c_store_guid_ && f_store_guid == f_store_guid_;
}

bool ClientRoleOwner::admit_role(RoleDiscriminator role) noexcept {
    if (role != RoleDiscriminator::Client) {
        ++counters_.rejected_wrong_role;
        return false;
    }
    if (live_fds_ >= limits_.max_live_fds) {
        ++counters_.rejected_limit;
        return false;
    }
    ++live_fds_;
    ++counters_.accepted;
    return true;
}

std::optional<RoleOwnedFd> ClientRoleOwner::adopt(RoleDiscriminator role,
                                                  CStoreGuid c_store_guid,
                                                  FStoreGuid f_store_guid,
                                                  int fd) noexcept {
    if (role != RoleDiscriminator::Client)
        ++counters_.rejected_wrong_role;
    else if (!namespace_matches(c_store_guid, f_store_guid))
        ++counters_.rejected_namespace;
    return adopt_impl(*this, RoleDiscriminator::Client, role, c_store_guid,
                      f_store_guid, fd);
}

ServerRoleOwner::ServerRoleOwner(CStoreGuid c_store_guid, FStoreGuid f_store_guid,
                                 RoleLimits limits) noexcept
    : c_store_guid_(c_store_guid), f_store_guid_(f_store_guid), limits_(limits) {}

ServerRoleOwner::~ServerRoleOwner() = default;

bool ServerRoleOwner::namespace_matches(CStoreGuid c_store_guid,
                                        FStoreGuid f_store_guid) const noexcept {
    return valid_namespace(c_store_guid_, f_store_guid_) &&
           c_store_guid == c_store_guid_ && f_store_guid == f_store_guid_;
}

bool ServerRoleOwner::admit_role(RoleDiscriminator role) noexcept {
    if (role != RoleDiscriminator::Server) {
        ++counters_.rejected_wrong_role;
        return false;
    }
    if (live_fds_ >= limits_.max_live_fds) {
        ++counters_.rejected_limit;
        return false;
    }
    ++live_fds_;
    ++counters_.accepted;
    return true;
}

std::optional<RoleOwnedFd> ServerRoleOwner::adopt(RoleDiscriminator role,
                                                  CStoreGuid c_store_guid,
                                                  FStoreGuid f_store_guid,
                                                  int fd) noexcept {
    if (role != RoleDiscriminator::Server)
        ++counters_.rejected_wrong_role;
    else if (!namespace_matches(c_store_guid, f_store_guid))
        ++counters_.rejected_namespace;
    return adopt_impl(*this, RoleDiscriminator::Server, role, c_store_guid,
                      f_store_guid, fd);
}

} // namespace icecc::p50::role
