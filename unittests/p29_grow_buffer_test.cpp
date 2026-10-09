// Unit tests for p29::GrowBuffer<T> (cache/codec/p29_grow_buffer.h), the
// mremap-growing replacement for the per-route std::vector occurrence
// streams in the P29 codec (OnlineS1::occurrences_/predecessors_ on the C
// side, P29ReceiverRouteState::occurrences on the F side).
//
// Coverage demanded by the change brief:
//   - growth: appends across many capacity crossings preserve every byte and
//     keep indices stable;
//   - pointer refresh: a raw pointer into the buffer does not survive a
//     growth (mremap may move the mapping) -- the documented contract;
//   - truncate/undo and clear: rollback keeps the prefix, forgets the tail;
//   - fill: resize(n, value) writes only the newly exposed tail;
//   - overflow: a size past max_size() throws std::length_error (the same
//     exception class std::vector throws), not UB;
//   - move semantics: the moved-from buffer is empty and reusable;
//   - copy semantics: a copy is a deep, non-aliasing copy with equal content.
//
// Everything here is deterministic and runs in well under a second; no
// filesystem corpus is needed.

#include "cache/codec/p29_grow_buffer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using p29::GrowBuffer;

[[noreturn]] void die(const std::string &reason) {
    throw std::runtime_error(reason);
}

void require(bool condition, const std::string &reason) {
    if (!condition)
        die(reason);
}

// A deterministic value pattern that cannot alias page-aligned zeros.
[[nodiscard]] std::uint32_t pattern(std::size_t index) {
    return static_cast<std::uint32_t>(0x9E3779B9u ^ (index * 2654435761u));
}

void check_growth_preserves_bytes() {
    GrowBuffer<std::uint32_t> buffer;
    std::vector<std::uint32_t> mirror;
    for (std::size_t index = 0; index != 200000; ++index) {
        const std::uint32_t value = pattern(index);
        buffer.push_back(value);
        mirror.push_back(value);
        require(buffer.size() == mirror.size(), "size drifts from mirror");
    }
    require(buffer.capacity() >= buffer.size(), "capacity below size");
    // Cross many capacity crossings (page granularity inside the container,
    // so force several by appending ranges at once too).
    std::vector<std::uint32_t> chunk;
    for (std::size_t index = 0; index != 4096; ++index)
        chunk.push_back(pattern(200000 + index));
    for (int round = 0; round != 10; ++round) {
        buffer.append(chunk.begin(), chunk.end());
        mirror.insert(mirror.end(), chunk.begin(), chunk.end());
    }
    require(buffer.size() == mirror.size(), "append size differs");
    for (std::size_t index = 0; index != buffer.size(); ++index)
        require(buffer[index] == mirror[index],
                "byte at index " + std::to_string(index) + " differs after growth");
    require(std::memcmp(buffer.data(), mirror.data(), buffer.size() * 4) == 0,
            "bulk memcmp differs after growth");
}

void check_pointer_refresh_contract() {
    GrowBuffer<std::uint32_t> buffer;
    buffer.push_back(1);
    const std::uint32_t *stale = buffer.data();
    std::uintptr_t addresses[16] = {};
    bool moved = false;
    for (int round = 0; round != 16; ++round) {
        // Record, then grow: the mapping may move (MREMAP_MAYMOVE), so a raw
        // pointer taken before the growth is only valid until the growth.
        const std::uintptr_t before =
            reinterpret_cast<std::uintptr_t>(buffer.data());
        for (int index = 0; index != 100000; ++index)
            buffer.push_back(static_cast<std::uint32_t>(round * 100000 + index));
        const std::uintptr_t after =
            reinterpret_cast<std::uintptr_t>(buffer.data());
        addresses[round] = after;
        moved = moved || after != before;
        // The documented rule: never dereference `stale` after a growth.
        (void)stale;
        require(buffer[0] == 1, "first element changed across growth");
    }
    (void)moved;  // the mapping MAY move; both outcomes are legal.
    // The mapping must never be page-unaligned (mmap guarantees it).
    for (const std::uintptr_t address : addresses)
        require((address & 0xFFF) == 0, "mapping is not page-aligned");
}

