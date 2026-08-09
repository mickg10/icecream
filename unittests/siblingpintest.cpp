/* Deterministic regression for compatible-platform sibling pinning. */
#include "../scheduler/compileserver.h"
#include "../scheduler/job.h"
#include "../scheduler/siblingpin.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
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

static bool exact_environment(const Job &job, const char *platform, const char *environment)
{
    const Environments envs = job.environments();
    return envs.size() == 1 && envs.front().first == platform
           && envs.front().second == environment;
}

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
        CompileServer submitter(sockets[0], reinterpret_cast<struct sockaddr *>(&address),
                                sizeof(address), true);
        submitter.setNodeName("pin-test-submitter");

        Job master(1, &submitter);
        Job first(2, &submitter);
        Job second(3, &submitter);
        first.appendEnvironment(std::make_pair(std::string("x86_64"), std::string("wrong")));
        second.appendEnvironment(std::make_pair(std::string("i686"), std::string("old")));
        second.appendEnvironment(std::make_pair(std::string("x86_64"), std::string("wrong")));
        master.setSelectedEnvironment("gcc-env");
        master.appendJob(&first);
        master.appendJob(&second);

        pin_sibling_environments(&master,
                                 SiblingPinSelection{"i686", "gcc-env", "x86_64"});

        REQUIRE(exact_environment(first, "i686", "gcc-env"),
                "compatible platform, not worker platform, pins the first sibling");
        REQUIRE(exact_environment(second, "i686", "gcc-env"),
                "pinning replaces every prior sibling environment atomically");
        REQUIRE(submitter.submittedJobsCount() == 3,
                "pinning does not alter submitted-job accounting");

        Job no_selection(4, &submitter);
        Job untouched(5, &submitter);
        untouched.appendEnvironment(std::make_pair(std::string("i686"), std::string("keep")));
        no_selection.appendJob(&untouched);
        pin_sibling_environments(&no_selection,
                                 SiblingPinSelection{"i686", "", "x86_64"});
        REQUIRE(exact_environment(untouched, "i686", "keep"),
                "an incomplete selection cannot narrow sibling environments");
    }

    close(sockets[1]);
    if (failures) {
        fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}
