#include "p50_input_buffer_pool.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace icecc::p50 {
namespace {

constexpr uint64_t kDefaultCapBytes = 2ULL * 1024 * 1024 * 1024;  // 2 GiB
constexpr int64_t kReportIntervalNs = 10LL * 1000 * 1000 * 1000;  // 10s

// Pre-sized so a put() -- including the common "discard a duplicate
// publish's backing" path reached from InputRecordStore::commit_prepared(),
// which InputRecordStore's own test suite asserts is allocation-free (see
// test_prepared_publish_is_exact_one_shot_and_allocation_free) -- does not
// have to grow free_list_'s own storage on the common case. This bounds the
// *bookkeeping* vector-of-vectors, not the buffers it holds (those stay
// governed solely by cap_bytes); 64 free-list slots is already far more
// concurrent held buffers than one F sidecar's in-flight TU count realistically
// reaches before cap_bytes starts rejecting puts via puts_dropped_. If actual
// concurrency ever exceeds this, free_list_ still grows correctly -- just
// loses the allocation-free property for that one put(), same as any other
// vector headroom choice.
constexpr std::size_t kFreeListReserve = 64;

int64_t monotonic_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Unlike this codebase's prevailing env-parsing convention (silent
// fallback to default on malformed input -- see armed_window_slots(),
// route_lane_count(), the interner's stats_period()), a malformed or
// out-of-range ICECC_P50_F_INPUT_POOL_BYTES is deliberately logged here:
// a typo in this particular knob silently reverting to "pooling enabled
// at the default cap" is a materially different outcome (steady-state
// memory footprint) from the other knobs' silent fallbacks, and is worth
// one stderr line to catch during rollout.
uint64_t resolve_cap_bytes() noexcept {
    const char* raw = std::getenv("ICECC_P50_F_INPUT_POOL_BYTES");
    if (raw == nullptr || *raw == '\0')
        return kDefaultCapBytes;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (end == raw || *end != '\0' || errno == ERANGE) {
        std::fprintf(stderr,
                     "P50_F_INPUT_POOL: malformed ICECC_P50_F_INPUT_POOL_BYTES"
                     " value %s, keeping default %llu bytes\n",
                     raw, static_cast<unsigned long long>(kDefaultCapBytes));
        return kDefaultCapBytes;
    }
    return static_cast<uint64_t>(parsed);
}

}  // namespace

InputBufferPool::InputBufferPool(uint64_t cap_bytes) noexcept
    : cap_bytes_(cap_bytes) {
    // Only pay for the reserve when pooling is actually enabled -- a
    // disabled pool (cap_bytes == 0) must stay a pure no-op on both get()
    // and put(), per the class comment and
    // test_zero_cap_disables_pooling().
    if (cap_bytes_ != 0) free_list_.reserve(kFreeListReserve);
}

std::shared_ptr<InputBufferPool> InputBufferPool::instance() {
    static const std::shared_ptr<InputBufferPool> singleton =
        std::make_shared<InputBufferPool>(resolve_cap_bytes());
    return singleton;
}

std::vector<std::uint8_t> InputBufferPool::get(std::size_t min_capacity) {
    if (!enabled())
        return {};

    std::vector<std::uint8_t> result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto best = free_list_.end();
        for (auto it = free_list_.begin(); it != free_list_.end(); ++it) {
            if (it->capacity() < min_capacity)
                continue;
            if (best == free_list_.end() ||
                it->capacity() < best->capacity())
                best = it;
        }
        if (best != free_list_.end()) {
            result = std::move(*best);
            held_bytes_ -= result.capacity();
            free_list_.erase(best);
            ++hits_;
        } else {
            ++misses_;
        }
    }
    return result;
}

void InputBufferPool::put(std::vector<std::uint8_t>&& buffer) noexcept {
    // Always take local ownership first: whatever path we take below, the
    // buffer (or whatever remains of it after a move-in) is freed by this
    // function's return, never left dangling.
    std::vector<std::uint8_t> local = std::move(buffer);
    if (!enabled() || local.capacity() == 0)
        return;  // Disabled, or nothing worth keeping: `local` frees normally.

    const uint64_t capacity = static_cast<uint64_t>(local.capacity());
    local.clear();  // size() -> 0, capacity() unchanged, no reallocation.

    std::lock_guard<std::mutex> lock(mutex_);
    if (held_bytes_ + capacity <= cap_bytes_) {
        held_bytes_ += capacity;
        free_list_.push_back(std::move(local));
    } else {
        ++puts_dropped_;
        // `local` is destroyed (freeing its backing storage) once this
        // scope exits, after the mutex is released -- the lock itself is
        // dropped by the lock_guard's destructor at the same point, so no
        // free ever happens while the mutex is held.
    }
}

InputBufferPool::Stats InputBufferPool::stats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return Stats{hits_, misses_, puts_dropped_, held_bytes_};
}

void InputBufferPool::maybe_report() noexcept {
    if (!enabled())
        return;
    const int64_t now = monotonic_ns();
    int64_t last = last_report_ns_.load(std::memory_order_relaxed);
    if (now - last < kReportIntervalNs)
        return;
    if (!last_report_ns_.compare_exchange_strong(last, now,
                                                  std::memory_order_relaxed))
        return;  // Another thread just won the race to report; skip ours.

    const Stats snapshot = stats();
    std::fprintf(stderr,
                 "P50_F_INPUT_POOL hits=%llu misses=%llu bytes_held=%llu"
                 " puts_dropped=%llu wall_clock_ns=%lld\n",
                 static_cast<unsigned long long>(snapshot.hits),
                 static_cast<unsigned long long>(snapshot.misses),
                 static_cast<unsigned long long>(snapshot.bytes_held),
                 static_cast<unsigned long long>(snapshot.puts_dropped),
                 static_cast<long long>(now));
}

}  // namespace icecc::p50
