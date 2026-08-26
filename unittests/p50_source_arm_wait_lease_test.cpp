#include "p50_source_arm_wait_lease.h"

#include <cassert>
#include <unistd.h>

using icecc::p50::CStoreGuid;
using icecc::p50::FStoreGuid;
using icecc::p50::StoreIdentityRoot;
using icecc::p50::c_store_guid_for_root;
using icecc::p50::f_store_guid_for_root;

static icecc::p50::sidecar::ReadyLease valid_lease()
{
    StoreIdentityRoot root{};
    root.bytes.back() = 1;
    const CStoreGuid c_guid = c_store_guid_for_root(root);
    const FStoreGuid f_guid = f_store_guid_for_root(root);
    icecc::p50::sidecar::ReadyLease lease;
    lease.identity = {17, 3};
    lease.store_generation = 1;
    lease.pid = ::getpid();
    lease.store_root = root;
    lease.store_derivation_version = icecc::p50::kStoreIdentityDerivationVersion;
    lease.c_store_guid = c_guid;
    lease.f_store_guid = f_guid;
    lease.private_directory = "/tmp/p50-source-arm-wait-lease";
    lease.socket_path = lease.private_directory + "/cache.sock";
    lease.socket_path_digest = icecc::digest128(lease.socket_path);
    lease.listener_device = 1;
    lease.listener_inode = 2;
    lease.directory_device = 1;
    lease.directory_inode = 3;
    return lease;
}

int main()
{
    const auto owner = valid_lease();
    assert(owner.valid());
    assert(!icecc::p50::daemon::p50_wait_owner_replaced(owner, owner));

    auto replacement = owner;
    replacement.identity.attempt++;
    assert(replacement.valid());
    assert(icecc::p50::daemon::p50_wait_owner_replaced(owner, replacement));

    replacement = owner;
    replacement.store_generation++;
    assert(replacement.valid());
    assert(icecc::p50::daemon::p50_wait_owner_replaced(owner, replacement));

    replacement = owner;
    replacement.listener_inode++;
    assert(icecc::p50::daemon::p50_wait_owner_replaced(owner, replacement));

    replacement = owner;
    replacement.f_store_guid = {};
    replacement.f_store_guid.bytes[0] = icecc::p50::kStoreIdentityFileRole;
    assert(!replacement.valid());
    assert(icecc::p50::daemon::p50_wait_owner_replaced(owner, replacement));
    return 0;
}
