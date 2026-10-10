// One read-only source input for the P50 C sidecar's per-TX source path.
//
// The sidecar's source read (read_source_fd, cache/p50_cache_service.cpp)
// historically materialized the whole preprocessed TU as a fresh
// std::vector<uint8_t>: a value-initialized allocation (a full zeroing
// pass), a full pread copy out of the page cache, and then a separate
// digest128 pass over the same bytes.  On the C sidecar's measured per-TX
// budget (microbench, 36 MiB hot-cache TU) that read+digest pair cost
// ~10.5 ms CPU per transfer; a MAP_PRIVATE | MAP_POPULATE mapping of the
// same regular file costs ~1 ms for the mapping plus the digest pass,
// because no bytes are copied at all -- the readers (digest, canonical
// split, interning, materialization verify) fault the kernel's page-cache
// pages in place.
//
// P50SourceView is that mapping plus the content digest fused into the
// same single pass that faults the pages in, so a source read and its
// digest are produced together with one traversal.  The mapping, not the
// file descriptor, keeps the bytes alive: closing fd after map_fd() does
// not invalidate the view.  The source file is fully written before the
// sidecar's source transfer begins (the compiler wrapper produced it and
// the sidecar owns its lifecycle), so no writer exists during the
// mapping's lifetime -- the same freshness assumption the historical
// pread read made, one whole-file read at a known size.  The bytes are
// byte-identical to a pread of the same file at the same size -- the
// differential identity harness (cache/sim/p29ccpubench.cpp --record /
// --compare) proves the produced wire bytes and the authority state do
// not change.
//
// own() keeps the historical owned-copy shape (used as the fallback when a
// mapping cannot be created, and by callers/tests that already hold a
// vector).  Both shapes expose the same span and the same digest field; a
// view produced by own() carries its digest computed the same way.
#ifndef ICECREAM_P50_SOURCE_VIEW_H
#define ICECREAM_P50_SOURCE_VIEW_H

#include "cache/protocol50.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace icecc::p50 {

class P50SourceView {
public:
    // Maps the whole regular file at fd (whose size must not exceed limit)
    // privately, faults every page in, and computes the XXH3-128 content
    // digest in the same single pass.  Returns nullopt exactly where the
    // historical pread read did: bad descriptor, not a regular file, or a
    // size above the limit.  If the mapping cannot be created, falls back
    // to the owned pread copy rather than failing where the historical
    // read succeeded.
    static std::optional<std::shared_ptr<const P50SourceView>>
    map_fd(int fd, uint64_t limit) noexcept;

    // The historical owned copy of bytes that already live in memory.
    static std::shared_ptr<const P50SourceView>
    own(std::vector<uint8_t> bytes) noexcept;

    // Public so std::make_shared can construct one; only the static
    // factories ever build a populated view.
    P50SourceView() = default;
    P50SourceView(const P50SourceView&) = delete;
    P50SourceView& operator=(const P50SourceView&) = delete;

    ~P50SourceView();

    [[nodiscard]] const std::uint8_t* data() const noexcept { return view_.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return view_.size(); }
    [[nodiscard]] std::span<const std::uint8_t> span() const noexcept { return view_; }

    // The XXH3-128 digest of the whole view, computed at construction.
    [[nodiscard]] Digest128 digest() const noexcept { return digest_; }

private:
    // The mapped or owned bytes.  For a mapping this is the mapped region
    // (munmap in the destructor); for own() the moved-in vector.
    std::span<const std::uint8_t> view_{};
    // Type-erased owner: either the owning vector (std::vector<uint8_t>)
    // or nullptr for a mapping (the destructor munmaps via mapping_).
    std::shared_ptr<const void> owner_;
    void* mapping_ = nullptr;  // MAP_FAILED-safe: nullptr when not mapped
    std::size_t mapping_bytes_ = 0;
    Digest128 digest_{};
};

}  // namespace icecc::p50

#endif  // ICECREAM_P50_SOURCE_VIEW_H
