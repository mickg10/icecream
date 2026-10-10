#include "cache/p50_source_view.h"

#include "services/digest128.h"

#include <sys/mman.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>

namespace icecc::p50 {

namespace {

// The single fault-in + digest pass over a fresh private mapping.  Reads
// every page exactly once (so the digest sees the same bytes a pread of
// the whole file would) and warms the PTEs so the later read-only readers
// (canonical split, interner probes, materialization verify) run without
// a per-page fault storm.  XXH3's streaming interface produces the
// identical digest a one-shot XXH3_128bits() over the same byte sequence
// would (the one-shot form is a convenience wrapper over the same
// accumulator), so the value matches the historical separate digest128()
// pass byte for byte.
void populate_and_digest(std::span<const std::uint8_t> bytes,
                         Digest128& digest) {
    icecc::Digest128Builder builder;
    constexpr std::size_t kChunk = std::size_t{2} << 20;
    const std::uint8_t* position = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining > kChunk) {
        builder.append(std::span<const std::uint8_t>(position, kChunk));
        position += kChunk;
        remaining -= kChunk;
    }
    if (remaining != 0)
        builder.append(std::span<const std::uint8_t>(position, remaining));
    digest = builder.finish();
}

}  // namespace

std::optional<std::shared_ptr<const P50SourceView>>
P50SourceView::map_fd(int fd, uint64_t limit) noexcept {
    if (fd < 0 || limit > SIZE_MAX)
        return std::nullopt;
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > limit)
        return std::nullopt;
    const std::size_t size = static_cast<std::size_t>(info.st_size);
    try {
        auto view = std::make_shared<P50SourceView>();
        if (size != 0) {
            void* mapping = ::mmap(nullptr, size, PROT_READ,
                                   MAP_PRIVATE | MAP_POPULATE, fd, 0);
            if (mapping != MAP_FAILED) {
                view->mapping_ = mapping;
                view->mapping_bytes_ = size;
                view->view_ = {static_cast<const std::uint8_t*>(mapping), size};
                populate_and_digest(view->view_, view->digest_);
                return std::const_pointer_cast<const P50SourceView>(view);
            }
            // Fall through to the owned pread copy: a mapping refusal must
            // not fail a read the historical path would have served.
        }
        auto bytes = std::make_shared<std::vector<uint8_t>>(size);
        std::size_t offset = 0;
        while (offset != bytes->size()) {
            const ssize_t count = ::pread(fd, bytes->data() + offset,
                                          bytes->size() - offset,
                                          static_cast<off_t>(offset));
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            return std::nullopt;
        }
        view->owner_ = std::move(bytes);
        view->view_ = *std::static_pointer_cast<const std::vector<uint8_t>>(view->owner_);
        // Digest over the copy: identical bytes, identical value.
        populate_and_digest(view->view_, view->digest_);
        return std::const_pointer_cast<const P50SourceView>(view);
    } catch (...) {
        return std::nullopt;
    }
}

std::shared_ptr<const P50SourceView>
P50SourceView::own(std::vector<uint8_t> bytes) noexcept {
    auto view = std::make_shared<P50SourceView>();
    auto held = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    view->view_ = *held;
    view->owner_ = std::move(held);
    try {
        populate_and_digest(view->view_, view->digest_);
    } catch (...) {
        // own() holds the bytes in memory already; a digest refusal cannot
        // happen for the streaming builder (its only failures are state
        // misuse), so keep the historical no-throw shape with a zero digest.
        view->digest_ = Digest128{};
    }
    return std::const_pointer_cast<const P50SourceView>(view);
}

P50SourceView::~P50SourceView() {
    if (mapping_ != nullptr)
        ::munmap(mapping_, mapping_bytes_);
}

}  // namespace icecc::p50
