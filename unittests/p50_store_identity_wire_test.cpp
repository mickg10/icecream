#include "services/p50_store_identity_wire.h"

#include <array>
#include <cstdio>

using namespace icecc::p50;

namespace {

int failures = 0;

#define CHECK(condition, message)                                               \
  do {                                                                          \
    if (!(condition)) {                                                          \
      std::fprintf(stderr, "FAIL: %s\n", message);                             \
      ++failures;                                                               \
    }                                                                            \
  } while (false)

std::array<uint8_t, 16> client_guid(uint8_t seed) {
  std::array<uint8_t, 16> guid{};
  for (size_t i = 0; i != guid.size(); ++i)
    guid[i] = static_cast<uint8_t>(seed + i);
  guid[kStoreIdentityRoleByte] &=
      static_cast<uint8_t>(~kStoreIdentityRoleMask);
  return guid;
}

} // namespace

int main() {
  const auto client = client_guid(0x20);
  auto file = client;
  file[kStoreIdentityRoleByte] |= kStoreIdentityFileRole;
  CHECK(store_identity_guid_valid_for_role(client, kStoreIdentityClientRole),
        "nonzero client root is valid");
  CHECK(store_identity_guid_valid_for_role(file, kStoreIdentityFileRole),
        "nonzero file root is valid");
  CHECK(store_identity_file_guid_matches_client(client, file),
        "role projections of one root match");

  std::array<uint8_t, 16> zero_client{};
  std::array<uint8_t, 16> role_only_file{};
  role_only_file[kStoreIdentityRoleByte] = kStoreIdentityFileRole;
  CHECK(!store_identity_guid_valid_for_role(zero_client,
                                             kStoreIdentityClientRole),
        "all-zero client root is invalid");
  CHECK(!store_identity_guid_valid_for_role(role_only_file,
                                             kStoreIdentityFileRole),
        "role-tag-only file GUID does not manufacture a nonzero root");
  CHECK(!store_identity_file_guid_matches_client(zero_client, role_only_file),
        "two zero roots cannot form an identity pair");

  auto independent = file;
  independent[7] ^= 1;
  CHECK(store_identity_guid_valid_for_role(independent,
                                            kStoreIdentityFileRole),
        "independent nonzero file root remains valid");
  CHECK(!store_identity_file_guid_matches_client(client, independent),
        "independent roots never match");
  CHECK(!store_identity_guid_valid_for_role(client, 0x40),
        "unknown expected role is rejected");
  return failures == 0 ? 0 : 1;
}
