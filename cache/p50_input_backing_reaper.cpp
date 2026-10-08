#include "p50_input_backing_reaper.h"

#include <chrono>
#include <cstdio>
#include <utility>

namespace icecc::p50 {

InputBackingReaper::InputBackingReaper(size_t max_queued_batches)
    : max_queued_batches_(max_queued_batches) {
    worker_ = std::thread(&InputBackingReaper::run, this);
}

InputBackingReaper::~InputBackingReaper() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    not_empty_.notify_all();
    if (worker_.joinable())
        worker_.join();
    // Anything left in the queue (there shouldn't be, after join) is freed
    // here, inline, on the destructing thread -- still correct, just not
    // deferred.
    queue_.clear();
}

bool InputBackingReaper::submit(InputBackingBatch&& batch) {
    if (batch.empty())
        return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_ || queue_.size() >= max_queued_batches_)
        return false;
    queue_.push_back(std::move(batch));
    not_empty_.notify_one();
    return true;
}

size_t InputBackingReaper::queued_batches() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void InputBackingReaper::run() {
    for (;;) {
        InputBackingBatch batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            not_empty_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stop_)
                    return;
                continue;
            }
            batch = std::move(queue_.front());
            queue_.pop_front();
        }
        // Everything from here down runs with the batch owned purely by
        // this stack frame; the owner thread that submitted it has already
        // moved on. Destroying `batch` at the end of this scope is where
        // the deferred free actually happens.
        const uint64_t freed_bytes = input_backing_batch_bytes(batch);
        const auto start = std::chrono::steady_clock::now();
        batch.clear();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count();
        if (elapsed_ms > 200) {
            const auto elapsed_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                    .count();
            std::fprintf(stderr,
                         "cache sidecar input reaper freed %llu in %lldms"
                         " wall_clock_ns=%lld\n",
                         static_cast<unsigned long long>(freed_bytes),
                         static_cast<long long>(elapsed_ms),
                         static_cast<long long>(elapsed_ns));
        }
    }
}

}  // namespace icecc::p50
