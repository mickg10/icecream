#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace icecc::p50 {

// Steady-state F sidecar traffic allocates and frees one fresh 8-26MB
// `std::vector<uint8_t>` per transaction (the materialized TU buffer, and
// the InputRecord backing derived from it). Under memory pressure on the
// worker host, munmap()/zap of those large anonymous mappings has been
// observed to stall for seconds under direct-compaction contention
// (DIAG-6: 6424ms to free 40.7MB on a pressured host). This pool lets that
// steady state reuse already-faulted capacity instead of allocating and
// freeing fresh pages every transaction.
//
// Deliberately small and dumb: a flat free list under one mutex, bounded by
// total bytes held. Buffers are kept at full capacity (never shrunk) and
// handed back via `clear()` (size 0, capacity unchanged) so a later
// `insert()`/`append()`-style fill never pays for zero-initialization.
//
// Thread-safety: get()/put() may be called concurrently from any thread
// (in particular, put() is designed to run from inside a shared_ptr custom
// deleter, which can fire on whatever thread drops the last reference --
// today that is usually the InputBackingReaper's background thread, but
// may be the owner thread inline).
class InputBufferPool {
public:
    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t puts_dropped = 0;
        uint64_t bytes_held = 0;
    };

    // `cap_bytes == 0` disables pooling entirely: get() always misses (empty
    // vector, no lock taken) and put() always frees its argument normally
    // (no lock taken) -- i.e. exactly today's pre-pool behavior, with no
    // added bookkeeping cost.
    explicit InputBufferPool(uint64_t cap_bytes) noexcept;

    InputBufferPool(const InputBufferPool&) = delete;
    InputBufferPool& operator=(const InputBufferPool&) = delete;

    // Process-wide instance, sized from ICECC_P50_F_INPUT_POOL_BYTES (see
    // .cpp for the exact parsing/logging rule). Returned as a shared_ptr so
    // that a custom shared_ptr deleter can capture it by value and keep the
    // pool alive for as long as that deleter might still fire, independent
    // of static-destruction order.
    static std::shared_ptr<InputBufferPool> instance();

    // Returns the smallest held buffer whose capacity() >= min_capacity, or
    // an empty vector (capacity 0) on a miss. On a hit, the returned vector
    // has size() == 0 and the same capacity it had when put() received it;
    // the caller is expected to insert()/append() into it, exactly as it
    // would after reserve() on a freshly constructed vector.
    [[nodiscard]] std::vector<std::uint8_t> get(std::size_t min_capacity);

    // Offers `buffer` back to the pool. If accepted (current bytes_held()
    // plus buffer.capacity() does not exceed the configured cap), the
    // buffer is cleared and kept; otherwise it is left to be freed by the
    // caller's destructor when this call returns (never under the mutex).
    void put(std::vector<std::uint8_t>&& buffer) noexcept;

    [[nodiscard]] bool enabled() const noexcept { return cap_bytes_ != 0; }
    [[nodiscard]] Stats stats() const noexcept;

    // Prints at most one `P50_F_INPUT_POOL hits=... misses=... bytes_held=...
    // puts_dropped=... wall_clock_ns=...` line to stderr per 10 seconds, and
    // only while enabled(). No-op otherwise. Safe to call on every get()/put().
    void maybe_report() noexcept;

private:
    const uint64_t cap_bytes_;
    mutable std::mutex mutex_;
    std::vector<std::vector<std::uint8_t>> free_list_;
    uint64_t held_bytes_ = 0;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
    uint64_t puts_dropped_ = 0;
    std::atomic<int64_t> last_report_ns_{0};
};

}  // namespace icecc::p50
