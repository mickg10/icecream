/* -*- mode: c++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/*
    Per-turn fd -> revents index over one poll() snapshot.

    first() keeps pollfd_is_set() semantics: the first entry registered for a
    descriptor decides.  any() ORs every entry registered for it.  Lookups
    are O(log P), so one daemon turn over N clients no longer rescans the
    whole snapshot for every client.
*/

#ifndef ICECREAM_POLL_READINESS_H
#define ICECREAM_POLL_READINESS_H

#include <poll.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace icecc {
namespace daemon_poll {

class PollReadiness
{
public:
    explicit PollReadiness(const std::vector<pollfd>& pollfds)
    {
        entries_.reserve(pollfds.size());
        for (const pollfd& descriptor : pollfds) {
            entries_.push_back(Entry{descriptor.fd, descriptor.revents, descriptor.revents});
        }
        std::stable_sort(entries_.begin(), entries_.end(),
                         [](const Entry& left, const Entry& right) {
                             return left.fd < right.fd;
                         });
        size_t kept = 0;
        for (size_t index = 0; index != entries_.size(); ++index) {
            if (kept != 0 && entries_[kept - 1].fd == entries_[index].fd) {
                entries_[kept - 1].any |= entries_[index].first;
            } else {
                entries_[kept++] = entries_[index];
            }
        }
        entries_.resize(kept);
    }

    short first(int fd) const
    {
        const Entry *entry = find(fd);
        return entry != nullptr ? entry->first : short(0);
    }

    short any(int fd) const
    {
        const Entry *entry = find(fd);
        return entry != nullptr ? entry->any : short(0);
    }

    bool is_set(int fd, int flags, bool check_errors = true) const
    {
        const short revents = first(fd);
        if (revents & flags) {
            return true;
        }
        return check_errors && (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
    }

private:
    struct Entry {
        int fd;
        short first;
        short any;
    };

    const Entry *find(int fd) const
    {
        const auto found = std::lower_bound(entries_.begin(), entries_.end(), fd,
                                            [](const Entry& entry, int wanted) {
                                                return entry.fd < wanted;
                                            });
        return found != entries_.end() && found->fd == fd ? &*found : nullptr;
    }

    std::vector<Entry> entries_;
};

} // namespace daemon_poll
} // namespace icecc

#endif // ICECREAM_POLL_READINESS_H
