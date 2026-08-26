#include "p50_store_identity_wire.h"

#include <array>
#include <cstdint>

int main()
{
    using Guid = std::array<uint8_t, 16>;
    Guid zero{};
    Guid f_role_only{};
    f_role_only[icecc::p50::kStoreIdentityRoleByte] =
        icecc::p50::kStoreIdentityFileRole;
    if (icecc::p50::store_identity_guid_valid_for_role(
            zero, icecc::p50::kStoreIdentityClientRole) ||
        icecc::p50::store_identity_guid_valid_for_role(
            f_role_only, icecc::p50::kStoreIdentityFileRole))
        return 1;

    Guid c = zero;
    c.back() = 1;
    Guid f = c;
    f[icecc::p50::kStoreIdentityRoleByte] =
        icecc::p50::kStoreIdentityFileRole;
    return icecc::p50::store_identity_guid_valid_for_role(
               c, icecc::p50::kStoreIdentityClientRole) &&
           icecc::p50::store_identity_guid_valid_for_role(
               f, icecc::p50::kStoreIdentityFileRole)
        ? 0 : 1;
}
