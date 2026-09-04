/* -*- mode: c++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/*
    Select daemon work by priority and then arrival identity.

    The Clients container is keyed by MsgChannel pointer, so iteration order
    is allocation order rather than queue order.  Keep the ordering rule in a
    pure helper so every queued status uses the same explicit comparison and
    the fairness contract can be tested without starting iceccd.
*/

#ifndef ICECREAM_CLIENT_QUEUE_ORDER_H
#define ICECREAM_CLIENT_QUEUE_ORDER_H

namespace icecc {
namespace daemon_queue {

template <typename ClientMap, typename Status>
typename ClientMap::mapped_type select_earliest(const ClientMap& clients,
                                                Status wanted_status)
{
    typename ClientMap::mapped_type selected = nullptr;

    for (const auto& entry : clients) {
        const auto candidate = entry.second;
        if (candidate->status != wanted_status) {
            continue;
        }
        if (selected == nullptr
                || candidate->niceness < selected->niceness
                || (candidate->niceness == selected->niceness
                    && candidate->client_id < selected->client_id)) {
            selected = candidate;
        }
    }

    return selected;
}

} // namespace daemon_queue
} // namespace icecc

#endif // ICECREAM_CLIENT_QUEUE_ORDER_H
