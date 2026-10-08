#pragma once

#include "p50_input_record.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>

namespace icecc::p50 {

// Defers the destruction of InputRecordStore backing buffers (potentially
// many MB to GB per batch) off whatever thread collected them. The F
// sidecar's P50ServerEndpoint runs a single OWNER thread that must not block
// on a bulk free; this class gives it somewhere else to put the work.
//
// Usage: a caller on the owner thread builds a local InputBackingBatch (via
// InputRecordStore::collect_garbage(&batch) or similar) and calls submit().
// If accepted, the batch's eventual destruction (and therefore the actual
// deallocation) happens on the reaper's background thread. If the queue is
// full or the reaper has been stopped, submit() returns false and the
// caller is expected to let the batch be destroyed inline (synchronously),
// exactly as if this class did not exist: correctness over speed.
//
// Thread-safety: safe to call submit() from a single owner thread while the
// background thread drains concurrently. Not designed for concurrent
// submit() calls from multiple threads (the F sidecar has exactly one
// owner), though the mutex would make it safe if that ever changed.
class InputBackingReaper {
public:
    // `max_queued_batches` bounds memory the reaper can be asked to hold
    // before submit() starts refusing new batches (forcing inline frees).
    explicit InputBackingReaper(size_t max_queued_batches = 64);

    // Drains and joins the background thread, freeing anything still queued
    // on this (destructor) thread. This is the "shutdown drains everything"
    // guarantee; no explicit stop() call is required by owners.
    ~InputBackingReaper();

    InputBackingReaper(const InputBackingReaper&) = delete;
    InputBackingReaper& operator=(const InputBackingReaper&) = delete;

    // Attempts to hand `batch` to the background thread. Returns true if
    // accepted (the batch is moved-from and will be destroyed on the
    // reaper thread); returns false if the queue was full or the reaper is
    // stopped/stopping, in which case `batch` is left untouched so the
    // caller can free it inline.
    bool submit(InputBackingBatch&& batch);

    // Diagnostics only; not synchronized against the background thread's
    // own counters beyond what the mutex incidentally provides, so treat as
    // approximate/instantaneous when read concurrently.
    [[nodiscard]] size_t queued_batches() const;

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<InputBackingBatch> queue_;
    size_t max_queued_batches_;
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace icecc::p50