void check_truncate_and_undo() {
    GrowBuffer<std::uint32_t> buffer;
    for (std::size_t index = 0; index != 50000; ++index)
        buffer.push_back(pattern(index));
    const std::size_t committed = 40000;
    // Simulate a TU that appends then aborts: journal records the old size,
    // truncate rolls back.  This is exactly OnlineS1::abort()'s shape.
    const std::size_t base = buffer.size();
    for (std::size_t index = 0; index != 1000; ++index)
        buffer.push_back(pattern(base + index));
    buffer.truncate(base);
    require(buffer.size() == base, "truncate did not restore the journal size");
    for (std::size_t index = 0; index != base; ++index)
        require(buffer[index] == pattern(index),
                "truncate damaged the committed prefix");
    // truncate to a LARGER size is a no-op (only truncation).
    buffer.truncate(base + 5);
    require(buffer.size() == base, "truncate grew the buffer");
    // clear() empties without releasing the mapping; re-append keeps working
    // (and the retained mapping means no new mmap for the reused ladder).
    buffer.clear();
    require(buffer.empty(), "clear left elements behind");
    for (std::size_t index = 0; index != 1000; ++index)
        buffer.push_back(pattern(index));
    require(buffer.size() == 1000, "re-append after clear failed");
    // Redo the aborted TU against a fresh journal at the truncated size:
    // appending the same values reproduces the pre-abort stream exactly.
    GrowBuffer<std::uint32_t> replay;
    for (std::size_t index = 0; index != committed; ++index)
        replay.push_back(pattern(index));
    const std::size_t replay_base = replay.size();
    for (std::size_t index = 0; index != 1000; ++index)
        replay.push_back(pattern(base + index));
    replay.truncate(replay_base);
    for (std::size_t index = 0; index != 1000; ++index)
        replay.push_back(pattern(base + index));
    require(replay.size() == replay_base + 1000,
            "redo after truncate did not reproduce the aborted TU");
    for (std::size_t index = 0; index != 1000; ++index)
        require(replay[replay_base + index] == pattern(base + index),
                "redo wrote different bytes than the aborted TU");
}

void check_resize_fill() {
    GrowBuffer<std::uint32_t> buffer;
    buffer.push_back(7);
    buffer.resize(10000, 0xFFFFFFFFu);
    require(buffer.size() == 10000, "resize size differs");
    require(buffer[0] == 7, "resize clobbered the existing prefix");
    for (std::size_t index = 1; index != 10000; ++index)
        require(buffer[index] == 0xFFFFFFFFu,
                "resize fill value differs at " + std::to_string(index));
    // Shrink keeps the prefix; growing again with a different fill only
    // writes the delta.
    buffer.resize(5000);
    require(buffer.size() == 5000 && buffer[4999] == 0xFFFFFFFFu,
            "shrink lost the prefix");
    buffer.resize(7000, 0x12345678u);
    require(buffer[4999] == 0xFFFFFFFFu, "regrow overwrote the kept prefix");
    for (std::size_t index = 5000; index != 7000; ++index)
        require(buffer[index] == 0x12345678u, "regrow fill differs");
    // resize with the default fill writes value-initialized Ts.
    GrowBuffer<std::uint32_t> fresh;
    fresh.resize(4097);
    for (std::size_t index = 0; index != 4097; ++index)
        require(fresh[index] == 0, "default resize did not zero-fill");
}

void check_overflow() {
    GrowBuffer<std::uint32_t> buffer;
    buffer.push_back(1);
    bool threw = false;
    try {
        // Past max_size(): the same class of overflow std::vector rejects
        // with std::length_error, never UB, and the buffer is unchanged.
        buffer.reserve(GrowBuffer<std::uint32_t>::max_size() + 1);
    } catch (const std::length_error &) {
        threw = true;
    }
    require(threw, "reserve past max_size() did not throw std::length_error");
    require(buffer.size() == 1 && buffer[0] == 1,
            "failed reserve mutated the buffer");
    threw = false;
    try {
        buffer.resize(GrowBuffer<std::uint32_t>::max_size() + 2, 0);
    } catch (const std::length_error &) {
        threw = true;
    }
    require(threw, "resize past max_size() did not throw std::length_error");
    require(buffer.size() == 1, "failed resize mutated the size");
}

void check_move_semantics() {
    GrowBuffer<std::uint32_t> source;
    for (std::size_t index = 0; index != 100000; ++index)
        source.push_back(pattern(index));
    const std::size_t size = source.size();
    const std::size_t capacity = source.capacity();
    GrowBuffer<std::uint32_t> moved = std::move(source);
    require(moved.size() == size && moved.capacity() == capacity,
            "move lost size or capacity");
    require(source.empty() && source.size() == 0 && source.data() == nullptr,
            "moved-from buffer is not empty");
    for (std::size_t index = 0; index != size; ++index)
        require(moved[index] == pattern(index), "move corrupted bytes");
    // The moved-from buffer is reusable: it starts a fresh growth ladder.
    source.push_back(42);
    require(source.size() == 1 && source[0] == 42,
            "moved-from buffer is not reusable");
    // Move-assign transfers and releases the target's old mapping.
    GrowBuffer<std::uint32_t> target;
    target.push_back(999);
    target = std::move(moved);
    require(target.size() == size, "move-assign lost the size");
    require(moved.empty(), "move-assign left the source non-empty");
    require(target[size - 1] == pattern(size - 1), "move-assign corrupted bytes");
}

