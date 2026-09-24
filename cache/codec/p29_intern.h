#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace icecc::codec {

// The allocation name is part of the provider/accounting seam.  In particular,
// these are C-side codec resources, not immutable F-side object installations.
enum class P29InternAllocation : std::uint8_t {
    TinyTable,
    ShortTable,
    LineTable,
    RegionIndex,
    RegionRecords,
    LineReferences,
    LineBytes,
    RegionBytes,
    RegionLineIds,
};

struct P29InternLayout {
    std::size_t tiny_capacity = std::size_t{1} << 10;
    std::size_t short_capacity = std::size_t{1} << 17;
    std::size_t line_capacity = std::size_t{1} << 21;
    std::size_t region_index_capacity = std::size_t{1} << 18;
    std::size_t region_capacity = std::size_t{1} << 18;
    std::size_t line_reference_capacity = std::size_t{1} << 20;
    std::size_t line_bytes_capacity = std::size_t{384} << 20;
    std::size_t region_bytes_capacity = std::size_t{512} << 20;
    std::size_t region_line_id_capacity = std::size_t{1} << 23;

    [[nodiscard]] static constexpr P29InternLayout probe() { return {}; }

    // Region counts are 4x upstream's.  A whole-repo build (8685 TUs,
    // 141 GB rewrite-includes input) interns 295K distinct Regions, so 1 << 18
    // filled at TU ~7000 while every other table was 11-56% full.
    [[nodiscard]] static constexpr P29InternLayout firefox() {
        P29InternLayout value;
        value.line_capacity = std::size_t{1} << 23;
        value.region_index_capacity = std::size_t{1} << 21;
        value.region_capacity = std::size_t{1} << 20;
        value.line_reference_capacity = std::size_t{1} << 22;
        value.line_bytes_capacity = std::size_t{1} << 30;
        value.region_bytes_capacity = std::size_t{1} << 30;
        value.region_line_id_capacity = std::size_t{1} << 24;
        return value;
    }
};

// Occupancy of the fixed-capacity tables, in the units P29InternLayout sizes
// them in.  process() fails once any one of them is full.  Every Region also
// occupies one region-index slot.
struct P29InternUsage {
    std::size_t tiny_slots = 0;
    std::size_t short_slots = 0;
    std::size_t line_slots = 0;
    std::size_t line_references = 0;
    std::size_t line_bytes = 0;
    std::size_t regions = 0;
    std::size_t region_bytes = 0;
    std::size_t region_line_ids = 0;
};

template <class P>
concept P29InternProvider = requires(P provider, P29InternAllocation kind,
                                     std::size_t bytes, std::size_t alignment,
                                     std::uint32_t id,
                                     std::span<const std::uint8_t> line,
                                     std::span<const std::uint32_t> region) {
    { P::fail(static_cast<const char *>(nullptr)) } -> std::same_as<void>;
    { provider.try_reserve_interner(bytes) } -> std::same_as<bool>;
    { provider.allocate_interner(kind, bytes, alignment) } -> std::same_as<void *>;
    { provider.deallocate_interner(kind, static_cast<void *>(nullptr), bytes,
                                   alignment) } noexcept;
    { provider.release_interner_reservation(bytes) } noexcept;
    { provider.report_interner_usage(bytes, bytes) };
    { provider.publish_line(id, line) };
    { provider.publish_region(id, region) };
};

// allocate_interner() returns suitably aligned, zero-filled storage.  That is
// deliberate: an mmap-backed provider can retain the kernel's shared zero
// pages instead of faulting every reserved table page merely to clear it.

