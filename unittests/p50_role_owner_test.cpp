#include "../cache/p50_role_owner.h"
#include "../cache/p50_incarnation_identity.h"

#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

using namespace icecc::p50;
using namespace icecc::p50::role;

namespace {
void check(bool condition, const char* expression) {
    if (!condition)
        throw std::runtime_error(expression);
}
#define CHECK(expression) check((expression), #expression)
}

int main() {
    StoreIdentityRoot root{};
    root.bytes[15] = 4;
    const CStoreGuid c = c_store_guid_for_root(root);
    const FStoreGuid f = f_store_guid_for_root(root);
    CHECK(c != CStoreGuid{} && f != FStoreGuid{} && c != f);
    CHECK((c.bytes[0] & kStoreIdentityRoleBit) == 0);
    CHECK((f.bytes[0] & kStoreIdentityRoleBit) != 0);
    StoreIdentityRoot other_root{};
    other_root.bytes[15] = 5;
    CHECK(c_store_guid_for_root(other_root) != c);

    ClientRoleOwner client(c, f, RoleLimits{1});
    ServerRoleOwner server(c, f, RoleLimits{1});
    int client_pair[2] = {-1, -1};
    int server_pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, client_pair) == 0);
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, server_pair) == 0);
    auto client_fd = client.adopt(RoleDiscriminator::Client, c, f, client_pair[0]);
    auto server_fd = server.adopt(RoleDiscriminator::Server, c, f, server_pair[0]);
    CHECK(client_fd.has_value() && server_fd.has_value());
    CHECK(client.counters().accepted == 1 && server.counters().accepted == 1);
    CHECK(client.live_fd_count() == 1 && server.live_fd_count() == 1);
    client_fd.reset();
    server_fd.reset();
    CHECK(client.live_fd_count() == 0 && server.live_fd_count() == 0);
    (void)::close(client_pair[1]);
    (void)::close(server_pair[1]);

    int released_pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, released_pair) == 0);
    auto released_owner =
        client.adopt(RoleDiscriminator::Client, c, f, released_pair[0]);
    CHECK(released_owner.has_value() && client.live_fd_count() == 1);
    const int transferred = released_owner->release();
    CHECK(transferred >= 0 && !released_owner->valid() && client.live_fd_count() == 0);
    CHECK(::close(transferred) == 0);
    CHECK(::close(released_pair[1]) == 0);

    // The descriptor may outlive its admitting owner.  Its shared accounting
    // state must remain valid until the descriptor wrapper is destroyed.
    std::optional<RoleOwnedFd> surviving;
    int surviving_pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, surviving_pair) == 0);
    {
        ClientRoleOwner scoped_client(c, f, RoleLimits{1});
        surviving = scoped_client.adopt(RoleDiscriminator::Client, c, f,
                                        surviving_pair[0]);
        CHECK(surviving.has_value() && scoped_client.live_fd_count() == 1);
    }
    CHECK(surviving->valid());
    surviving.reset();
    CHECK(::close(surviving_pair[1]) == 0);

    int wrong_pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wrong_pair) == 0);
    CHECK(!server.adopt(RoleDiscriminator::Client, c, f, wrong_pair[0]).has_value());
    CHECK(server.counters().rejected_wrong_role == 1);
    CHECK(::fcntl(wrong_pair[0], F_GETFD) < 0);
    (void)::close(wrong_pair[1]);

    int namespace_pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, namespace_pair) == 0);
    const CStoreGuid other_c = c_store_guid_for_root(other_root);
    CHECK(!client.adopt(RoleDiscriminator::Client, other_c, f, namespace_pair[0]).has_value());
    CHECK(client.counters().rejected_namespace == 1);
    (void)::close(namespace_pair[1]);
    return 0;
}
