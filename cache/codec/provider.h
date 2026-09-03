#pragma once

#include <concepts>
#include <cstdint>
#include <span>
#include <string_view>

namespace residual_group {
class Codec;
}

namespace icecc::codec {

template <class P>
concept ErrorPolicy = requires(const char *reason) {
    { P::fail(reason) } -> std::same_as<void>;
};

template <class H>
concept HistoryProvider = requires(H history, std::span<const std::uint8_t> committed,
                                   std::uint64_t limit) {
    { history.view() } -> std::convertible_to<std::span<const std::uint8_t>>;
    {history.retain(committed, limit)};
    { history.limit() } -> std::same_as<std::uint64_t>;
};

template <class I>
concept AnchorIndexProvider = requires(I index, std::uint64_t key, std::uint64_t position) {
    { index.capacity() } -> std::same_as<std::size_t>;
    { index.insert(key, position) } -> std::same_as<bool>;
    { index.candidates(key) } -> std::convertible_to<std::span<const std::uint64_t>>;
    {index.rebuild(std::span<const std::uint8_t>{}, 0U)};
    {index.begin_provisional()};
    {index.commit_provisional()};
    {index.rollback_provisional()};
};

template <class S>
concept ObjectStoreProvider = requires(S store, std::span<const std::uint8_t> bytes,
                                       std::span<const std::uint32_t> children) {
    { store.intern_line(bytes) } -> std::same_as<std::uint32_t>;
    { store.intern_region(children) } -> std::same_as<std::uint32_t>;
    { store.line_bytes(std::uint32_t{}) } -> std::convertible_to<std::span<const std::uint8_t>>;
    {
        store.region_children(std::uint32_t{})
        } -> std::convertible_to<std::span<const std::uint32_t>>;
    { store.distinct_lines() } -> std::same_as<std::uint64_t>;
};

template <class P>
concept ResourceProvider = ErrorPolicy<P> && requires(P provider) {
    { provider.history() } -> HistoryProvider;
    { provider.anchors() } -> AnchorIndexProvider;
    { provider.objects() } -> ObjectStoreProvider;
    { provider.residual() } -> std::same_as<residual_group::Codec &>;
    { P::max_threads } -> std::convertible_to<unsigned>;
};

} // namespace icecc::codec
