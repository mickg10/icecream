#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

// p29::GrowBuffer<T> -- a no-copy growth container for the per-route
// monotonically-growing region-occurrence streams in the P29 codec: the
// matcher's occurrences_/predecessors_ on the C side and the mirror
// occurrence stream in P29ReceiverRouteState on the F side.
//
// WHY THIS TYPE EXISTS.  Those streams live for the whole route and only ever
// append (plus a bounded per-TU rollback), so a std::vector doubles its
// allocation and copies the entire committed history every time capacity is
// exhausted.  Late in a CodebaseN route (thousands of TUs, ~10^8 occurrences)
// that is a several-hundred-MB copy a handful of times per route, measured as
// ~300 ms on-CPU owner-thread stalls that block every route on the
// single-threaded P50 owner; the F-side deserializer keeps a mirror stream
// with the same growth and stalls the same way.  An anonymous mmap grows with
// mremap(MREMAP_MAYMOVE): the kernel moves the physical page list without
// copying the bytes, and freshly mapped tail pages fault lazily on first
// write, so growth costs neither a copy nor an eager zero-fill.
//
// API SHAPE.  Only the subset of std::vector the call sites use is provided:
// data/size/operator[]/reserve/append-range/push_back/resize-with-fill/
// truncate/clear/view/equality.  Semantics match vector for each:
//   - append(first, last)      == vector::insert(end(), first, last)
//   - resize(n, fill)          == vector::resize(n, fill); shrinking keeps
//                                 the prefix, growing writes only the delta
//   - truncate(n)/clear()     == vector::resize(n)/vector::clear()
//   - max_size()               == SIZE_MAX / sizeof(T) (as for vector with a
//                                 default allocator, modulo allocator slack)
// Overflow throws the same std::length_error vector would throw past
// max_size(); an mmap/mremap failure is translated to std::bad_alloc, the
// same exception type an allocator failure would have thrown, so existing
// catch sites keep working.
//
// POINTER INVALIDATION IS UNCHANGED.  mremap can MOVE the mapping, so raw
// pointers into the buffer do not survive a growth, exactly as with
// std::vector.  The call sites already follow that rule (they recompute
// data() per use) and no iterator is held across a growth.
//
// COPY SEMANTICS.  Copying is a deep copy onto a fresh mapping (the route
// state structs that embed this buffer are copied in the wire tests to
// snapshot "before" state).  Copies are O(n) and never alias.
//
// The type is deliberately not thread-safe: one route owns one buffer.
namespace p29 {

inline constexpr std::size_t grow_buffer_max_size(std::size_t element_size) noexcept {
    return element_size == 0 ? std::numeric_limits<std::size_t>::max()
                             : std::numeric_limits<std::size_t>::max() / element_size;
}

template <class T>
class GrowBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "GrowBuffer<T> requires trivially copyable T");