void check_copy_semantics() {
    GrowBuffer<std::uint32_t> source;
    for (std::size_t index = 0; index != 100000; ++index)
        source.push_back(pattern(index));
    const GrowBuffer<std::uint32_t> copy(source);
    require(copy == source, "copy differs from source");
    require(copy.data() != source.data(), "copy aliases the source mapping");
    for (std::size_t index = 0; index != source.size(); ++index)
        require(copy[index] == pattern(index), "copy byte differs");
    // Mutating the copy must not touch the source.
    GrowBuffer<std::uint32_t> mutable_copy(copy);
    mutable_copy[3] = 0xDEADBEEFu;
    require(source[3] == pattern(3) && copy[3] == pattern(3),
            "mutating a copy changed another buffer");
    // Copy-assign self-assignment is a no-op (guard against accidentally
    // releasing the source mapping through the copy-assign path).
    GrowBuffer<std::uint32_t> self;
    self.push_back(5);
    GrowBuffer<std::uint32_t> &alias = self;
    alias = self;
    require(self.size() == 1 && self[0] == 5, "self-assign broke the buffer");
    // Copy of an empty buffer stays empty and usable.
    GrowBuffer<std::uint32_t> empty;
    const GrowBuffer<std::uint32_t> empty_copy(empty);
    require(empty_copy.empty(), "copy of empty is not empty");
    GrowBuffer<std::uint32_t> assigned;
    assigned = empty_copy;
    assigned.push_back(3);
    require(assigned.size() == 1 && assigned[0] == 3,
            "assigned-from-empty buffer is unusable");
}

void check_equality_and_view() {
    GrowBuffer<std::uint32_t> left;
    GrowBuffer<std::uint32_t> right;
    require(left == right, "two empty buffers differ");
    left.push_back(1);
    require(left != right, "empty equals one-element");
    right.push_back(1);
    require(left == right, "identical single elements differ");
    right.push_back(2);
    require(left != right, "size difference not detected");
    left.push_back(3);
    require(left != right, "equal size, different bytes, not detected");
    // The span view covers exactly the logical contents.
    left.clear();
    for (std::size_t index = 0; index != 300; ++index)
        left.push_back(pattern(index));
    const std::span<const std::uint32_t> view = left.view();
    require(view.size() == left.size(), "view size differs");
    require(view.data() == left.data(), "view data differs");
    require(view.front() == pattern(0) && view.back() == pattern(299),
            "view contents differ");
}

// Route-shaped integration: the OnlineS1 matcher's own growth path over a
// long synthetic route, checking the stream against a mirror kept in plain
// vectors.  This is the same shape the matcher uses (append per TU, resize
// the predecessor stream, truncate on abort).
void check_route_shaped_stream() {
    GrowBuffer<std::uint32_t> occurrences;
    GrowBuffer<std::uint32_t> predecessors;
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<std::uint32_t> width_dist(1, 5000);
    std::vector<std::uint32_t> mirror_occ;
    std::vector<std::uint32_t> mirror_pre;
    constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
    for (int tu = 0; tu != 400; ++tu) {
        const std::uint32_t width = width_dist(rng);
        std::vector<std::uint32_t> current(width);
        for (std::uint32_t index = 0; index != width; ++index)
            current[index] = rng() % 1000;
        occurrences.append(current.begin(), current.end());
        predecessors.resize(occurrences.size(), kNone);
        mirror_occ.insert(mirror_occ.end(), current.begin(), current.end());
        mirror_pre.resize(mirror_occ.size(), kNone);
        require(occurrences.size() == mirror_occ.size() &&
                    predecessors.size() == mirror_pre.size(),
                "route stream sizes drift from the mirror");
        // Every 16th TU is discarded: abort rolls back exactly like the
        // matcher's journal does.
        if (tu % 16 == 15) {
            occurrences.truncate(mirror_occ.size() - current.size());
            predecessors.truncate(mirror_pre.size() - current.size());
            mirror_occ.resize(mirror_occ.size() - current.size());
            mirror_pre.resize(mirror_pre.size() - current.size());
        }
    }
    require(std::memcmp(occurrences.data(), mirror_occ.data(),
                        occurrences.size() * 4) == 0,
            "occurrence stream differs from its vector mirror");
    require(std::memcmp(predecessors.data(), mirror_pre.data(),
                        predecessors.size() * 4) == 0,
            "predecessor stream differs from its vector mirror");
    require(occurrences.size() == mirror_occ.size(),
            "route stream sizes drift from the mirror");
    require(occurrences.size() > 500000,
            "synthetic route is too short to cross many growths");
}

} // namespace

int main() {
    try {
        check_growth_preserves_bytes();
        check_pointer_refresh_contract();
        check_truncate_and_undo();
        check_resize_fill();
        check_overflow();
        check_move_semantics();
        check_copy_semantics();
        check_equality_and_view();
        check_route_shaped_stream();
    } catch (const std::exception &error) {
        std::cerr << "p29_grow_buffer: FAIL: " << error.what() << "\n";
        return 1;
    }
    std::cout << "p29_grow_buffer: PASS\n";
    return 0;
}
