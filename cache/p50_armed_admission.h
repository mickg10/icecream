/*
    This file is part of Icecream.

    Copyright (c) 2026 the Icecream maintainers

    Admission to a route's armed-session window, unit-tested in
    unittests/armedadmissiontest.cpp.

    Icecream is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef ICECREAM_P50_ARMED_ADMISSION_H
#define ICECREAM_P50_ARMED_ADMISSION_H

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>

namespace icecc::p50::service {

/* Waiters take free slots in order of arrival less a head start that grows
   with the source's size, up to kMaxHeadStart at kHeadStartBytes.  On a
   saturated far F, a large TU (a long compile) that queued behind hundreds of
   small ones is admitted first instead of whenever it wins a polling race,
   and no waiter is overtaken by anything that arrived more than kMaxHeadStart
   after it.  That bounds the reordering only: a waiter still waits for every
   slot ahead of it, and the caller's own deadline and stop checks end a wait
   that the queue does not.  */
class ArmedAdmission {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr uint64_t kHeadStartBytes = uint64_t(64) << 20;
    static constexpr std::chrono::nanoseconds kMaxHeadStart = std::chrono::seconds(8);

    explicit ArmedAdmission(std::ptrdiff_t slots) : free_(slots) {}

    // One upload's place in the queue, then its slot: the destructor releases
    // a held slot, or leaves the queue.
    class Wait {
    public:
        Wait(ArmedAdmission &gate, uint64_t source_bytes, Clock::time_point arrival)
            : gate_(gate)
        {
            const uint64_t head = std::min(source_bytes, kHeadStartBytes) *
                                  uint64_t(kMaxHeadStart.count()) / kHeadStartBytes;
            std::lock_guard lock(gate_.mutex_);
            key_ = {std::chrono::duration_cast<std::chrono::nanoseconds>(
                        arrival.time_since_epoch()).count() - int64_t(head),
                    gate_.sequence_++};
            gate_.waiting_.emplace(key_, &turn_);
        }
        ~Wait()
        {
            std::lock_guard lock(gate_.mutex_);
            if (held_) {
                ++gate_.free_;
            } else {
                gate_.waiting_.erase(key_);
            }
            gate_.wake_head();
        }
        Wait(const Wait &) = delete;
        Wait &operator=(const Wait &) = delete;

        bool acquire_until(Clock::time_point limit)
        {
            std::unique_lock lock(gate_.mutex_);
            if (held_) {
                return true;
            }
            if (!turn_.wait_until(lock, limit, [this] {
                    return gate_.free_ > 0 && gate_.waiting_.begin()->first == key_;
                })) {
                return false;
            }
            gate_.waiting_.erase(key_);
            --gate_.free_;
            held_ = true;
            gate_.wake_head();
            return true;
        }

    private:
        ArmedAdmission &gate_;
        std::pair<int64_t, uint64_t> key_;
        std::condition_variable turn_;
        bool held_ = false;
    };

private:
    void wake_head()
    {
        if (free_ > 0 && !waiting_.empty()) {
            waiting_.begin()->second->notify_one();
        }
    }

    std::mutex mutex_;
    std::ptrdiff_t free_;
    uint64_t sequence_ = 0;
    std::map<std::pair<int64_t, uint64_t>, std::condition_variable *> waiting_;
};

} // namespace icecc::p50::service

#endif