public:
    using value_type = T;

    GrowBuffer() noexcept = default;

    ~GrowBuffer() { release(); }

    GrowBuffer(const GrowBuffer &other)
        : GrowBuffer(other.size_ == 0 ? nullptr : allocate_copy(other)) {
        if (other.size_ != 0) {
            size_ = other.size_;
            capacity_ = other.capacity_;
        }
    }

    GrowBuffer &operator=(const GrowBuffer &other) {
        if (this == &other)
            return *this;
        if (other.size_ == 0) {
            clear();
            return *this;
        }
        GrowBuffer copy(other);
        swap(copy);
        return *this;
    }

    // Move: the source is left empty.  The moved-from buffer stays usable
    // (it simply starts a fresh growth ladder).
    GrowBuffer(GrowBuffer &&other) noexcept
        : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    GrowBuffer &operator=(GrowBuffer &&other) noexcept {
        if (this != &other) {
            release();
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    void swap(GrowBuffer &other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
    }

    // -- element access ------------------------------------------------------
    [[nodiscard]] T *data() noexcept { return data_; }
    [[nodiscard]] const T *data() const noexcept { return data_; }

    [[nodiscard]] T &operator[](std::size_t index) noexcept { return data_[index]; }
    [[nodiscard]] const T &operator[](std::size_t index) const noexcept {
        return data_[index];
    }

    [[nodiscard]] std::span<const T> view() const noexcept {
        return std::span<const T>(data_, size_);
    }
    [[nodiscard]] std::span<T> view() noexcept { return std::span<T>(data_, size_); }

    // -- capacity -----------------------------------------------------------
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] static constexpr std::size_t max_size() noexcept {
        return grow_buffer_max_size(sizeof(T));
    }

    // Reserve at least `required` elements.  Capacity grows geometrically
    // (at least doubling), so the amortized number of mremap calls over a
    // route is O(log n) and each one touches only page tables, not data.
    void reserve(std::size_t required) {
        if (required <= capacity_)
            return;
        if (required > max_size())
            throw std::length_error("GrowBuffer size exceeds max_size");
        std::size_t wanted = required;
        if (capacity_ != 0) {
            const std::size_t doubled = capacity_ * 2;
            if (doubled > wanted && doubled <= max_size())
                wanted = doubled;
        }
        ensure_capacity(wanted);
    }

    // -- growth -------------------------------------------------------------
    // Append a range, mirroring vector::insert(end(), first, last) for
    // random-access ranges of trivially copyable values.
    template <class InputIt>
    void append(InputIt first, InputIt last) {
        const std::size_t count =
            static_cast<std::size_t>(std::distance(first, last));
        reserve(size_ + count);
        if (count != 0) {
            std::memcpy(data_ + size_, std::to_address(first), count * sizeof(T));
            size_ += count;
        }
    }

    void push_back(const T &value) {
        reserve(size_ + 1);
        data_[size_++] = value;
    }

    // Resize with fill.  Growing writes only the newly exposed tail (the
    // existing prefix is untouched, as with vector); shrinking keeps the
    // prefix bytes and only forgets the logical size.
    void resize(std::size_t new_size, const T &fill) {
        if (new_size > size_) {
            reserve(new_size);
            for (std::size_t index = size_; index != new_size; ++index)
                data_[index] = fill;
        }
        size_ = new_size;
    }

    void resize(std::size_t new_size) { resize(new_size, T{}); }

    // Truncate/undo for the matcher's per-TU rollback.  Bytes beyond the
    // new size stay in the mapping (unreachable, never read past size()).
    void truncate(std::size_t new_size) noexcept {
        if (new_size < size_)
            size_ = new_size;
    }

    void clear() noexcept { size_ = 0; }

    // -- equality (the route-state structs default their operator== into this)
    [[nodiscard]] bool operator==(const GrowBuffer &other) const noexcept {
        return size_ == other.size_ &&
               (size_ == 0 ||
                std::memcmp(data_, other.data_, size_ * sizeof(T)) == 0);
    }
    [[nodiscard]] bool operator!=(const GrowBuffer &other) const noexcept {
        return !(*this == other);
    }

private:
    // Copy constructor tail: allocate a same-capacity mapping and copy the
    // logical contents.  Capacity is preserved so a copied stream does not
    // re-enter the growth ladder at page one.
    [[nodiscard]] static T *allocate_copy(const GrowBuffer &other) {
        const std::size_t bytes = mapped_bytes(other.capacity_);
        void *mapping = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED)
            throw std::bad_alloc();
        advise_hugepage(mapping, bytes);
        T *destination = static_cast<T *>(mapping);
        std::memcpy(destination, other.data_, other.size_ * sizeof(T));
        return destination;
    }

    GrowBuffer(T *stolen) noexcept : data_(stolen) {}

    static std::size_t page_bytes() noexcept {
        static const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        return page == 0 ? std::size_t{4096} : page;
    }

    static std::size_t mapped_bytes(std::size_t elements) noexcept {
        const std::size_t bytes = elements * sizeof(T);
        const std::size_t page = page_bytes();
        return (bytes + page - 1) / page * page;
    }

    // Best-effort huge-page hint, matching the P29MmapInternProvider
    // precedent: the kernel may back the stream with transparent huge pages
    // so the matcher's linear scans fault one 2 MiB page per 512 KiB of
    // stream.  A pure hint: it cannot change bytes, indices, or exceptions,
    // and is ignored by kernels with THP disabled.
    static void advise_hugepage(void *mapping, std::size_t bytes) noexcept {
#if defined(MADV_HUGEPAGE)
        (void)::madvise(mapping, bytes, MADV_HUGEPAGE);
#endif
    }

    void ensure_capacity(std::size_t required_elements) {
        const std::size_t required_bytes = mapped_bytes(required_elements);
        if (data_ == nullptr) {
            void *mapping = ::mmap(nullptr, required_bytes, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mapping == MAP_FAILED)
                throw std::bad_alloc();
            advise_hugepage(mapping, required_bytes);
            data_ = static_cast<T *>(mapping);
        } else {
            const std::size_t old_bytes = mapped_bytes(capacity_);
            // mremap moves the page tables; the bytes never move.  The tail
            // pages fault lazily, so growth costs no eager zero-fill.
            void *grown =
                ::mremap(data_, old_bytes, required_bytes, MREMAP_MAYMOVE);
            if (grown == MAP_FAILED) {
                // Translate the kernel failure to the allocator's exception
                // type so a failed growth leaves the buffer unchanged and
                // existing std::bad_alloc catch sites keep working.
                throw std::bad_alloc();
            }
            data_ = static_cast<T *>(grown);
            advise_hugepage(data_, required_bytes);
        }
        capacity_ = required_bytes / sizeof(T);
    }

    void release() noexcept {
        if (data_) {
            ::munmap(data_, mapped_bytes(capacity_));
            data_ = nullptr;
        }
        size_ = 0;
        capacity_ = 0;
    }

    T *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;  // elements that fit in the mapping
};

}  // namespace p29
