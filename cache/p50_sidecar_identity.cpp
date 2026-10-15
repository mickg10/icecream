#include "p50_sidecar_identity.h"

#include <limits>
#include <time.h>

namespace icecc::p50::sidecar {

std::optional<MonotonicObservation>
SystemMonotonicObservationSource::observe() noexcept {
    timespec sample{};
    if (::clock_gettime(CLOCK_MONOTONIC, &sample) != 0 || sample.tv_sec < 0 ||
        sample.tv_nsec < 0 || sample.tv_nsec >= 1000000000L)
        return std::nullopt;

    constexpr int64_t kNanosPerSecond = 1000000000;
    if (static_cast<uint64_t>(sample.tv_sec) >
        static_cast<uint64_t>((std::numeric_limits<int64_t>::max() -
                               kNanosPerSecond + 1) /
                              kNanosPerSecond))
        return std::nullopt;

    const int64_t seconds = static_cast<int64_t>(sample.tv_sec);
    const int64_t now_ns = seconds * kNanosPerSecond + sample.tv_nsec;
    const MonotonicClockIdentity clock = process_monotonic_clock_identity();
    MonotonicObservation observation{now_ns, clock};
    return observation.valid() ? std::optional<MonotonicObservation>(observation)
                               : std::nullopt;
}

LaunchIdentityAllocator::LaunchIdentityAllocator(
    uint64_t generation, uint64_t first_attempt,
    StoreIdentityEntropyProvider entropy_provider) noexcept
    : generation_(generation), next_attempt_(first_attempt),
      next_store_generation_(generation), entropy_provider_(entropy_provider) {
    if (generation_ == 0 || generation_ == std::numeric_limits<uint64_t>::max() ||
        next_attempt_ == 0 || next_attempt_ == std::numeric_limits<uint64_t>::max() ||
        next_store_generation_ == 0 ||
        next_store_generation_ == std::numeric_limits<uint64_t>::max()) {
        generation_ = 0;
        next_attempt_ = 0;
        next_store_generation_ = 0;
    }
}

std::optional<LaunchIncarnation> LaunchIdentityAllocator::allocate() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation_ == 0 || next_attempt_ == 0 ||
        next_attempt_ == std::numeric_limits<uint64_t>::max() ||
        next_store_generation_ == 0 ||
        next_store_generation_ == std::numeric_limits<uint64_t>::max())
        return std::nullopt;
    const local::Identity identity{generation_, next_attempt_++};
    const uint64_t store_generation = next_store_generation_++;
    constexpr size_t kMaxRootRetries = 8;
    for (size_t retry = 0; retry != kMaxRootRetries; ++retry) {
        StoreIdentityRoot root{};
        if (!fresh_store_identity_root_with_provider(root, entropy_provider_))
            return std::nullopt;
        bool duplicate = false;
        for (const StoreIdentityRoot& prior : recent_roots_) {
            if (prior == root) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;
        recent_roots_.push_back(root);
        constexpr size_t kMaxDuplicateHistory = 128;
        if (recent_roots_.size() > kMaxDuplicateHistory)
            recent_roots_.pop_front();
        LaunchIncarnation incarnation{identity, store_generation, root,
                                      c_store_guid_for_root(root),
                                      f_store_guid_for_root(root)};
        if (incarnation.valid())
            return incarnation;
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace icecc::p50::sidecar
