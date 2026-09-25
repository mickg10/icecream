/*
    Per-turn poll readiness index.

    The daemon loop looks up revents through one index per poll snapshot
    instead of rescanning the snapshot per client.  first()/is_set() must
    agree with pollfd_is_set() (the first entry for a descriptor decides) and
    any() must OR every entry for a descriptor, including duplicates and
    negative descriptors.
*/

#include "daemon/poll_readiness.h"
#include "services/util.h"

#include <poll.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using icecc::daemon_poll::PollReadiness;

int failures = 0;

void check(bool condition, const char *what)
{
    std::printf(condition ? "ok       - %s\n" : "FAILED   - %s\n", what);
    if (!condition) {
        ++failures;
    }
}

short linear_any(const std::vector<pollfd>& pollfds, int fd)
{
    short revents = 0;
    for (const pollfd& descriptor : pollfds) {
        if (descriptor.fd == fd) {
            revents |= descriptor.revents;
        }
    }
    return revents;
}

short linear_first(const std::vector<pollfd>& pollfds, int fd)
{
    for (const pollfd& descriptor : pollfds) {
        if (descriptor.fd == fd) {
            return descriptor.revents;
        }
    }
    return 0;
}

bool matches_linear_scans(const std::vector<pollfd>& pollfds, int low_fd, int high_fd)
{
    static const int flag_sets[] = {POLLIN, POLLOUT, POLLIN | POLLHUP | POLLERR,
                                    POLLIN | POLLOUT};
    const PollReadiness ready(pollfds);
    for (int fd = low_fd; fd <= high_fd; ++fd) {
        if (ready.first(fd) != linear_first(pollfds, fd) ||
            ready.any(fd) != linear_any(pollfds, fd)) {
            return false;
        }
        for (const int flags : flag_sets) {
            if (ready.is_set(fd, flags) != pollfd_is_set(pollfds, fd, flags) ||
                ready.is_set(fd, flags, false) != pollfd_is_set(pollfds, fd, flags, false)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main()
{
    const std::vector<pollfd> snapshot{
        {5, POLLIN, POLLIN},
        {3, POLLIN, 0},
        {5, POLLOUT, POLLHUP},
        {-1, POLLIN, 0},
        {7, POLLIN, POLLERR},
        {3, POLLOUT, POLLOUT},
        {-1, POLLIN, POLLNVAL},
    };
    const PollReadiness ready(snapshot);

    check(ready.first(5) == POLLIN && ready.any(5) == (POLLIN | POLLHUP),
          "duplicate descriptor: first() keeps the first entry, any() ORs all");
    check(!ready.is_set(3, POLLOUT) && ready.any(3) == POLLOUT,
          "a later duplicate cannot satisfy is_set() once the first entry is quiet");
    check(ready.is_set(7, POLLIN) && !ready.is_set(7, POLLIN, false),
          "POLLERR satisfies is_set() only when error checking is requested");
    check(ready.first(9) == 0 && ready.any(9) == 0 && !ready.is_set(9, POLLIN),
          "an unregistered descriptor has no readiness");
    check(ready.first(-1) == 0 && ready.any(-1) == POLLNVAL,
          "negative descriptors follow the same first/any rules as linear scans");
    check(matches_linear_scans(snapshot, -2, 10),
          "fixed snapshot agrees with pollfd_is_set() and OR scans");

    check(matches_linear_scans({}, -2, 4), "an empty snapshot agrees with the linear scans");

    static const short revent_values[] = {0, POLLIN, POLLOUT, POLLHUP, POLLERR, POLLNVAL,
                                          POLLIN | POLLHUP, POLLOUT | POLLERR};
    uint32_t state = 12345;
    bool random_agree = true;
    for (int round = 0; round != 200 && random_agree; ++round) {
        std::vector<pollfd> pollfds;
        const size_t count = 1 + round % 40;
        for (size_t index = 0; index != count; ++index) {
            state = state * 1103515245u + 12345u;
            const int fd = static_cast<int>((state >> 16) % 24) - 2;
            state = state * 1103515245u + 12345u;
            const short revents = revent_values[(state >> 16) % 8];
            pollfds.push_back(pollfd{fd, POLLIN, revents});
        }
        random_agree = matches_linear_scans(pollfds, -3, 23);
    }
    check(random_agree, "random snapshots with duplicates agree with the linear scans");

    return failures == 0 ? 0 : 1;
}
