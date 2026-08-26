#pragma once

#include "p50_sidecar_supervisor.h"

namespace icecc::p50::daemon {

// Dependency-light production seam for the F WAIT owner boundary.  A WAIT
// owner is retained only while every accepted READY observation remains the
// same sidecar/listener incarnation; changing any launch, store, path, or
// listener identity invalidates it before a replacement is advertised.
inline bool p50_ready_lease_observation_equal(
    const sidecar::ReadyLease& left,
    const sidecar::ReadyLease& right) noexcept
{
    return left.valid() && right.valid() &&
           left.identity == right.identity && left.pid == right.pid &&
           left.store_generation == right.store_generation &&
           left.store_root == right.store_root &&
           left.store_derivation_version == right.store_derivation_version &&
           left.c_store_guid == right.c_store_guid &&
           left.f_store_guid == right.f_store_guid &&
           left.private_directory == right.private_directory &&
           left.socket_path == right.socket_path &&
           left.socket_path_digest == right.socket_path_digest &&
           left.listener_device == right.listener_device &&
           left.listener_inode == right.listener_inode &&
           left.directory_device == right.directory_device &&
           left.directory_inode == right.directory_inode;
}

inline bool p50_wait_owner_replaced(
    const sidecar::ReadyLease& owner,
    const sidecar::ReadyLease& current) noexcept
{
    return !p50_ready_lease_observation_equal(owner, current);
}

} // namespace icecc::p50::daemon
