/* Deterministic regression for StatsMsg application at the scheduler. */
#include "../scheduler/compileserver.h"

#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int failures = 0;

#define REQUIRE(cond, what)                                                     \
    do {                                                                        \
        if (cond) {                                                             \
            fprintf(stderr, "ok       - %s\n", what);                         \
        } else {                                                                \
            fprintf(stderr, "FAILED   - %s (at %s:%d)\n", what, __FILE__,    \
                    __LINE__);                                                   \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

int main()
{
    int sockets[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return 2;
    }

    {
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        CompileServer server(sockets[0], reinterpret_cast<struct sockaddr *>(&address),
                             sizeof(address), true);
        server.setNodeName("stats-test-daemon");
        server.setClientCount(37);   // last authoritative lifecycle message
        server.setLoad(999);

        StatsMsg heartbeat;
        heartbeat.load = 123;
        REQUIRE(heartbeat.client_count == 0,
                "an ordinary StatsMsg starts with an unserialized zero client count");
        server.applyStats(heartbeat);
        REQUIRE(server.load() == 123, "serialized load is applied from StatsMsg");
        REQUIRE(server.clientCount() == 37,
                "StatsMsg cannot overwrite the authoritative lifecycle client count");

        heartbeat.load = 456;
        heartbeat.client_count = 999;  // even a local value is not a wire contract
        server.applyStats(heartbeat);
        REQUIRE(server.load() == 456, "later serialized load updates remain effective");
        REQUIRE(server.clientCount() == 37,
                "unnegotiated client_count is ignored regardless of its local value");
    }

    close(sockets[1]);
    if (failures) {
        fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}
