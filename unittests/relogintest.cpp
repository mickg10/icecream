/* Deterministic regression for scheduler relogin drain-loop ownership. */
#include "../scheduler/compileserver.h"
#include "../scheduler/relogin.h"

#include <cstdio>
#include <cstring>
#include <poll.h>
#include <signal.h>
#include <string>
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

static CompileServer *make_server(int fd, struct sockaddr_un *address)
{
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    CompileServer *server = new CompileServer(
        fd, reinterpret_cast<struct sockaddr *>(address), sizeof(*address), true);
    server->setNodeName("relogin-test-daemon");
    return server;
}

int main()
{
    signal(SIGPIPE, SIG_IGN);

    int sockets[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return 2;
    }

    struct sockaddr_un address;
    CompileServer *server = make_server(sockets[0], &address);
    server->protocol = PROTOCOL_VERSION;
    server->setBusyInstalling(1234);
    server->setCompilerVersions(Environments{
        std::make_pair(std::string("old-platform"), std::string("old-env"))});

    LoginMsg login(10245, "relogin-test-daemon", "x86_64", 0);
    login.envs.push_back(std::make_pair(std::string("i686"), std::string("new-env")));
    const ReloginResult result = apply_relogin(server, login);
    REQUIRE(result == ReloginResult::KeepConnection,
            "successful relogin keeps the daemon connection in the drain loop");
    REQUIRE(server->busyInstalling() == 0,
            "relogin clears the stale environment-installing state");
    REQUIRE(server->compilerVersions() == login.envs,
            "relogin atomically replaces the advertised compiler environments");

    struct pollfd readable = { sockets[1], POLLIN, 0 };
    REQUIRE(poll(&readable, 1, 1000) == 1,
            "modern relogin emits its configuration reply before returning keep");
    char buffer[128];
    REQUIRE(read(sockets[1], buffer, sizeof(buffer)) > 0,
            "configuration reply contains bytes on the daemon channel");

    delete server;
    close(sockets[1]);

    int legacy[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, legacy) != 0) {
        perror("socketpair legacy");
        return 2;
    }
    CompileServer *old_server = make_server(legacy[0], &address);
    old_server->protocol = 23;
    REQUIRE(apply_relogin(old_server, login) == ReloginResult::KeepConnection,
            "legacy relogin without ConfCS remains a successful keep transition");
    readable.fd = legacy[1];
    readable.events = POLLIN;
    readable.revents = 0;
    REQUIRE(poll(&readable, 1, 0) == 0,
            "pre-24 relogin does not emit an unsupported configuration reply");
    delete old_server;
    close(legacy[1]);

    int broken[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, broken) != 0) {
        perror("socketpair broken");
        return 2;
    }
    CompileServer *broken_server = make_server(broken[0], &address);
    broken_server->protocol = PROTOCOL_VERSION;
    close(broken[1]);
    REQUIRE(apply_relogin(broken_server, login) == ReloginResult::CloseConnection,
            "failed configuration delivery closes the relogin transition explicitly");
    delete broken_server;

    REQUIRE(apply_relogin(nullptr, login) == ReloginResult::CloseConnection,
            "a missing connection cannot be reported as a successful relogin");

    if (failures) {
        fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}
