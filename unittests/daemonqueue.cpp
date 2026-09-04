/*
    Daemon queue ordering regression.

    Clients are stored by channel pointer, which is unrelated to arrival
    order.  Selection must therefore be lexicographic by (niceness,
    client_id): lower niceness first, FIFO within equal niceness.
*/

#include "daemon/client_queue_order.h"

#include <cstdint>
#include <cstdio>
#include <map>

namespace {

int failures = 0;

void check(bool condition, const char *what)
{
    std::printf(condition ? "ok       - %s\n" : "FAILED   - %s\n", what);
    if (!condition) {
        ++failures;
    }
}

struct FakeClient {
    enum Status { OTHER, TOCOMPILE } status;
    int client_id;
    std::uint32_t niceness;
};

using FakeClients = std::map<std::uintptr_t, FakeClient *>;

FakeClient *select(const FakeClients& clients)
{
    return icecc::daemon_queue::select_earliest(clients, FakeClient::TOCOMPILE);
}

} // namespace

int main()
{
    FakeClient oldest{FakeClient::TOCOMPILE, 10, 7};
    FakeClient newer{FakeClient::TOCOMPILE, 20, 7};
    FakeClient newest{FakeClient::TOCOMPILE, 30, 7};

    /* Adverse channel order: the newest client is visited first. */
    FakeClients equal_priority{{1, &newest}, {2, &newer}, {3, &oldest}};
    check(select(equal_priority) == &oldest,
          "equal-priority clients are FIFO by client id, not channel-map order");

    /* Churn can move a newer allocation ahead of every retained channel. */
    equal_priority.erase(1);
    equal_priority.emplace(0, &newest);
    check(select(equal_priority) == &oldest,
          "channel churn cannot let a newer equal-priority client overtake");

    FakeClient low_priority_old{FakeClient::TOCOMPILE, 1, 20};
    FakeClient high_priority_new{FakeClient::TOCOMPILE, 99, 3};
    FakeClients mixed_priority{{1, &low_priority_old}, {2, &high_priority_new}};
    check(select(mixed_priority) == &high_priority_new,
          "niceness outranks client id across priority classes");

    FakeClient ignored{FakeClient::OTHER, 0, 0};
    mixed_priority.emplace(0, &ignored);
    check(select(mixed_priority) == &high_priority_new,
          "clients in another status do not influence queue selection");

    FakeClients empty;
    check(select(empty) == nullptr, "an empty eligible queue selects no client");

    return failures == 0 ? 0 : 1;
}