namespace p29_intern_detail {

struct LineRef {
    std::uint32_t off;
    std::uint32_t len;
};

struct TinySlot {
    std::uint64_t bytes;
    std::uint32_t id;
    std::uint8_t len;
    std::uint8_t pad[3];
};

struct ShortSlot {
    std::uint64_t lo;
    std::uint64_t hi;
    std::uint32_t id;
    std::uint8_t len;
    std::uint8_t pad[3];
};

struct LineSlot {
    std::uint64_t hash;
    std::uint32_t off;
    std::uint32_t len;
    std::uint32_t id;
    std::uint32_t pad;
};

struct RegionRecord {
    std::uint64_t hash;
    std::uint32_t raw_off;
    std::uint32_t raw_len;
    std::uint32_t ids_off;
    std::uint32_t ids_count;
    std::uint32_t next1;
    std::uint32_t next2;
};

static_assert(sizeof(LineRef) == 8);
static_assert(sizeof(TinySlot) == 16);
static_assert(sizeof(ShortSlot) == 24);
static_assert(sizeof(LineSlot) == 24);
static_assert(sizeof(RegionRecord) == 32);

[[nodiscard]] inline constexpr bool power_of_two(std::size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] inline std::size_t checked_product(std::size_t left,
                                                 std::size_t right) {
    if (left && right > std::numeric_limits<std::size_t>::max() / left)
        throw std::overflow_error("P29 interner reservation overflows size_t");
    return left * right;
}

inline void checked_add(std::size_t &total, std::size_t value) {
    if (value > std::numeric_limits<std::size_t>::max() - total)
        throw std::overflow_error("P29 interner reservation overflows size_t");
    total += value;
}

[[nodiscard]] inline std::uint64_t mix64(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

[[nodiscard]] inline std::uint64_t fold128(std::uint64_t left,
                                           std::uint64_t right) {
    const __uint128_t product = __uint128_t(left) * right;
    return std::uint64_t(product) ^ std::uint64_t(product >> 64);
}

[[nodiscard]] inline std::uint64_t read64(const std::uint8_t *data) {
    std::uint64_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

[[nodiscard]] inline std::uint64_t read_tail(const std::uint8_t *data,
                                             std::uint32_t length) {
    std::uint64_t value = 0;
    std::memcpy(&value, data, length);
    return value;
}

[[nodiscard]] inline std::uint64_t sampled_hash(const std::uint8_t *data,
                                                std::uint32_t length) {
    constexpr std::uint64_t a = 0xa0761d6478bd642fULL;
    constexpr std::uint64_t b = 0xe7037ed1a0b428dbULL;
    std::uint64_t hash = mix64(std::uint64_t(length) ^ a);
    if (length <= 8)
        return mix64(hash ^ read_tail(data, length));
    if (length <= 16)
        return fold128(read64(data) ^ a, read64(data + length - 8) ^ hash);
    if (length <= 32) {
        hash = fold128(read64(data) ^ a, read64(data + 8) ^ hash);
        return fold128(read64(data + length - 16) ^ b,
                       read64(data + length - 8) ^ hash);
    }
    const std::uint32_t middle = (length >> 1) - 4;
    hash = fold128(read64(data) ^ a, read64(data + 8) ^ hash);
    hash = fold128(read64(data + middle) ^ b,
                   read64(data + length - 16) ^ hash);
    return fold128(read64(data + length - 8) ^ a, hash ^ b);
}

[[nodiscard]] inline std::uint64_t line_hash(const std::uint8_t *key,
                                             std::uint32_t length) {
    constexpr std::uint64_t secret[3] = {
        0x2d358dccaa6c78a5ULL,
        0x8bb84b93962eacc9ULL,
        0x4b33a62ed433d4a3ULL,
    };
    std::uint64_t seed = 0xbdd89aa982704029ULL ^ std::uint64_t(length);
    const std::uint8_t *position = key;
    std::uint32_t remaining = length;
    if (remaining <= 16) {
        std::uint64_t left = 0;
        std::uint64_t right = 0;
        if (remaining >= 8) {
            left = read64(position);
            right = read64(position + remaining - 8);
        } else if (remaining) {
            left = read_tail(position, remaining);
            right = left;
        }
        return fold128(left ^ secret[0], right ^ seed ^ secret[1]);
    }
    std::uint64_t left = read64(position) ^ secret[0];
    std::uint64_t right = read64(position + 8) ^ seed;
    position += 16;
    remaining -= 16;
    while (remaining >= 48) {
        seed = fold128(read64(position) ^ secret[0],
                       read64(position + 8) ^ seed);
        left = fold128(read64(position + 16) ^ secret[1],
                       read64(position + 24) ^ left);
        right = fold128(read64(position + 32) ^ secret[2],
                        read64(position + 40) ^ right);
        position += 48;
        remaining -= 48;
    }
    while (remaining >= 16) {
        seed = fold128(read64(position) ^ secret[0],
                       read64(position + 8) ^ seed);
        position += 16;
        remaining -= 16;
    }
    if (remaining) {
        const std::uint64_t first = remaining >= 8
                                        ? read64(position)
                                        : read_tail(position, remaining);
        const std::uint64_t last = remaining >= 8
                                       ? read64(position + remaining - 8)
                                       : first;
        seed = fold128(first ^ secret[1], last ^ seed);
    }
    return fold128(left ^ secret[0], right ^ seed ^ secret[2]);
}

[[nodiscard]] inline const std::uint8_t *next_region(
    const std::uint8_t *position, const std::uint8_t *end) {
    const std::uint8_t *scan = position + 1;
    while (scan + 1 < end) {
        if (*scan == '#' && scan[-1] == '\n' && scan[1] == ' ')
            return scan;
        ++scan;
    }
    return end;
}

} // namespace p29_intern_detail

[[nodiscard]] inline std::size_t
p29_interner_reservation(const P29InternLayout &layout) {
    using namespace p29_intern_detail;
    std::size_t total = 0;
    checked_add(total, checked_product(layout.tiny_capacity, sizeof(TinySlot)));
    checked_add(total, checked_product(layout.short_capacity, sizeof(ShortSlot)));
    checked_add(total, checked_product(layout.line_capacity, sizeof(LineSlot)));
    checked_add(total,
                checked_product(layout.region_index_capacity, sizeof(std::uint32_t)));
    checked_add(total,
                checked_product(layout.region_capacity, sizeof(RegionRecord)));
    checked_add(total, checked_product(layout.line_reference_capacity,
                                       sizeof(LineRef)));
    checked_add(total, layout.line_bytes_capacity);
    checked_add(total, layout.region_bytes_capacity);
    checked_add(total, checked_product(layout.region_line_id_capacity,
                                       sizeof(std::uint32_t)));
    return total;
}

template <P29InternProvider Provider> class P29Interner {
  public:
    explicit P29Interner(Provider &provider,
                         P29InternLayout layout = P29InternLayout::probe())
        : provider_(provider), layout_(layout) {
        validate_layout();
        reserved_bytes_ = p29_interner_reservation(layout_);
        if (!provider_.try_reserve_interner(reserved_bytes_))
            fail("P29 interner reservation exceeds provider budget");
        reservation_active_ = true;
        try {
            tiny_ = allocate<p29_intern_detail::TinySlot>(
                P29InternAllocation::TinyTable, layout_.tiny_capacity);
            short_ = allocate<p29_intern_detail::ShortSlot>(
                P29InternAllocation::ShortTable, layout_.short_capacity);
            lines_ = allocate<p29_intern_detail::LineSlot>(
                P29InternAllocation::LineTable, layout_.line_capacity);
            region_index_ = allocate<std::uint32_t>(
                P29InternAllocation::RegionIndex, layout_.region_index_capacity);
            region_records_ = allocate<p29_intern_detail::RegionRecord>(
                P29InternAllocation::RegionRecords, layout_.region_capacity);
            line_refs_ = allocate<p29_intern_detail::LineRef>(
                P29InternAllocation::LineReferences,
                layout_.line_reference_capacity);
            line_bytes_ = allocate<std::uint8_t>(P29InternAllocation::LineBytes,
                                                 layout_.line_bytes_capacity);
            region_bytes_ = allocate<std::uint8_t>(
                P29InternAllocation::RegionBytes, layout_.region_bytes_capacity);
            region_ids_ = allocate<std::uint32_t>(
                P29InternAllocation::RegionLineIds,
                layout_.region_line_id_capacity);
            // Line ordinal zero is deliberately absent.  Retaining the sentinel
            // reference reproduces the research dictionary's ordinal layout.
            line_refs_[0] = {0, 0};
        } catch (...) {
            release_storage();
            throw;
        }
    }

    ~P29Interner() { release_storage(); }

    P29Interner(const P29Interner &) = delete;
    P29Interner &operator=(const P29Interner &) = delete;
    P29Interner(P29Interner &&) = delete;
    P29Interner &operator=(P29Interner &&) = delete;

    [[nodiscard]] std::uint32_t distinct_lines() const {
        return next_line_id_ - 1;
    }
    [[nodiscard]] std::uint32_t distinct_regions() const {
        return static_cast<std::uint32_t>(region_count_);
    }
    [[nodiscard]] std::uint64_t region_occurrences() const {
        return region_occurrences_;
    }
    [[nodiscard]] std::uint64_t successor_hits() const { return successor_hits_; }
    [[nodiscard]] std::size_t reserved_bytes() const { return reserved_bytes_; }

    [[nodiscard]] P29InternUsage usage() const noexcept {
        return {tiny_occupied_,   short_occupied_,    line_occupied_,
                next_line_id_,    line_bytes_used_,   region_count_,
                region_bytes_used_, region_ids_used_};
    }

    [[nodiscard]] std::size_t committed_bytes() const {
        using namespace p29_intern_detail;
        std::size_t total = 0;
        checked_add(total, checked_product(tiny_occupied_, sizeof(TinySlot)));
        checked_add(total, checked_product(short_occupied_, sizeof(ShortSlot)));
        checked_add(total, checked_product(line_occupied_, sizeof(LineSlot)));
        checked_add(total,
                    checked_product(region_count_, sizeof(std::uint32_t)));
        checked_add(total, checked_product(region_count_, sizeof(RegionRecord)));
        checked_add(total,
                    checked_product(next_line_id_, sizeof(LineRef)));
        checked_add(total, line_bytes_used_);
        checked_add(total, region_bytes_used_);
        checked_add(total,
                    checked_product(region_ids_used_, sizeof(std::uint32_t)));
        return total;
    }

    // A call is one TU: successor prediction is reset at its boundary and is
    // trained only by transitions wholly within this byte span.
    template <class RegionOutput>
    void process(std::span<const std::uint8_t> bytes, RegionOutput &regions,
                 bool train = true) {
        if (bytes.empty()) {
            provider_.report_interner_usage(reserved_bytes_, committed_bytes());
            return;
        }
        const std::uint8_t *position = bytes.data();
        const std::uint8_t *end = position + bytes.size();
        std::uint32_t previous = kNone;
        while (position < end) {
            bool found = false;
            std::uint32_t region_id = kNone;
            std::uint32_t raw_length = 0;
            std::uint64_t hash = 0;

            if (previous != kNone) {
                const auto &prior = region_records_[previous];
                const std::uint32_t candidates[2] = {prior.next1, prior.next2};
                for (std::uint32_t encoded : candidates) {
                    if (!encoded)
                        continue;
                    const std::uint32_t candidate_id = encoded - 1;
                    const auto &candidate = region_records_[candidate_id];
                    if (candidate.raw_len > std::size_t(end - position))
                        continue;
                    const std::uint8_t *candidate_end = position + candidate.raw_len;
                    const bool exact_boundary =
                        candidate_end == end ||
                        (candidate_end + 1 < end && candidate_end[-1] == '\n' &&
                         candidate_end[0] == '#' && candidate_end[1] == ' ');
                    if (exact_boundary &&
                        std::memcmp(region_bytes_ + candidate.raw_off, position,
                                    candidate.raw_len) == 0) {
                        region_id = candidate_id;
                        raw_length = candidate.raw_len;
                        ++successor_hits_;
                        found = true;
                        break;
                    }
                }
            }

            std::size_t open_region_slot = 0;
            if (!found) {
                const std::uint8_t *next =
                    p29_intern_detail::next_region(position, end);
                const std::size_t wide_length = std::size_t(next - position);
                if (wide_length > std::numeric_limits<std::uint32_t>::max())
                    fail("P29 Region exceeds u32 length");
                raw_length = static_cast<std::uint32_t>(wide_length);
                hash = p29_intern_detail::sampled_hash(position, raw_length) | 1ULL;
                open_region_slot = std::uint32_t(hash) &
                                   (layout_.region_index_capacity - 1);
                std::size_t probes = 0;
                for (;;) {
                    if (++probes > layout_.region_index_capacity)
                        fail("P29 Region table is full");
                    const std::uint32_t encoded = region_index_[open_region_slot];
                    if (!encoded)
                        break;
                    auto &candidate = region_records_[encoded - 1];
                    if (candidate.hash == hash && candidate.raw_len == raw_length &&
                        std::memcmp(region_bytes_ + candidate.raw_off, position,
                                    raw_length) == 0) {
                        region_id = encoded - 1;
                        found = true;
                        break;
                    }
                    open_region_slot =
                        (open_region_slot + 1) &
                        (layout_.region_index_capacity - 1);
                }
            }

            if (!found) {
                region_id = add_region(position, raw_length, hash, open_region_slot);
            }

            regions.push_back(region_id);
            ++region_occurrences_;
            if (train && previous != kNone) {
                auto &prior = region_records_[previous];
                const std::uint32_t encoded = region_id + 1;
                if (!prior.next1)
                    prior.next1 = encoded;
                else if (prior.next1 != encoded && !prior.next2)
                    prior.next2 = encoded;
            }
            previous = region_id;
            position += raw_length;
        }
        provider_.report_interner_usage(reserved_bytes_, committed_bytes());
    }

    [[nodiscard]] std::uint32_t
    intern_line(std::span<const std::uint8_t> bytes) {
        if (bytes.size() > std::numeric_limits<std::uint32_t>::max())
            fail("P29 Line exceeds u32 length");
        const auto length = static_cast<std::uint32_t>(bytes.size());
        if (length <= 4)
            return intern_tiny(bytes.data(), length);
        if (length <= 16)
            return intern_short(bytes.data(), length);

        const std::uint64_t hash =
            p29_intern_detail::line_hash(bytes.data(), length) | 1ULL;
        std::size_t slot = std::uint32_t(hash) & (layout_.line_capacity - 1);
        std::size_t probes = 0;
        for (;;) {
            if (++probes > layout_.line_capacity)
                fail("P29 Line table is full");
            auto &candidate = lines_[slot];
            if (!candidate.id) {
                const std::uint32_t id = add_line(bytes.data(), length);
                candidate = {hash, line_refs_[id].off, length, id, 0};
                ++line_occupied_;
                return id;
            }
            if (candidate.hash == hash && candidate.len == length &&
                std::memcmp(line_bytes_ + candidate.off, bytes.data(), length) == 0)
                return candidate.id;
            slot = (slot + 1) & (layout_.line_capacity - 1);
        }
    }

    [[nodiscard]] std::span<const std::uint8_t> line(std::uint32_t id) const {
        if (id == 0 || id >= next_line_id_)
            throw std::out_of_range("P29 Line id is absent");
        const auto &reference = line_refs_[id];
        return {line_bytes_ + reference.off, reference.len};
    }

    [[nodiscard]] std::span<const std::uint32_t>
    region_lines(std::uint32_t id) const {
        if (id >= region_count_)
            throw std::out_of_range("P29 Region id is absent");
        const auto &record = region_records_[id];
        return {region_ids_ + record.ids_off, record.ids_count};
    }

    [[nodiscard]] std::span<const std::uint8_t>
    region_bytes(std::uint32_t id) const {
        if (id >= region_count_)
            throw std::out_of_range("P29 Region id is absent");
        const auto &record = region_records_[id];
        return {region_bytes_ + record.raw_off, record.raw_len};
    }

  private:
    static constexpr std::uint32_t kNone =
        std::numeric_limits<std::uint32_t>::max();

    [[noreturn]] static void fail(const char *reason) {
        Provider::fail(reason);
        throw std::runtime_error(reason);
    }

    void validate_layout() const {
        if (!p29_intern_detail::power_of_two(layout_.tiny_capacity) ||
            !p29_intern_detail::power_of_two(layout_.short_capacity) ||
            !p29_intern_detail::power_of_two(layout_.line_capacity) ||
            !p29_intern_detail::power_of_two(layout_.region_index_capacity))
            throw std::invalid_argument("P29 interner hash capacities must be powers of two");
        if (!layout_.region_capacity || layout_.region_capacity > kNone ||
            layout_.region_index_capacity > kNone ||
            !layout_.line_reference_capacity ||
            layout_.line_reference_capacity > kNone ||
            layout_.line_bytes_capacity > kNone ||
            layout_.region_bytes_capacity > kNone ||
            layout_.region_line_id_capacity > kNone)
            throw std::invalid_argument("P29 interner layout exceeds u32 addressing");
    }

    template <class T>
    [[nodiscard]] T *allocate(P29InternAllocation kind, std::size_t count) {
        const std::size_t bytes =
            p29_intern_detail::checked_product(count, sizeof(T));
        void *storage = provider_.allocate_interner(kind, bytes, alignof(T));
        if (!storage)
            fail("P29 interner provider returned a null allocation");
        allocations_[allocation_count_++] = {kind, storage, bytes, alignof(T)};
        return static_cast<T *>(storage);
    }

    struct Allocation {
        P29InternAllocation kind{};
        void *pointer = nullptr;
        std::size_t bytes = 0;
        std::size_t alignment = 0;
    };

    void release_storage() noexcept {
        while (allocation_count_) {
            const Allocation &allocation = allocations_[--allocation_count_];
            provider_.deallocate_interner(allocation.kind, allocation.pointer,
                                          allocation.bytes,
                                          allocation.alignment);
        }
        if (reservation_active_) {
            provider_.release_interner_reservation(reserved_bytes_);
            reservation_active_ = false;
        }
    }

    [[nodiscard]] std::uint32_t add_line(const std::uint8_t *data,
                                         std::uint32_t length) {
        if (next_line_id_ == kNone ||
            next_line_id_ >= layout_.line_reference_capacity)
            fail("P29 Line reference arena is full");
        if (length > layout_.line_bytes_capacity - line_bytes_used_)
            fail("P29 Line byte arena is full");
        const std::uint32_t id = next_line_id_;
        provider_.publish_line(id, {data, length});
        const auto offset = static_cast<std::uint32_t>(line_bytes_used_);
        std::memcpy(line_bytes_ + line_bytes_used_, data, length);
        line_bytes_used_ += length;
        line_refs_[id] = {offset, length};
        ++next_line_id_;
        return id;
    }

    [[nodiscard]] std::uint32_t intern_tiny(const std::uint8_t *data,
                                            std::uint32_t length) {
        const std::uint64_t bytes = p29_intern_detail::read_tail(data, length);
        std::size_t slot = std::uint32_t(p29_intern_detail::mix64(
                               bytes ^ (std::uint64_t(length) << 56))) &
                           (layout_.tiny_capacity - 1);
        std::size_t probes = 0;
        for (;;) {
            if (++probes > layout_.tiny_capacity)
                fail("P29 Tiny Line table is full");
            auto &candidate = tiny_[slot];
            if (!candidate.id) {
                const std::uint32_t id = add_line(data, length);
                candidate.bytes = bytes;
                candidate.id = id;
                candidate.len = static_cast<std::uint8_t>(length);
                ++tiny_occupied_;
                return id;
            }
            if (candidate.len == length && candidate.bytes == bytes)
                return candidate.id;
            slot = (slot + 1) & (layout_.tiny_capacity - 1);
        }
    }

    [[nodiscard]] std::uint32_t intern_short(const std::uint8_t *data,
                                             std::uint32_t length) {
        const std::uint64_t low = length >= 8
                                      ? p29_intern_detail::read64(data)
                                      : p29_intern_detail::read_tail(data, length);
        const std::uint64_t high = length > 8
                                       ? p29_intern_detail::read_tail(data + 8,
                                                                      length - 8)
                                       : low;
        const std::uint64_t hash = p29_intern_detail::fold128(
            low ^ 0xa0761d6478bd642fULL,
            high ^ std::uint64_t(length) * 0xe7037ed1a0b428dbULL);
        std::size_t slot = std::uint32_t(hash) &
                           (layout_.short_capacity - 1);
        std::size_t probes = 0;
        for (;;) {
            if (++probes > layout_.short_capacity)
                fail("P29 Short Line table is full");
            auto &candidate = short_[slot];
            if (!candidate.id) {
                const std::uint32_t id = add_line(data, length);
                candidate.lo = low;
                candidate.hi = high;
                candidate.id = id;
                candidate.len = static_cast<std::uint8_t>(length);
                ++short_occupied_;
                return id;
            }
            if (candidate.len == length && candidate.lo == low &&
                candidate.hi == high)
                return candidate.id;
            slot = (slot + 1) & (layout_.short_capacity - 1);
        }
    }

    [[nodiscard]] std::uint32_t add_region(const std::uint8_t *data,
                                           std::uint32_t raw_length,
                                           std::uint64_t hash,
                                           std::size_t index_slot) {
        if (region_count_ >= layout_.region_capacity || region_count_ == kNone)
            fail("P29 Region record arena is full");
        if (raw_length > layout_.region_bytes_capacity - region_bytes_used_)
            fail("P29 Region byte arena is full");

        const std::size_t ids_begin = region_ids_used_;
        const std::uint8_t *line_position = data;
        const std::uint8_t *end = data + raw_length;
        while (line_position < end) {
            const void *newline =
                std::memchr(line_position, '\n', std::size_t(end - line_position));
            const std::uint8_t *line_end =
                newline ? static_cast<const std::uint8_t *>(newline) + 1 : end;
            if (region_ids_used_ >= layout_.region_line_id_capacity)
                fail("P29 Region Line-id arena is full");
            region_ids_[region_ids_used_++] = intern_line(
                {line_position, static_cast<std::size_t>(line_end - line_position)});
            line_position = line_end;
        }

        const auto ids_count = region_ids_used_ - ids_begin;
        if (ids_count > std::numeric_limits<std::uint32_t>::max())
            fail("P29 Region has too many Line ids");
        const std::uint32_t region_id =
            static_cast<std::uint32_t>(region_count_);
        provider_.publish_region(
            region_id,
            {region_ids_ + ids_begin, static_cast<std::size_t>(ids_count)});

        const auto raw_offset = static_cast<std::uint32_t>(region_bytes_used_);
        std::memcpy(region_bytes_ + region_bytes_used_, data, raw_length);
        region_bytes_used_ += raw_length;
        region_records_[region_count_] = {
            hash,
            raw_offset,
            raw_length,
            static_cast<std::uint32_t>(ids_begin),
            static_cast<std::uint32_t>(ids_count),
            0,
            0,
        };
        region_index_[index_slot] = region_id + 1;
        ++region_count_;
        return region_id;
    }

    Provider &provider_;
    P29InternLayout layout_;
    std::size_t reserved_bytes_ = 0;
    bool reservation_active_ = false;
    Allocation allocations_[9]{};
    std::size_t allocation_count_ = 0;

    p29_intern_detail::TinySlot *tiny_ = nullptr;
    p29_intern_detail::ShortSlot *short_ = nullptr;
    p29_intern_detail::LineSlot *lines_ = nullptr;
    std::uint32_t *region_index_ = nullptr;
    p29_intern_detail::RegionRecord *region_records_ = nullptr;
    p29_intern_detail::LineRef *line_refs_ = nullptr;
    std::uint8_t *line_bytes_ = nullptr;
    std::uint8_t *region_bytes_ = nullptr;
    std::uint32_t *region_ids_ = nullptr;

    std::size_t tiny_occupied_ = 0;
    std::size_t short_occupied_ = 0;
    std::size_t line_occupied_ = 0;
    std::size_t line_bytes_used_ = 0;
    std::size_t region_bytes_used_ = 0;
    std::size_t region_ids_used_ = 0;
    std::size_t region_count_ = 0;
    std::uint32_t next_line_id_ = 1;
    std::uint64_t region_occurrences_ = 0;
    std::uint64_t successor_hits_ = 0;
};

} // namespace icecc::codec
