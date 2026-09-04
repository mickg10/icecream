#include "p50_slice0.h"
#include "codec/p29_intern.h"
#include "codec/p29_wire.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace icecc::p50 {
namespace {

constexpr uint32_t byte_swap32(uint32_t value) noexcept {
    return ((value & 0x000000ffU) << 24) |
           ((value & 0x0000ff00U) << 8) |
           ((value & 0x00ff0000U) >> 8) |
           ((value & 0xff000000U) >> 24);
}

constexpr uint64_t byte_swap64(uint64_t value) noexcept {
    return ((value & 0x00000000000000ffULL) << 56) |
           ((value & 0x000000000000ff00ULL) << 40) |
           ((value & 0x0000000000ff0000ULL) << 24) |
           ((value & 0x00000000ff000000ULL) << 8) |
           ((value & 0x000000ff00000000ULL) >> 8) |
           ((value & 0x0000ff0000000000ULL) >> 24) |
           ((value & 0x00ff000000000000ULL) >> 40) |
           ((value & 0xff00000000000000ULL) >> 56);
}

template <typename T>
constexpr T host_to_big_endian(T value) noexcept {
    static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));
    static_assert(std::endian::native == std::endian::little ||
                  std::endian::native == std::endian::big);
    if constexpr (std::endian::native == std::endian::little) {
        if constexpr (sizeof(T) == sizeof(uint32_t))
            return static_cast<T>(byte_swap32(static_cast<uint32_t>(value)));
        else
            return static_cast<T>(byte_swap64(static_cast<uint64_t>(value)));
    }
    return value;
}

void store_u64be(uint8_t* output, uint64_t value) noexcept {
    const uint64_t encoded = host_to_big_endian(value);
    std::memcpy(output, &encoded, sizeof(encoded));
}

uint64_t load_u64be(const uint8_t* input) noexcept {
    uint64_t encoded = 0;
    std::memcpy(&encoded, input, sizeof(encoded));
    return host_to_big_endian(encoded);
}

bool byte_object(ObjectType type) {
    return type == ObjectType::Atom || type == ObjectType::Line ||
           type == ObjectType::Material || type == ObjectType::Path ||
           type == ObjectType::Blob;
}

std::vector<uint8_t> encode_payload(ObjectType type, const ObjectPayload& payload) {
    constexpr size_t header_bytes = 1 + sizeof(uint64_t);
    if (const auto* bytes = std::get_if<BytesPayload>(&payload)) {
        if (bytes->bytes.size() > std::numeric_limits<size_t>::max() - header_bytes)
            throw std::overflow_error("canonical byte payload exceeds addressable size");
        std::vector<uint8_t> result(header_bytes + bytes->bytes.size());
        result[0] = 0;
        store_u64be(result.data() + 1, bytes->bytes.size());
        if (!bytes->bytes.empty())
            std::memcpy(result.data() + header_bytes, bytes->bytes.data(),
                        bytes->bytes.size());
        (void)type;
        return result;
    }

    const auto& children = std::get<ChildrenPayload>(payload).children;
    if (children.size() >
        (std::numeric_limits<size_t>::max() - header_bytes) / sizeof(uint64_t))
        throw std::overflow_error("canonical child payload exceeds addressable size");
    std::vector<uint8_t> result(header_bytes + children.size() * sizeof(uint64_t));
    result[0] = 1;
    store_u64be(result.data() + 1, children.size());
    size_t offset = header_bytes;
    for (Key64 child : children) {
        store_u64be(result.data() + offset, child.wire_value());
        offset += sizeof(uint64_t);
    }
    (void)type;
    return result;
}

uint64_t read_u64(std::span<const uint8_t> bytes, size_t& offset) {
    if (bytes.size() - offset < 8)
        throw std::invalid_argument("canonical object payload ended early");
    const uint64_t result = load_u64be(bytes.data() + offset);
    offset += sizeof(uint64_t);
    return result;
}

ObjectPayload decode_object_payload(ObjectType type, std::span<const uint8_t> bytes) {
    if (bytes.empty()) throw std::invalid_argument("canonical object payload is empty");
    size_t offset = 1;
    const uint64_t count = read_u64(bytes, offset);
    if (bytes[0] == 0) {
        if (!byte_object(type) || count != bytes.size() - offset)
            throw std::invalid_argument("byte object payload has the wrong shape");
        return BytesPayload{{bytes.begin() + offset, bytes.end()}};
    }
    if (bytes[0] != 1 || byte_object(type) || count > (bytes.size() - offset) / 8 ||
        count * 8 != bytes.size() - offset)
        throw std::invalid_argument("child object payload has the wrong shape");
    ChildrenPayload result;
    result.children.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i != count; ++i) {
        const auto key = Key64::from_wire(read_u64(bytes, offset));
        if (!key) throw std::invalid_argument("child object contains invalid Key64");
        result.children.push_back(*key);
    }
    return result;
}

Digest128 object_digest(ObjectType type, const ObjectPayload& payload) {
    const std::vector<uint8_t> encoded = encode_payload(type, payload);
    Digest128Builder out;
    out.append("ICECC-P50-OBJECT-V1");
    out.append_u8(static_cast<uint8_t>(type));
    out.append_u64(encoded.size());
    out.append(encoded);
    return out.finish();
}

bool same_commit(const TxCommit& commit, const CActiveTx& active) {
    const Digest128 expected_post = compute_post_state_digest(
        active.begin.pre_state_digest, active.begin.history_nonce,
        active.begin.rel_seq, active.begin.tu_seq, active.begin.transaction_digest);
    return commit.history_nonce == active.begin.history_nonce &&
           commit.rel_seq == active.begin.rel_seq &&
           commit.tu_seq == active.begin.tu_seq &&
           commit.transaction_digest == active.begin.transaction_digest &&
           commit.raw_digest == active.begin.raw_digest &&
           commit.post_state_digest == expected_post;
}

bool same_begin(const TxBegin& left, const TxBegin& right) { return left == right; }

struct P29CachedSourceText {
    bool attempted = false;
    bool available = false;
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

class P29SourceTextCache {
public:
    codec::P29SourceTextView get(std::string_view path) {
        auto position = sources_.find(path);
        bool inserted = false;
        if (position == sources_.end()) {
            auto result = sources_.try_emplace(std::string(path));
            position = result.first;
            inserted = result.second;
        }
        P29CachedSourceText& source = position->second;
        if (inserted || !source.attempted)
            load(position->first, source);
        return {source.available, source.bytes, source.offsets};
    }

private:
    static void load(const std::string& path, P29CachedSourceText& source) {
        source.attempted = true;
        source.offsets.assign(1, 0);
        if (!codec::p29_wire_detail::system_source_path(path))
            return;
        std::error_code error;
        const uintmax_t wide_size = std::filesystem::file_size(path, error);
        if (error || wide_size > std::numeric_limits<uint32_t>::max())
            return;
        const size_t size = static_cast<size_t>(wide_size);
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return;
        source.bytes.resize(size);
        if (size != 0 &&
            !input.read(reinterpret_cast<char*>(source.bytes.data()),
                        static_cast<std::streamsize>(size))) {
            source.bytes.clear();
            return;
        }
        source.offsets.reserve(size / 32 + 2);
        for (size_t index = 0; index < source.bytes.size(); ++index)
            if (source.bytes[index] == '\n')
                source.offsets.push_back(static_cast<uint32_t>(index + 1));
        if (source.offsets.back() != source.bytes.size())
            source.offsets.push_back(static_cast<uint32_t>(source.bytes.size()));
        source.available = true;
    }

    std::map<std::string, P29CachedSourceText, std::less<>> sources_;
};

Digest128 hash_regular_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open P29 system source");
    Digest128Builder digest;
    std::array<uint8_t, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const std::streamsize count = input.gcount();
        if (count > 0)
            digest.append(std::span<const uint8_t>(
                buffer.data(), static_cast<size_t>(count)));
    }
    if (!input.eof())
        throw std::runtime_error("cannot read P29 system source");
    return digest.finish();
}

struct P29FingerprintFile {
    std::string path;
    uint64_t size = 0;
    int64_t mtime_ns = 0;
    Digest128 digest{};
};

using P29FingerprintCacheKey =
    std::tuple<std::string, uint64_t, int64_t>;
using P29FingerprintCache =
    std::map<P29FingerprintCacheKey, Digest128>;

constexpr std::string_view kP29FingerprintCacheMagic =
    "ICECC-P29-SYSTEM-SOURCE-CACHE-V1";
constexpr std::string_view kP29FingerprintCacheFile =
    "p29-system-source-fingerprint-v1.cache";
constexpr std::string_view kP29FingerprintLockFile =
    "p29-system-source-fingerprint-v1.lock";
constexpr uint64_t kP29FingerprintMaximumCacheBytes = uint64_t{128} << 20;
constexpr uint64_t kP29FingerprintMaximumCacheEntries = uint64_t{1} << 20;
constexpr uint64_t kP29FingerprintMaximumPathBytes = uint64_t{1} << 20;

struct P29OwnedFd {
    int value = -1;
    ~P29OwnedFd() {
        if (value >= 0)
            (void)::close(value);
    }
    P29OwnedFd() = default;
    explicit P29OwnedFd(int fd) : value(fd) {}
    P29OwnedFd(const P29OwnedFd&) = delete;
    P29OwnedFd& operator=(const P29OwnedFd&) = delete;
    P29OwnedFd(P29OwnedFd&& other) noexcept : value(other.value) {
        other.value = -1;
    }
    P29OwnedFd& operator=(P29OwnedFd&& other) noexcept {
        if (this != &other) {
            if (value >= 0)
                (void)::close(value);
            value = other.value;
            other.value = -1;
        }
        return *this;
    }
};

struct P29CacheLock {
    P29OwnedFd directory;
    P29OwnedFd lock;

    ~P29CacheLock() {
        if (lock.value >= 0)
            (void)::flock(lock.value, LOCK_UN);
    }

    P29CacheLock() = default;
    P29CacheLock(const P29CacheLock&) = delete;
    P29CacheLock& operator=(const P29CacheLock&) = delete;
    P29CacheLock(P29CacheLock&&) noexcept = default;
    P29CacheLock& operator=(P29CacheLock&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return directory.value >= 0 && lock.value >= 0;
    }
};

void append_u64(std::vector<uint8_t>& output, uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        output.push_back(static_cast<uint8_t>(value >> shift));
}

bool consume_u64(std::span<const uint8_t> input, size_t& cursor,
                 uint64_t& value) noexcept {
    if (input.size() - cursor < 8)
        return false;
    value = 0;
    for (unsigned shift = 0; shift != 64; shift += 8)
        value |= static_cast<uint64_t>(input[cursor++]) << shift;
    return true;
}

P29FingerprintFile inspect_p29_source_file(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0)
        throw std::runtime_error("cannot stat P29 system source");
#if defined(__APPLE__)
    const auto seconds = info.st_mtimespec.tv_sec;
    const auto nanoseconds = info.st_mtimespec.tv_nsec;
#else
    const auto seconds = info.st_mtim.tv_sec;
    const auto nanoseconds = info.st_mtim.tv_nsec;
#endif
    if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000L ||
        static_cast<uint64_t>(seconds) >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) /
                UINT64_C(1000000000))
        throw std::runtime_error("P29 system source timestamp is invalid");
    return P29FingerprintFile{
        path, static_cast<uint64_t>(info.st_size),
        static_cast<int64_t>(seconds) * INT64_C(1000000000) +
            static_cast<int64_t>(nanoseconds),
        {}};
}

std::vector<P29FingerprintFile> enumerate_p29_system_sources() {
    namespace fs = std::filesystem;
    static constexpr std::array<std::string_view, 3> roots{
        "/usr/include/", "/usr/lib/gcc/", "/usr/local/include/"};
    std::vector<P29FingerprintFile> files;
    for (const std::string_view root : roots) {
        std::error_code error;
        if (!fs::exists(root, error)) {
            if (error)
                throw std::runtime_error("cannot inspect P29 system source root");
            continue;
        }
        fs::recursive_directory_iterator current(
            fs::path(root), fs::directory_options::skip_permission_denied, error);
        const fs::recursive_directory_iterator end;
        if (error)
            throw std::runtime_error("cannot enumerate P29 system source root");
        while (current != end) {
            const fs::directory_entry entry = *current;
            const bool regular = entry.is_regular_file(error);
            if (error)
                throw std::runtime_error("cannot stat P29 system source");
            if (regular) {
                const std::string path =
                    entry.path().lexically_normal().generic_string();
                files.push_back(inspect_p29_source_file(path));
            }
            current.increment(error);
            if (error)
                throw std::runtime_error("cannot enumerate P29 system source root");
        }
    }
    std::sort(files.begin(), files.end(),
              [](const P29FingerprintFile& left,
                 const P29FingerprintFile& right) {
                  return left.path < right.path;
              });
    files.erase(std::unique(files.begin(), files.end(),
                            [](const P29FingerprintFile& left,
                               const P29FingerprintFile& right) {
                                return left.path == right.path;
                            }),
                files.end());
    return files;
}

P29CacheLock lock_p29_fingerprint_cache(
    const std::string& cache_directory) noexcept {
    P29CacheLock result;
    if (cache_directory.empty() || cache_directory.front() != '/' ||
        cache_directory.find('\0') != std::string::npos)
        return result;
    result.directory.value = ::open(
        cache_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (result.directory.value < 0)
        return result;
    result.lock.value = ::openat(
        result.directory.value, std::string(kP29FingerprintLockFile).c_str(),
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (result.lock.value < 0)
        return result;
    if (::flock(result.lock.value, LOCK_EX) != 0) {
        (void)::close(result.lock.value);
        result.lock.value = -1;
        return result;
    }
    return result;
}

std::vector<uint8_t> read_p29_cache_bytes(int directory_fd) {
    P29OwnedFd input(::openat(
        directory_fd, std::string(kP29FingerprintCacheFile).c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (input.value < 0)
        return {};
    struct stat info {};
    if (::fstat(input.value, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) >
            kP29FingerprintMaximumCacheBytes)
        return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(info.st_size));
    size_t offset = 0;
    while (offset != bytes.size()) {
        const ssize_t count = ::pread(input.value, bytes.data() + offset,
                                      bytes.size() - offset,
                                      static_cast<off_t>(offset));
        if (count > 0) {
            offset += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return {};
    }
    return bytes;
}

P29FingerprintCache read_p29_fingerprint_cache(int directory_fd) {
    const std::vector<uint8_t> bytes = read_p29_cache_bytes(directory_fd);
    const std::span<const uint8_t> input(bytes);
    if (input.size() < kP29FingerprintCacheMagic.size() + 8 ||
        !std::equal(kP29FingerprintCacheMagic.begin(),
                    kP29FingerprintCacheMagic.end(), input.begin()))
        return {};
    size_t cursor = kP29FingerprintCacheMagic.size();
    uint64_t count = 0;
    if (!consume_u64(input, cursor, count) ||
        count > kP29FingerprintMaximumCacheEntries)
        return {};
    P29FingerprintCache result;
    for (uint64_t index = 0; index != count; ++index) {
        uint64_t path_bytes = 0;
        uint64_t size = 0;
        uint64_t encoded_mtime = 0;
        if (!consume_u64(input, cursor, path_bytes) ||
            !consume_u64(input, cursor, size) ||
            !consume_u64(input, cursor, encoded_mtime) ||
            path_bytes > kP29FingerprintMaximumPathBytes ||
            input.size() - cursor < Digest128{}.bytes.size() ||
            input.size() - cursor - Digest128{}.bytes.size() < path_bytes)
            return {};
        Digest128 digest{};
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(cursor),
                    digest.bytes.size(), digest.bytes.begin());
        cursor += digest.bytes.size();
        std::string path(
            reinterpret_cast<const char*>(input.data() + cursor),
            static_cast<size_t>(path_bytes));
        cursor += static_cast<size_t>(path_bytes);
        if (!result.emplace(P29FingerprintCacheKey{
                                std::move(path), size,
                                static_cast<int64_t>(encoded_mtime)},
                            digest)
                 .second)
            return {};
    }
    return cursor == input.size() ? result : P29FingerprintCache{};
}

bool write_all(int fd, std::span<const uint8_t> bytes) noexcept {
    size_t offset = 0;
    while (offset != bytes.size()) {
        const ssize_t count =
            ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

void persist_p29_fingerprint_cache(
    int directory_fd, const std::vector<P29FingerprintFile>& files) noexcept {
    try {
        std::vector<uint8_t> bytes;
        size_t estimate = kP29FingerprintCacheMagic.size() + 8;
        for (const P29FingerprintFile& file : files) {
            if (file.path.size() > kP29FingerprintMaximumPathBytes ||
                estimate > kP29FingerprintMaximumCacheBytes - 40 -
                               file.path.size())
                return;
            estimate += 40 + file.path.size();
        }
        bytes.reserve(estimate);
        bytes.insert(bytes.end(), kP29FingerprintCacheMagic.begin(),
                     kP29FingerprintCacheMagic.end());
        append_u64(bytes, files.size());
        for (const P29FingerprintFile& file : files) {
            append_u64(bytes, file.path.size());
            append_u64(bytes, file.size);
            append_u64(bytes, static_cast<uint64_t>(file.mtime_ns));
            bytes.insert(bytes.end(), file.digest.bytes.begin(),
                         file.digest.bytes.end());
            bytes.insert(bytes.end(), file.path.begin(), file.path.end());
        }

        static std::atomic<uint64_t> next_temporary{1};
        std::string temporary;
        P29OwnedFd output;
        for (unsigned attempt = 0; attempt != 8 && output.value < 0; ++attempt) {
            temporary = ".p29-system-source-fingerprint-v1.tmp." +
                        std::to_string(static_cast<long long>(::getpid())) + "." +
                        std::to_string(next_temporary.fetch_add(
                            1, std::memory_order_relaxed));
            output.value = ::openat(
                directory_fd, temporary.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        }
        if (output.value < 0)
            return;
        if (!write_all(output.value, bytes) || ::fsync(output.value) != 0) {
            (void)::unlinkat(directory_fd, temporary.c_str(), 0);
            return;
        }
        if (::close(output.value) != 0) {
            output.value = -1;
            (void)::unlinkat(directory_fd, temporary.c_str(), 0);
            return;
        }
        output.value = -1;
        if (::renameat(directory_fd, temporary.c_str(), directory_fd,
                       std::string(kP29FingerprintCacheFile).c_str()) != 0) {
            (void)::unlinkat(directory_fd, temporary.c_str(), 0);
            return;
        }
        (void)::fsync(directory_fd);
    } catch (...) {
    }
}

Digest128 compute_p29_system_source_fingerprint(
    const std::string& cache_directory) {
    P29CacheLock cache_lock =
        lock_p29_fingerprint_cache(cache_directory);
    const P29FingerprintCache cache =
        cache_lock.valid()
            ? read_p29_fingerprint_cache(cache_lock.directory.value)
            : P29FingerprintCache{};
    std::vector<P29FingerprintFile> files = enumerate_p29_system_sources();
    if (files.empty())
        return {};
    for (P29FingerprintFile& file : files) {
        bool stable = false;
        for (unsigned attempt = 0; attempt != 2 && !stable; ++attempt) {
            const auto position = cache.find(P29FingerprintCacheKey{
                file.path, file.size, file.mtime_ns});
            file.digest = position == cache.end()
                              ? hash_regular_file(file.path)
                              : position->second;
            const P29FingerprintFile verified =
                inspect_p29_source_file(file.path);
            stable = verified.size == file.size &&
                     verified.mtime_ns == file.mtime_ns;
            if (!stable) {
                file.size = verified.size;
                file.mtime_ns = verified.mtime_ns;
            }
        }
        if (!stable)
            throw std::runtime_error(
                "P29 system source changed during fingerprinting");
    }
    Digest128Builder fingerprint;
    fingerprint.append("ICECC-P29-SYSTEM-SOURCE-V1");
    fingerprint.append_u64(files.size());
    for (const P29FingerprintFile& file : files) {
        fingerprint.append_u64(file.path.size());
        fingerprint.append(file.path);
        fingerprint.append_digest(file.digest);
    }
    Digest128 result = fingerprint.finish();
    if (result == Digest128{})
        result.bytes.back() = 1;
    if (cache_lock.valid())
        persist_p29_fingerprint_cache(cache_lock.directory.value, files);
    return result;
}

class P29FingerprintState {
public:
    void start(std::string cache_directory) {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::Idle)
            return;
        phase_ = Phase::Running;
        try {
            std::thread([this, cache = std::move(cache_directory)] {
                Digest128 result{};
                try {
                    result = compute_p29_system_source_fingerprint(cache);
                } catch (...) {
                }
                {
                    std::lock_guard publish(mutex_);
                    fingerprint_ = result;
                    phase_ = Phase::Ready;
                }
                ready_.notify_all();
            }).detach();
        } catch (...) {
            fingerprint_ = {};
            phase_ = Phase::Ready;
            ready_.notify_all();
        }
    }

    void wait() {
        std::unique_lock lock(mutex_);
        if (phase_ == Phase::Idle)
            return;
        ready_.wait(lock, [this] { return phase_ == Phase::Ready; });
    }

    [[nodiscard]] Digest128 snapshot() const {
        std::lock_guard lock(mutex_);
        return phase_ == Phase::Ready ? fingerprint_ : Digest128{};
    }

private:
    enum class Phase { Idle, Running, Ready };
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    Phase phase_ = Phase::Idle;
    Digest128 fingerprint_{};
};

P29FingerprintState& p29_fingerprint_state() {
    // The worker is process-scoped and may still be finishing while static
    // teardown begins.  Deliberately retain this tiny synchronization object
    // until process exit so a detached worker never observes a dead owner.
    static P29FingerprintState* state = new P29FingerprintState;
    return *state;
}

class P29MmapInternProvider {
public:
    explicit P29MmapInternProvider(size_t budget) : budget_(budget) {}

    [[noreturn]] static void fail(const char* reason) {
        throw std::length_error(reason);
    }

    bool try_reserve_interner(size_t bytes) {
        if (bytes > budget_ - reserved_)
            return false;
        reserved_ += bytes;
        return true;
    }

    void* allocate_interner(codec::P29InternAllocation, size_t bytes, size_t) {
        if (bytes > reserved_ - live_)
            return nullptr;
        void* result = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (result == MAP_FAILED)
            return nullptr;
#if defined(MADV_HUGEPAGE)
        (void)::madvise(result, bytes, MADV_HUGEPAGE);
#endif
        live_ += bytes;
        return result;
    }

    void deallocate_interner(codec::P29InternAllocation, void* pointer,
                             size_t bytes, size_t) noexcept {
        if (pointer != nullptr)
            (void)::munmap(pointer, bytes);
        live_ -= bytes;
    }

    void release_interner_reservation(size_t bytes) noexcept {
        reserved_ -= bytes;
    }

    void report_interner_usage(size_t reserved, size_t committed) {
        if (reserved != reserved_)
            throw std::logic_error("P29 interner reservation accounting differs");
        committed_ = committed;
    }

    void publish_line(uint32_t, std::span<const uint8_t>) {}
    void publish_region(uint32_t, std::span<const uint32_t>) {}

    [[nodiscard]] size_t reserved() const noexcept { return reserved_; }
    [[nodiscard]] size_t committed() const noexcept { return committed_; }

private:
    size_t budget_ = 0;
    size_t reserved_ = 0;
    size_t live_ = 0;
    size_t committed_ = 0;
};

static_assert(codec::P29InternProvider<P29MmapInternProvider>);

class P29SenderProvider {
public:
    explicit P29SenderProvider(size_t max_tu_bytes) {
        limits_.max_tu_bytes = max_tu_bytes;
        limits_.max_region_bytes = max_tu_bytes;
    }

    [[noreturn]] static void fail(const char* reason) {
        throw std::invalid_argument(reason);
    }

    codec::P29SenderRouteState& sender_route() { return route_; }
    codec::P29SourceTextView source_text(std::string_view path) {
        return sources_.get(path);
    }
    codec::P29WireLimits wire_limits() { return limits_; }

private:
    codec::P29SenderRouteState route_;
    codec::P29WireLimits limits_;
    P29SourceTextCache sources_;
};

static_assert(codec::P29SenderProvider<P29SenderProvider>);

class P29ReceiverProvider {
public:
    explicit P29ReceiverProvider(size_t max_tu_bytes) {
        limits_.max_tu_bytes = max_tu_bytes;
        limits_.max_region_bytes = max_tu_bytes;
    }

    [[noreturn]] static void fail(const char* reason) {
        throw std::invalid_argument(reason);
    }

    codec::P29ReceiverRouteState& receiver_route() { return route_; }
    codec::P29SourceTextView source_text(std::string_view path) {
        return sources_.get(path);
    }
    codec::P29WireLimits wire_limits() { return limits_; }

    void begin_wire_publish() {
        if (publishing_)
            fail("P29 receiver publication is already active");
        publishing_ = true;
    }
    void publish_wire_region(uint32_t, std::span<const uint8_t>) {
        if (!publishing_)
            fail("P29 receiver Region publication is inactive");
    }
    void publish_wire_block(uint32_t, std::span<const uint32_t>) {
        if (!publishing_)
            fail("P29 receiver Block publication is inactive");
    }
    void commit_wire_publish() {
        if (!publishing_)
            fail("P29 receiver publication is inactive");
        publishing_ = false;
    }
    void abandon_wire_publish() noexcept { publishing_ = false; }

private:
    codec::P29ReceiverRouteState route_;
    codec::P29WireLimits limits_;
    P29SourceTextCache sources_;
    bool publishing_ = false;
};

static_assert(codec::P29ReceiverProvider<P29ReceiverProvider>);

uint64_t sender_route_bytes(const codec::P29SenderRouteState& state) noexcept {
    return codec::p29_sender_route_state_bytes(state);
}

struct P29FRouteCodec {
    explicit P29FRouteCodec(size_t max_tu_bytes)
        : provider(max_tu_bytes), deserializer(provider) {}

    P29ReceiverProvider provider;
    codec::P29Deserializer<P29ReceiverProvider> deserializer;
    std::optional<bool> fixed_system_source_reuse;
    bool terminal = false;
};

}  // namespace

void start_p29_system_source_fingerprint(
    std::string cache_directory) noexcept {
    try {
        p29_fingerprint_state().start(std::move(cache_directory));
    } catch (...) {
    }
}

void wait_p29_system_source_fingerprint() noexcept {
    try {
        p29_fingerprint_state().wait();
    } catch (...) {
    }
}

Digest128 p29_system_source_fingerprint() noexcept {
    try {
        return p29_fingerprint_state().snapshot();
    } catch (...) {
        return {};
    }
}

ImmutableObject ImmutableObject::bytes(Key64 key,
                                       std::span<const uint8_t> payload) {
    ImmutableObject result{key, BytesPayload{{payload.begin(), payload.end()}}, {}};
    result.content_digest = object_digest(key.type(), result.payload);
    if (!result.valid_shape()) throw std::invalid_argument("invalid byte object shape");
    return result;
}

ImmutableObject ImmutableObject::children(Key64 key,
                                          std::span<const Key64> payload) {
    ImmutableObject result{key, ChildrenPayload{{payload.begin(), payload.end()}}, {}};
    result.content_digest = object_digest(key.type(), result.payload);
    if (!result.valid_shape()) throw std::invalid_argument("invalid child object shape");
    return result;
}

ImmutableObject ImmutableObject::from_record(const FillRecord& record) {
    if (!record.key.valid()) throw std::invalid_argument("FILL object has invalid Key64");
    ImmutableObject result{record.key,
                           decode_object_payload(record.key.type(), record.object_bytes),
                           record.content_digest};
    if (!result.valid_shape())
        throw std::invalid_argument("FILL object content does not match its digest or type");
    return result;
}

bool ImmutableObject::valid_shape() const {
    if (!key.valid() || content_digest != object_digest(key.type(), payload)) return false;
    if (byte_object(key.type())) return std::holds_alternative<BytesPayload>(payload);
    const auto* children = std::get_if<ChildrenPayload>(&payload);
    if (!children) return false;
    if (key.type() == ObjectType::Block)
        return std::all_of(children->children.begin(), children->children.end(),
                           [](Key64 child) { return child.type() == ObjectType::Region; });
    return key.type() == ObjectType::Region &&
           std::all_of(children->children.begin(), children->children.end(),
                       [](Key64 child) {
                           return child.valid() && child.type() != ObjectType::Region &&
                                  child.type() != ObjectType::Block;
                       });
}

std::vector<uint8_t> ImmutableObject::canonical_payload() const {
    return encode_payload(key.type(), payload);
}

FillRecord ImmutableObject::fill_record() const {
    return {key, content_digest, canonical_payload()};
}

ObjectApplyResult ImmutableObjectStore::apply(const ImmutableObject& object) {
    if (!object.valid_shape()) throw std::invalid_argument("invalid immutable object");
    auto [position, inserted] = objects_.emplace(object.key, object);
    if (inserted) return ObjectApplyResult::Applied;
    if (position->second != object)
        throw std::logic_error("Key64 already names different immutable content");
    return ObjectApplyResult::Duplicate;
}

ObjectApplyResult ImmutableObjectStore::apply(ImmutableObject&& object) {
    if (!object.valid_shape()) throw std::invalid_argument("invalid immutable object");
    const Key64 key = object.key;
    auto [position, inserted] = objects_.try_emplace(key, std::move(object));
    if (inserted) return ObjectApplyResult::Applied;
    if (position->second != object)
        throw std::logic_error("Key64 already names different immutable content");
    return ObjectApplyResult::Duplicate;
}

const ImmutableObject* ImmutableObjectStore::find(Key64 key) const {
    const auto position = objects_.find(key);
    return position == objects_.end() ? nullptr : &position->second;
}

CObjectArena::CObjectArena(CStoreGuid guid, uint16_t generation,
                           uint64_t first_ordinal)
    : guid_(guid), generation_(generation), first_ordinal_(first_ordinal) {
    // XXH3-128 distributes content digests uniformly. A short collision chain
    // keeps this index cheaper than the former tree without a bucket per key.
    content_index_.max_load_factor(4.0F);
    if (generation > KeyLayoutV1::generation_value_mask)
        throw std::invalid_argument("Key64 generation exceeds KeyLayoutV1");
    if (first_ordinal == 0 || first_ordinal > KeyLayoutV1::ordinal_mask)
        throw std::invalid_argument("Key64 first ordinal exceeds KeyLayoutV1");
    next_ordinal_.fill(first_ordinal_);
}

Key64 CObjectArena::allocate(ObjectType type) {
    if (!Key64::make(type, generation_, 1))
        throw std::invalid_argument("unknown Key64 object type");
    const uint8_t type_value = static_cast<uint8_t>(type);
    uint64_t& next = next_ordinal_[type_value];
    if (next == 0 || next > KeyLayoutV1::ordinal_mask)
        throw std::overflow_error("Key64 ordinal space exhausted");
    const std::optional<Key64> key = Key64::make(type, generation_, next);
    if (!key) throw std::invalid_argument("invalid Key64 arena allocation");
    next = next == KeyLayoutV1::ordinal_mask ? 0 : next + 1;
    return *key;
}

std::optional<Key64> CObjectArena::find_equal(ObjectType type,
                                              const ObjectPayload& payload,
                                              Digest128 digest) const {
    const auto position = content_index_.find(digest);
    if (position == content_index_.end()) return std::nullopt;
    for (Key64 key : position->second) {
        const ImmutableObject& object = this->object(key);
        if (key.type() == type && object.payload == payload) return key;
    }
    return std::nullopt;
}

Key64 CObjectArena::install(ObjectType type, ObjectPayload payload) {
    const Digest128 digest = object_digest(type, payload);
    if (const auto existing = find_equal(type, payload, digest)) return *existing;
    const Key64 key = allocate(type);
    ImmutableObject object{key, std::move(payload), digest};
    objects_.apply(std::move(object));
    content_index_[digest].push_back(key);
    return key;
}

Key64 CObjectArena::intern_bytes(ObjectType type,
                                 std::span<const uint8_t> payload) {
    if (!Key64::make(type, generation_, 1))
        throw std::invalid_argument("unknown Key64 object type");
    if (!byte_object(type)) throw std::invalid_argument("object type does not carry bytes");
    return install(type, BytesPayload{{payload.begin(), payload.end()}});
}

Key64 CObjectArena::intern_children(ObjectType type,
                                    std::span<const Key64> payload) {
    const std::optional<Key64> probe = Key64::make(type, generation_, 1);
    if (!probe) throw std::invalid_argument("unknown Key64 object type");
    if (byte_object(type)) throw std::invalid_argument("object type does not carry children");
    ObjectPayload candidate = ChildrenPayload{{payload.begin(), payload.end()}};
    ImmutableObject shape{*probe, candidate, object_digest(type, candidate)};
    if (!shape.valid_shape()) throw std::invalid_argument("invalid child object shape");
    return install(type, std::move(candidate));
}

void CObjectArena::reserve_for_tu(size_t expected_lines) {
    objects_.reserve(expected_lines);
    content_index_.reserve(expected_lines);
}

GenerationAdvanceResult CObjectArena::advance_generation() {
    if (generation_ == KeyLayoutV1::generation_value_mask)
        return GenerationAdvanceResult::GuidFlipRequired;
    ++generation_;
    next_ordinal_.fill(first_ordinal_);
    return GenerationAdvanceResult::Advanced;
}

struct GlobalResourceModel::Object {
    enum class State : uint8_t { Absent, Installing, Present, Pinned };
    State state = State::Absent;
    Digest128 content_digest{};
    uint64_t bytes = 0;
    size_t slot = 0;
    unsigned attempts = 0;
    bool crashed = false;
};

struct GlobalResourceModel::Namespace {
    uint16_t generation = 0;
    bool live = false;
    bool stopped = false;
    bool active = false;
    uint64_t lru = 0;
    std::set<CStoreGuid> guid_history;
    std::map<Key64, Object> objects;
};

namespace {

using GlobalObjectState = GlobalResourceModel::Object::State;

bool checked_add(uint64_t left, uint64_t right, uint64_t limit) {
    return right <= limit && left <= limit - right;
}

}  // namespace

std::string_view global_action_name(GlobalActionType action) {
    switch (action) {
    case GlobalActionType::NAMESPACE_ADMITTED: return "NAMESPACE_ADMITTED";
    case GlobalActionType::NAMESPACE_TOUCHED: return "NAMESPACE_TOUCHED";
    case GlobalActionType::TU_STARTED: return "TU_STARTED";
    case GlobalActionType::TU_FINISHED: return "TU_FINISHED";
    case GlobalActionType::ARENA_INSTALLING: return "ARENA_INSTALLING";
    case GlobalActionType::ARENA_RETRY_INSTALLING: return "ARENA_RETRY_INSTALLING";
    case GlobalActionType::ARENA_PRESENT: return "ARENA_PRESENT";
    case GlobalActionType::ARENA_PINNED: return "ARENA_PINNED";
    case GlobalActionType::ARENA_UNPINNED: return "ARENA_UNPINNED";
    case GlobalActionType::ARENA_RELEASED: return "ARENA_RELEASED";
    case GlobalActionType::INSTALL_CRASHED: return "INSTALL_CRASHED";
    case GlobalActionType::CONTENT_CONFLICT_FATAL: return "CONTENT_CONFLICT_FATAL";
    case GlobalActionType::NAMESPACE_EVICTED: return "NAMESPACE_EVICTED";
    case GlobalActionType::GENERATION_ADVANCED: return "GENERATION_ADVANCED";
    case GlobalActionType::GENERATION_WRAP_STOPPED: return "GENERATION_WRAP_STOPPED";
    case GlobalActionType::C_GUID_FLIPPED: return "C_GUID_FLIPPED";
    }
    throw std::logic_error("unknown Protocol-50 global action");
}

std::string global_action_jsonl(const GlobalActionRecord& record) {
    static constexpr char digits[] = "0123456789abcdef";
    const auto id_hex = [&](const CStoreGuid& id) {
        std::string value;
        value.reserve(id.bytes.size() * 2);
        for (const uint8_t byte : id.bytes) {
            value.push_back(digits[byte >> 4]);
            value.push_back(digits[byte & 0x0f]);
        }
        return value;
    };
    const std::string guid = id_hex(record.c_store_guid);
    std::ostringstream output;
    output << "{\"action\":\"" << global_action_name(record.action)
           << "\",\"c_store_guid\":\"" << guid
           << "\",\"previous_c_store_guid\":\""
           << id_hex(record.previous_c_store_guid)
           << "\",\"generation\":" << record.generation
           << ",\"key64\":" << (record.key.valid() ? record.key.wire_value() : 0)
           << ",\"slot\":" << record.slot
           << ",\"bytes\":" << record.bytes
           << ",\"lru\":" << record.lru
           << ",\"content_digest\":\"" << digest128_hex(record.content_digest)
           << "\"}";
    return output.str();
}

void write_global_trace(const GlobalResourceTrace& trace, const std::string& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open global action trace output");
    for (const GlobalActionRecord& record : trace.records())
        output << global_action_jsonl(record) << '\n';
    if (!output) throw std::runtime_error("cannot write global action trace output");
}

GlobalResourceModel::GlobalResourceModel(GlobalResourceLimits limits,
                                         GlobalResourceFaults faults,
                                         GlobalResourceTrace* trace)
    : limits_(limits), faults_(faults), trace_(trace) {
    if (limits_.max_aggregate_bytes == 0 || limits_.max_namespace_bytes == 0 ||
        limits_.max_staging_bytes == 0 || limits_.max_total_bytes == 0 ||
        limits_.max_staging_slots == 0 ||
        limits_.max_namespace_bytes > limits_.max_aggregate_bytes ||
        limits_.max_aggregate_bytes > limits_.max_total_bytes ||
        limits_.max_generation == 0)
        throw std::invalid_argument("global resource limits are inconsistent");
}

GlobalResourceModel::~GlobalResourceModel() = default;

GlobalResourceModel::Namespace&
GlobalResourceModel::require_namespace(CStoreGuid c_store_guid) {
    const auto position = namespaces_.find(c_store_guid);
    if (position == namespaces_.end()) throw std::logic_error("unknown C namespace");
    return *position->second;
}

const GlobalResourceModel::Namespace&
GlobalResourceModel::require_namespace(CStoreGuid c_store_guid) const {
    const auto position = namespaces_.find(c_store_guid);
    if (position == namespaces_.end()) throw std::logic_error("unknown C namespace");
    return *position->second;
}

void GlobalResourceModel::emit(GlobalActionRecord action) {
    if (trace_) trace_->record(std::move(action));
}

void GlobalResourceModel::admit(CStoreGuid c_store_guid, uint16_t generation) {
    if (c_store_guid == CStoreGuid{})
        throw std::invalid_argument("zero C namespace GUID is reserved");
    auto position = namespaces_.find(c_store_guid);
    if (position == namespaces_.end()) {
        auto inserted = std::make_unique<Namespace>();
        inserted->generation = generation;
        inserted->guid_history.insert(c_store_guid);
        position = namespaces_.emplace(c_store_guid, std::move(inserted)).first;
    }
    Namespace& space = *position->second;
    if (space.live || space.stopped || generation != space.generation ||
        space.generation >= limits_.max_generation)
        throw std::logic_error("namespace admission is not at its current generation");
    space.live = true;
    emit({GlobalActionType::NAMESPACE_ADMITTED, c_store_guid, {}, space.generation});
}

void GlobalResourceModel::touch(CStoreGuid c_store_guid) {
    Namespace& space = require_namespace(c_store_guid);
    if (!space.live) throw std::logic_error("touch of an absent namespace");
    ++clock_;
    space.lru = clock_;
    emit({GlobalActionType::NAMESPACE_TOUCHED, c_store_guid, {}, space.generation,
          {}, 0, 0, space.lru});
}

void GlobalResourceModel::start_tu(CStoreGuid c_store_guid) {
    Namespace& space = require_namespace(c_store_guid);
    if (!space.live || space.active) throw std::logic_error("invalid global TU start");
    space.active = true;
    emit({GlobalActionType::TU_STARTED, c_store_guid, {}, space.generation});
}

void GlobalResourceModel::finish_tu(CStoreGuid c_store_guid) {
    Namespace& space = require_namespace(c_store_guid);
    if (!space.active) throw std::logic_error("invalid global TU finish");
    for (auto& [key, object] : space.objects) {
        if (object.state == GlobalObjectState::Installing)
            throw std::logic_error("cannot finish TU with an installing object");
        if (object.state == GlobalObjectState::Pinned) object.state = GlobalObjectState::Present;
        (void)key;
    }
    space.active = false;
    emit({GlobalActionType::TU_FINISHED, c_store_guid, {}, space.generation});
}

void GlobalResourceModel::begin_install(CStoreGuid c_store_guid, Key64 key,
                                        Digest128 content_digest, uint64_t bytes,
                                        size_t slot, bool retry) {
    Namespace& space = require_namespace(c_store_guid);
    if (!space.live || !space.active || !key.valid() || bytes == 0 ||
        key.generation() != space.generation)
        throw std::logic_error("invalid global install identity");
    size_t installing_objects = 0;
    for (const auto& [unused_key, candidate] : space.objects) {
        (void)unused_key;
        if (candidate.state == GlobalObjectState::Installing)
            ++installing_objects;
    }
    if (installing_objects >= 2)
        throw std::length_error("namespace staging-object bound exceeded");
    if (key.type() == ObjectType::P29Segment) {
        const auto input_key = Key64::make(
            ObjectType::Blob, key.generation(), key.ordinal());
        const auto input = input_key ? space.objects.find(*input_key)
                                     : space.objects.end();
        if (input == space.objects.end() ||
            input->second.state != GlobalObjectState::Installing)
            throw std::logic_error(
                "P29Segment install lacks same-TU Blob staging");
    }
    auto& object = space.objects[key];
    if (object.state != GlobalObjectState::Absent || (!retry && object.attempts != 0) ||
        (retry && (!object.crashed || object.attempts != 1)))
        throw std::logic_error("global install does not start from the required state");
    const auto owner = slots_.find(slot);
    if (owner != slots_.end() && !faults_.ignore_slot_ownership)
        throw std::logic_error("global staging slot is already owned");
    if (slots_.size() >= limits_.max_staging_slots && owner == slots_.end())
        throw std::length_error("global staging slot pool is exhausted");
    const uint64_t staged = staging_bytes();
    if (!faults_.ignore_staging_cap &&
        (bytes > limits_.max_staging_bytes || staged > limits_.max_staging_bytes - bytes))
        throw std::length_error("global staging byte cap exceeded");
    uint64_t next_staged = 0;
    if (!checked_add(staged, bytes, std::numeric_limits<uint64_t>::max()) ||
        (!faults_.ignore_total_cap &&
         !checked_add(resident_bytes(), next_staged = staged + bytes,
                      limits_.max_total_bytes)))
        throw std::length_error("global total byte cap exceeded");
    object.state = GlobalObjectState::Installing;
    object.content_digest = content_digest;
    object.bytes = bytes;
    object.slot = slot;
    object.attempts = retry ? 2 : 1;
    object.crashed = false;
    slots_[slot] = {c_store_guid, key};
    emit({retry ? GlobalActionType::ARENA_RETRY_INSTALLING : GlobalActionType::ARENA_INSTALLING,
          c_store_guid, {}, space.generation, key, static_cast<uint32_t>(slot), bytes, 0,
          content_digest});
}

void GlobalResourceModel::preflight_publish_pair(
    CStoreGuid c_store_guid, Key64 segment_key, size_t segment_slot,
    Digest128 segment_digest, Key64 input_key, size_t input_slot,
    Digest128 input_digest) const {
    const Namespace& space = require_namespace(c_store_guid);
    if (!space.live || !space.active || segment_key == input_key ||
        segment_slot == input_slot ||
        segment_key.type() != ObjectType::P29Segment ||
        input_key.type() != ObjectType::Blob ||
        segment_key.generation() != input_key.generation() ||
        segment_key.ordinal() != input_key.ordinal())
        throw std::logic_error(
            "global pair preflight does not identify one P29V1 TU");

    const auto segment_position = space.objects.find(segment_key);
    const auto input_position = space.objects.find(input_key);
    if (segment_position == space.objects.end() ||
        input_position == space.objects.end() ||
        segment_position->second.state != GlobalObjectState::Installing ||
        input_position->second.state != GlobalObjectState::Installing)
        throw std::logic_error(
            "global pair preflight requires two INSTALLING objects");
    const Object& segment = segment_position->second;
    const Object& input = input_position->second;
    const auto segment_owner = slots_.find(segment_slot);
    const auto input_owner = slots_.find(input_slot);
    if (segment_owner == slots_.end() || input_owner == slots_.end() ||
        segment_owner->second != std::pair{c_store_guid, segment_key} ||
        input_owner->second != std::pair{c_store_guid, input_key})
        throw std::logic_error(
            "global pair preflight lost staging ownership");
    if (segment.content_digest != segment_digest ||
        input.content_digest != input_digest)
        throw std::logic_error(
            "global pair preflight observed changed content");

    uint64_t addition = 0;
    if (!checked_add(segment.bytes, input.bytes,
                     std::numeric_limits<uint64_t>::max()))
        throw std::length_error("global pair byte count overflows");
    addition = segment.bytes + input.bytes;
    uint64_t namespace_bytes = 0;
    for (const auto& [unused, candidate] : space.objects) {
        (void)unused;
        if (candidate.state == GlobalObjectState::Present ||
            candidate.state == GlobalObjectState::Pinned) {
            if (!checked_add(namespace_bytes, candidate.bytes,
                             std::numeric_limits<uint64_t>::max()))
                throw std::length_error(
                    "global namespace resident byte count overflows");
            namespace_bytes += candidate.bytes;
        }
    }
    if (!faults_.ignore_namespace_cap &&
        !checked_add(namespace_bytes, addition, limits_.max_namespace_bytes))
        throw std::length_error("global namespace byte cap exceeded");
    if (!faults_.ignore_aggregate_cap &&
        !checked_add(resident_bytes(), addition, limits_.max_aggregate_bytes))
        throw std::length_error("global aggregate byte cap exceeded");
}

void GlobalResourceModel::publish(CStoreGuid c_store_guid, Key64 key, size_t slot,
                                  Digest128 content_digest) {
    Namespace& space = require_namespace(c_store_guid);
    auto object_position = space.objects.find(key);
    if (object_position == space.objects.end() ||
        object_position->second.state != GlobalObjectState::Installing)
        throw std::logic_error("global publish without INSTALLING object");
    Object& object = object_position->second;
    const auto owner = slots_.find(slot);
    if (owner == slots_.end() || owner->second != std::pair{c_store_guid, key})
        throw std::logic_error("global publish lost staging ownership");
    if (object.content_digest != content_digest)
        throw std::logic_error("global staged content changed before publish");
    uint64_t namespace_bytes = 0;
    for (const auto& [unused, candidate] : space.objects)
        if (candidate.state == GlobalObjectState::Present ||
            candidate.state == GlobalObjectState::Pinned)
            namespace_bytes += candidate.bytes;
    if (!faults_.ignore_namespace_cap &&
        (object.bytes > limits_.max_namespace_bytes ||
         namespace_bytes > limits_.max_namespace_bytes - object.bytes))
        throw std::length_error("global namespace byte cap exceeded");
    if (!faults_.ignore_aggregate_cap &&
        (object.bytes > limits_.max_aggregate_bytes ||
         resident_bytes() > limits_.max_aggregate_bytes - object.bytes))
        throw std::length_error("global aggregate byte cap exceeded");
    object.state = GlobalObjectState::Present;
    slots_.erase(owner);
    emit({GlobalActionType::ARENA_PRESENT, c_store_guid, {}, space.generation, key,
          static_cast<uint32_t>(slot), object.bytes, 0, content_digest});
}

void GlobalResourceModel::pin(CStoreGuid c_store_guid, Key64 key) {
    Namespace& space = require_namespace(c_store_guid);
    auto position = space.objects.find(key);
    if (!space.active || position == space.objects.end() ||
        position->second.state != GlobalObjectState::Present)
        throw std::logic_error("global pin requires an active PRESENT object");
    position->second.state = GlobalObjectState::Pinned;
    emit({GlobalActionType::ARENA_PINNED, c_store_guid, {}, space.generation, key, 0,
          position->second.bytes});
}

void GlobalResourceModel::unpin(CStoreGuid c_store_guid, Key64 key) {
    Namespace& space = require_namespace(c_store_guid);
    auto position = space.objects.find(key);
    if (position == space.objects.end() || position->second.state != GlobalObjectState::Pinned)
        throw std::logic_error("global unpin requires a PINNED object");
    position->second.state = GlobalObjectState::Present;
    emit({GlobalActionType::ARENA_UNPINNED, c_store_guid, {}, space.generation, key, 0,
          position->second.bytes});
}

void GlobalResourceModel::crash_install(CStoreGuid c_store_guid, Key64 key, size_t slot) {
    Namespace& space = require_namespace(c_store_guid);
    auto position = space.objects.find(key);
    const auto owner = slots_.find(slot);
    if (position == space.objects.end() || position->second.state != GlobalObjectState::Installing ||
        owner == slots_.end() || owner->second != std::pair{c_store_guid, key})
        throw std::logic_error("global crash does not identify the installing owner");
    const uint64_t bytes = position->second.bytes;
    position->second = Object{};
    position->second.crashed = true;
    position->second.attempts = 1;
    slots_.erase(owner);
    emit({GlobalActionType::INSTALL_CRASHED, c_store_guid, {}, space.generation, key,
          static_cast<uint32_t>(slot), bytes});
}

[[noreturn]] void GlobalResourceModel::conflict(CStoreGuid c_store_guid, Key64 key,
                                                Digest128 content_digest) {
    Namespace& space = require_namespace(c_store_guid);
    const auto position = space.objects.find(key);
    if (position == space.objects.end() ||
        (position->second.state != GlobalObjectState::Present &&
         position->second.state != GlobalObjectState::Pinned) ||
        position->second.content_digest == content_digest)
        throw std::logic_error("global content conflict lacks an immutable existing object");
    emit({GlobalActionType::CONTENT_CONFLICT_FATAL, c_store_guid, {}, space.generation, key,
          0, position->second.bytes, 0, content_digest});
    throw std::logic_error("same Key64 names different immutable content");
}

void GlobalResourceModel::evict(CStoreGuid c_store_guid) {
    Namespace& victim = require_namespace(c_store_guid);
    if (!victim.live || victim.active) throw std::logic_error("global eviction requires idle namespace");
    for (const auto& [key, object] : victim.objects)
        if (object.state == GlobalObjectState::Installing || object.state == GlobalObjectState::Pinned)
            throw std::logic_error("global eviction crossed a namespace lease");
    if (!faults_.ignore_lru) {
        for (const auto& [guid, candidate] : namespaces_) {
            if (guid == c_store_guid || !candidate->live || candidate->active || candidate->lru >= victim.lru)
                continue;
            bool eligible = true;
            for (const auto& [key, object] : candidate->objects)
                eligible = eligible && object.state != GlobalObjectState::Installing &&
                           object.state != GlobalObjectState::Pinned;
            if (eligible)
                throw std::logic_error("global eviction selected a non-LRU namespace");
        }
    }
    last_eviction_lru_ = victim.lru;
    victim.live = false;
    victim.objects.clear();
    emit({GlobalActionType::NAMESPACE_EVICTED, c_store_guid, {}, victim.generation, {}, 0, 0,
          victim.lru});
}

CStoreGuid GlobalResourceModel::evict_oldest() {
    std::optional<std::pair<CStoreGuid, uint64_t>> oldest;
    for (const auto& [guid, space] : namespaces_) {
        if (!space->live || space->active) continue;
        bool eligible = true;
        for (const auto& [key, object] : space->objects)
            eligible = eligible && object.state != GlobalObjectState::Installing &&
                       object.state != GlobalObjectState::Pinned;
        if (eligible && (!oldest || space->lru < oldest->second)) oldest = {guid, space->lru};
    }
    if (!oldest) throw std::logic_error("no eligible global namespace to evict");
    evict(oldest->first);
    return oldest->first;
}

void GlobalResourceModel::advance_generation(CStoreGuid c_store_guid) {
    Namespace& space = require_namespace(c_store_guid);
    if (space.live || space.stopped || space.generation >= limits_.max_generation)
        throw std::logic_error("global generation cannot advance");
    ++space.generation;
    emit({GlobalActionType::GENERATION_ADVANCED, c_store_guid, {}, space.generation});
}

void GlobalResourceModel::stop_generation_wrap(CStoreGuid c_store_guid) {
    Namespace& space = require_namespace(c_store_guid);
    if (space.live || space.stopped || space.generation != limits_.max_generation)
        throw std::logic_error("global generation wrap stop is not admissible");
    space.stopped = true;
    emit({GlobalActionType::GENERATION_WRAP_STOPPED, c_store_guid, {}, space.generation});
}

void GlobalResourceModel::flip_guid(CStoreGuid old_guid, CStoreGuid new_guid) {
    Namespace& old = require_namespace(old_guid);
    if (!old.stopped || new_guid == CStoreGuid{} || namespaces_.contains(new_guid))
        throw std::logic_error("global GUID flip does not establish a fresh namespace");
    auto replacement = std::make_unique<Namespace>();
    replacement->guid_history = old.guid_history;
    replacement->guid_history.insert(new_guid);
    namespaces_.emplace(new_guid, std::move(replacement));
    emit({GlobalActionType::C_GUID_FLIPPED, new_guid, old_guid, 0});
}

std::optional<std::string> GlobalResourceModel::check_invariants() const {
    const uint64_t resident = resident_bytes();
    const uint64_t staging = staging_bytes();
    if (resident > limits_.max_aggregate_bytes) return "aggregate resident byte cap exceeded";
    if (staging > limits_.max_staging_bytes) return "aggregate staging byte cap exceeded";
    if (!checked_add(resident, staging, limits_.max_total_bytes))
        return "total simultaneous byte cap exceeded";
    std::map<size_t, std::pair<CStoreGuid, Key64>> reverse;
    for (const auto& [guid, space] : namespaces_) {
        uint64_t namespace_bytes = 0;
        size_t installing_objects = 0;
        for (const auto& [key, object] : space->objects) {
            if (object.state == GlobalObjectState::Present || object.state == GlobalObjectState::Pinned)
                namespace_bytes += object.bytes;
            if (object.state == GlobalObjectState::Installing) {
                ++installing_objects;
                if (!reverse.emplace(object.slot, std::pair{guid, key}).second)
                    return "multiple INSTALLING objects own one staging slot";
                const auto owner = slots_.find(object.slot);
                if (owner == slots_.end() || owner->second != std::pair{guid, key})
                    return "INSTALLING object does not own its staging slot";
                if (key.type() == ObjectType::P29Segment) {
                    const auto input_key = Key64::make(
                        ObjectType::Blob, key.generation(), key.ordinal());
                    const auto input =
                        input_key ? space->objects.find(*input_key)
                                  : space->objects.end();
                    if (input == space->objects.end() ||
                        input->second.state != GlobalObjectState::Installing)
                        return "P29Segment INSTALLING object lacks same-TU Blob";
                }
            }
        }
        if (installing_objects > 2)
            return "namespace has more than two staged objects";
        if (namespace_bytes > limits_.max_namespace_bytes) return "namespace byte cap exceeded";
    }
    if (reverse.size() != slots_.size()) return "staging slot has no INSTALLING owner";
    for (const auto& [slot, owner] : slots_)
        if (!reverse.contains(slot) || reverse.at(slot) != owner)
            return "staging slot reverse ownership mismatch";
    if (last_eviction_lru_)
        for (const auto& [guid, space] : namespaces_)
            if (space->live && !space->active && space->lru < *last_eviction_lru_)
                return "global eviction violated LRU ordering";
    return std::nullopt;
}

uint64_t GlobalResourceModel::resident_bytes() const {
    uint64_t total = 0;
    for (const auto& [guid, space] : namespaces_)
        for (const auto& [key, object] : space->objects)
            if (object.state == GlobalObjectState::Present || object.state == GlobalObjectState::Pinned)
                total += object.bytes;
    return total;
}

uint64_t GlobalResourceModel::staging_bytes() const {
    uint64_t total = 0;
    for (const auto& [guid, space] : namespaces_)
        for (const auto& [key, object] : space->objects)
            if (object.state == GlobalObjectState::Installing) total += object.bytes;
    return total;
}

size_t GlobalResourceModel::live_namespace_count() const {
    size_t count = 0;
    for (const auto& [guid, space] : namespaces_)
        if (space->live) ++count;
    return count;
}

size_t GlobalResourceModel::free_staging_slots() const {
    return limits_.max_staging_slots - slots_.size();
}

std::optional<size_t> GlobalResourceModel::first_free_staging_slot() const {
    for (size_t slot = 0; slot < limits_.max_staging_slots; ++slot)
        if (!slots_.contains(slot)) return slot;
    return std::nullopt;
}

bool GlobalResourceModel::install_retry_required(
    CStoreGuid c_store_guid, Key64 key) const {
    const Namespace& space = require_namespace(c_store_guid);
    const auto position = space.objects.find(key);
    return position != space.objects.end() &&
           position->second.state == GlobalObjectState::Absent &&
           position->second.crashed && position->second.attempts == 1;
}

void GlobalResourceModel::release(CStoreGuid c_store_guid, Key64 key) {
    Namespace& space = require_namespace(c_store_guid);
    const auto position = space.objects.find(key);
    // Zero-byte transactions are legal in the endpoint protocol and have no
    // global resident object to release.
    if (position == space.objects.end()) return;
    if (position->second.state != GlobalObjectState::Present &&
        position->second.state != GlobalObjectState::Pinned)
        throw std::logic_error("global release requires a resident object");
    const uint64_t bytes = position->second.bytes;
    space.objects.erase(position);
    emit({GlobalActionType::ARENA_RELEASED, c_store_guid, {}, space.generation,
          key, 0, bytes});
}

const ImmutableObject& CObjectArena::object(Key64 key) const {
    const ImmutableObject* result = objects_.find(key);
    if (!result) throw std::out_of_range("Key64 is absent from C object arena");
    return *result;
}

struct CAuthority::P29V1State {
    static codec::P29InternLayout select_layout(uint64_t budget) {
        const codec::P29InternLayout firefox = codec::P29InternLayout::firefox();
        if (codec::p29_interner_reservation(firefox) <= budget)
            return firefox;
        const codec::P29InternLayout probe = codec::P29InternLayout::probe();
        if (codec::p29_interner_reservation(probe) <= budget)
            return probe;
        throw std::length_error("P29V1 interner budget admits no production layout");
    }

    P29V1State(uint64_t budget, uint64_t max_tu)
        : provider(static_cast<size_t>(std::min<uint64_t>(
              budget, std::numeric_limits<size_t>::max()))),
          layout(select_layout(budget)), interner(provider, layout),
          max_tu_bytes(max_tu) {}

    P29MmapInternProvider provider;
    codec::P29InternLayout layout;
    codec::P29Interner<P29MmapInternProvider> interner;
    uint64_t max_tu_bytes = 0;
    bool runnable = true;
};

CAuthority::CAuthority(CStoreGuid guid, p29::OnlineS1::Config config,
                       TuSeq first_tu_seq)
    : guid_(guid), s1_config_(config),
      next_tu_seq_(first_tu_seq.value) {}

CAuthority::~CAuthority() = default;

void CAuthority::enable_p29v1(uint64_t max_interner_reserved_bytes,
                              uint64_t max_tu_bytes) {
    if (p29v1_)
        throw std::logic_error("P29V1 interner was already enabled");
    if (max_tu_bytes == 0 || max_tu_bytes > std::numeric_limits<size_t>::max())
        throw std::length_error("P29V1 TU limit is not addressable");
    p29v1_ = std::make_unique<P29V1State>(max_interner_reserved_bytes,
                                         max_tu_bytes);
}

bool CAuthority::p29v1_runnable() const noexcept {
    return p29v1_ && p29v1_->runnable;
}

uint64_t CAuthority::p29v1_interner_reserved_bytes() const noexcept {
    return p29v1_ ? p29v1_->provider.reserved() : 0;
}

uint64_t CAuthority::p29v1_interner_committed_bytes() const noexcept {
    return p29v1_ ? p29v1_->provider.committed() : 0;
}

PreparedTUPtr CAuthority::prepare_p29v1_at_seq(
    std::span<const uint8_t> exact_input, TuSeq tu_seq,
    Digest128 exact_digest) {
    if (!p29v1_ || !p29v1_->runnable)
        throw std::logic_error("P29V1 is not runnable until daemon restart");
    if (exact_input.size() > p29v1_->max_tu_bytes)
        throw std::length_error("P29V1 input exceeds its TU limit");
    try {
        auto prepared = std::make_shared<PreparedTU>();
        prepared->dense_regions.reserve(exact_input.size() / 32 + 1);
        p29v1_->interner.process(exact_input, prepared->dense_regions);
        size_t offset = 0;
        for (const uint32_t id : prepared->dense_regions) {
            const std::span<const uint8_t> region =
                p29v1_->interner.region_bytes(id);
            if (region.size() > exact_input.size() - offset ||
                (!region.empty() &&
                 std::memcmp(region.data(), exact_input.data() + offset,
                             region.size()) != 0))
                throw std::logic_error(
                    "P29V1 interner materialization differs from exact input");
            offset += region.size();
        }
        if (offset != exact_input.size())
            throw std::logic_error(
                "P29V1 interner materialization length differs");
        prepared->tu_seq = tu_seq;
        prepared->raw_bytes = exact_input.size();
        prepared->raw_digest = exact_digest;
        return prepared;
    } catch (...) {
        p29v1_->runnable = false;
        throw;
    }
}

TuSeq CAuthority::reserve_tu_seq() const {
    if (tu_seq_exhausted_)
        throw std::overflow_error("TU_SEQ space exhausted");
    return TuSeq{next_tu_seq_};
}

void CAuthority::commit_tu_seq(TuSeq reserved) {
    if (tu_seq_exhausted_ || reserved.value != next_tu_seq_)
        throw std::logic_error("TU_SEQ reservation is no longer current");
    if (next_tu_seq_ == std::numeric_limits<uint64_t>::max())
        tu_seq_exhausted_ = true;
    else
        ++next_tu_seq_;
}

struct CRoute::P29V1State {
    P29V1State(CAuthority& authority, uint64_t max_route_bytes)
        : provider(static_cast<size_t>(authority.p29v1_->max_tu_bytes)),
          serializer(provider, authority.p29v1_->interner,
                     authority.s1_config(), authority.block_catalogue()),
          max_route_state_bytes(max_route_bytes) {}

    P29SenderProvider provider;
    codec::P29Serializer<P29SenderProvider,
                         codec::P29Interner<P29MmapInternProvider>> serializer;
    std::optional<bool> fixed_system_source_reuse;
    std::vector<uint8_t> answered_need;
    uint64_t max_route_state_bytes = 0;
    bool fill_answered = false;
    bool terminal = false;
};

CRoute::CRoute(CAuthority& authority, FStoreGuid f_store_guid,
               HistoryNonce history_nonce, ActionTrace* trace)
    : authority_(authority), f_store_guid_(f_store_guid),
      history_nonce_(history_nonce),
      state_digest_(initial_route_digest(authority.guid(), history_nonce)),
      trace_(trace) {}

CRoute::~CRoute() = default;

uint64_t CRoute::p29v1_route_state_bytes() const noexcept {
    return p29v1_ ? sender_route_bytes(p29v1_->provider.sender_route()) : 0;
}

std::optional<bool> CRoute::p29v1_system_source_reuse() const noexcept {
    return p29v1_ ? p29v1_->fixed_system_source_reuse : std::nullopt;
}

const CActiveTx& CRoute::begin_v1(
    const PreparedTUPtr& prepared, uint64_t max_route_state_bytes) {
    if (!prepared)
        throw std::invalid_argument("cannot route a null P29V1 PreparedTU");
    if (active_)
        throw std::logic_error("C route already has one ACTIVE_TX");
    if (next_rel_seq_.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("REL_SEQ space exhausted");
    if (!authority_.p29v1_runnable())
        throw std::logic_error("P29V1 is not runnable until daemon restart");
    if (!p29v1_)
        p29v1_ = std::make_unique<P29V1State>(authority_, max_route_state_bytes);
    if (p29v1_->terminal)
        throw std::logic_error("P29V1 route is not runnable until daemon restart");
    if (p29v1_->max_route_state_bytes != max_route_state_bytes)
        throw std::logic_error("P29V1 route-state limit changed");

    try {
        CActiveTx active;
        active.prepared = prepared;
        active.body = p29v1_->serializer.begin_tu(prepared->dense_regions);
        active.region_count = prepared->dense_regions.size();
        active.begin.history_nonce = history_nonce_;
        active.begin.rel_seq = next_rel_seq_;
        active.begin.tu_seq = prepared->tu_seq;
        active.begin.profile = ProfileId::P29V1;
        active.begin.pre_state_digest = state_digest_;
        active.begin.body = describe_component(
            static_cast<uint16_t>(ProfileId::P29V1), active.body,
            p29v1_->serializer.root_reference_count());
        active.begin.raw_bytes = prepared->raw_bytes;
        active.begin.raw_digest = prepared->raw_digest;
        active.begin.transaction_digest = compute_transaction_digest(
            active.begin, active.body);
        active_ = std::move(active);
        p29v1_->answered_need.clear();
        p29v1_->fill_answered = false;
        record(ActionType::TX_BEGIN, *active_);
        return *active_;
    } catch (...) {
        if (p29v1_->serializer.has_pending()) {
            try {
                p29v1_->serializer.abandon();
            } catch (...) {
            }
        }
        p29v1_->terminal = true;
        active_.reset();
        throw;
    }
}

void CRoute::pin_v1_system_source_reuse(bool reuse) {
    if (!p29v1_ || p29v1_->terminal)
        throw std::logic_error("P29V1 route is unavailable for reuse pinning");
    if (p29v1_->fixed_system_source_reuse &&
        *p29v1_->fixed_system_source_reuse != reuse) {
        p29v1_->terminal = true;
        throw std::logic_error(
            "P29V1 system-source reuse changed on the route");
    }
    p29v1_->fixed_system_source_reuse = reuse;
    p29v1_->provider.sender_route().system_source_reuse = reuse;
}

std::span<const uint8_t> CRoute::build_fill_v1(
    std::span<const uint8_t> inner_need) {
    if (!active_ || active_->begin.profile != ProfileId::P29V1 || !p29v1_ ||
        p29v1_->terminal)
        throw std::logic_error("C route has no runnable P29V1 ACTIVE_TX");
    try {
        if (inner_need.size() < 5 ||
            inner_need[inner_need.size() - 5] !=
                static_cast<uint8_t>(codec::P29WireKind::TuEnd) ||
            std::any_of(inner_need.end() - 4, inner_need.end(),
                        [](uint8_t value) { return value != 0; }))
            throw std::invalid_argument("P29V1 NEED has no exact TU_END");
        if (!p29v1_->fixed_system_source_reuse)
            throw std::logic_error(
                "P29V1 system-source reuse was not pinned at HISTORY_RESET");

        if (p29v1_->fill_answered) {
            if (!std::equal(p29v1_->answered_need.begin(),
                            p29v1_->answered_need.end(), inner_need.begin(),
                            inner_need.end()))
                throw std::logic_error("P29V1 replay NEED differs");
            return p29v1_->serializer.captured_fill();
        }
        const std::span<const uint8_t> codec_need =
            inner_need.first(inner_need.size() - 5);
        const std::vector<uint8_t>& fill =
            p29v1_->serializer.answer_need(codec_need, false);
        p29v1_->answered_need.assign(inner_need.begin(), inner_need.end());
        p29v1_->fill_answered = true;
        if (p29v1_->serializer.pending_route_state_bytes() >
            p29v1_->max_route_state_bytes)
            throw std::length_error("P29V1 route state exceeds its budget");
        return fill;
    } catch (...) {
        if (p29v1_->serializer.has_pending()) {
            try {
                p29v1_->serializer.abandon();
            } catch (...) {
            }
        }
        p29v1_->terminal = true;
        active_.reset();
        throw;
    }
}

void CRoute::restart_v1_for_transport_retry() {
    if (!active_ || active_->begin.profile != ProfileId::P29V1 || !p29v1_ ||
        p29v1_->terminal)
        throw std::logic_error(
            "C route has no runnable P29V1 transaction to retry");
    try {
        if (p29v1_->serializer.has_pending())
            p29v1_->serializer.abandon();
        const std::vector<uint8_t> body =
            p29v1_->serializer.begin_tu(active_->prepared->dense_regions);
        if (body != active_->body ||
            p29v1_->serializer.root_reference_count() !=
                active_->begin.body.decoded_bytes)
            throw std::logic_error(
                "P29V1 transport retry changed the frozen BODY");
        p29v1_->answered_need.clear();
        p29v1_->fill_answered = false;
    } catch (...) {
        if (p29v1_->serializer.has_pending()) {
            try {
                p29v1_->serializer.abandon();
            } catch (...) {
            }
        }
        p29v1_->terminal = true;
        active_.reset();
        throw;
    }
}

void CRoute::reset_v1_route(FStoreGuid f_store_guid,
                            HistoryNonce history_nonce) {
    if (!active_ || active_->begin.profile != ProfileId::P29V1)
        throw std::logic_error(
            "C route has no active P29V1 transaction to reset");
    reset_history(f_store_guid, history_nonce);
}

void CRoute::record(ActionType action, const CActiveTx& active) {
    if (!trace_) return;
    ActionRecord record;
    record.action = action;
    record.actor = ActorSide::C;
    record.c_store_guid = authority_.guid();
    record.f_store_guid = f_store_guid_;
    record.history_nonce = active.begin.history_nonce;
    record.rel_seq = active.begin.rel_seq;
    record.tu_seq = active.begin.tu_seq;
    record.transaction_digest = active.begin.transaction_digest;
    record.raw_digest = active.begin.raw_digest;
    record.state_digest =
        action == ActionType::COMMIT_ACCEPTED ||
                action == ActionType::LOST_COMMIT_ACCEPTED
            ? compute_post_state_digest(active.begin.pre_state_digest,
                                        active.begin.history_nonce,
                                        active.begin.rel_seq,
                                        active.begin.tu_seq,
                                        active.begin.transaction_digest)
            : active.begin.pre_state_digest;
    trace_->record(std::move(record));
}

void CRoute::accept_commit(const TxCommit& committed, ActionType action) {
    if (!active_) throw std::logic_error("C route has no ACTIVE_TX to commit");
    if (action != ActionType::COMMIT_ACCEPTED &&
        action != ActionType::LOST_COMMIT_ACCEPTED)
        throw std::invalid_argument("invalid commit action");
    if (!same_commit(committed, *active_))
        throw std::logic_error("TX_COMMIT does not close C's ACTIVE_TX");
    if (active_->begin.profile != ProfileId::P29V1 || !p29v1_ ||
        p29v1_->terminal)
        throw std::logic_error("P29V1 route cannot accept a commit");
    try {
        p29v1_->serializer.commit();
        if (p29v1_route_state_bytes() > p29v1_->max_route_state_bytes)
            throw std::logic_error(
                "P29V1 committed route state exceeded its preflight");
    } catch (...) {
        p29v1_->terminal = true;
        throw;
    }
    state_digest_ = committed.post_state_digest;
    record(action, *active_);
    ++next_rel_seq_.value;
    active_.reset();
}

void CRoute::abandon_active() {
    if (!active_) return;
    record(ActionType::TX_ABORTED, *active_);
    if (p29v1_ && p29v1_->serializer.has_pending())
        p29v1_->serializer.abandon();
    active_.reset();
}

void CRoute::reset_history(FStoreGuid f_store_guid, HistoryNonce history_nonce) {
    if (history_nonce == history_nonce_)
        throw std::invalid_argument("history reset requires a fresh HISTORY_NONCE");
    abandon_active();
    f_store_guid_ = f_store_guid;
    history_nonce_ = history_nonce;
    next_rel_seq_ = RelSeq{0};
    state_digest_ = initial_route_digest(authority_.guid(), history_nonce_);
    p29v1_.reset();
}

struct FStore::Namespace {
    struct FPending {
        explicit FPending(TxBegin value) : begin(std::move(value)) {}
        TxBegin begin;
        std::vector<uint8_t> body;
        bool body_complete = false;
        std::vector<uint8_t> p29v1_need;
        std::vector<uint8_t> p29v1_fill;
        Digest128 p29v1_segment_digest{};
        bool p29v1_materialized = false;
    };

    struct Route {
        HistoryNonce history_nonce{};
        RelSeq next_rel_seq{};
        Digest128 state_digest{};
        std::optional<TxCommit> last_commit;
        std::optional<FPending> pending;
        std::unique_ptr<P29FRouteCodec> p29v1;
    };

    bool established = false;
    uint64_t active_session_serial = 0;
    std::optional<Route> route;
};

FStore::FStore(FStoreGuid guid, uint64_t first_session_serial, ActionTrace* trace,
               uint64_t p29v1_max_tu_bytes,
               bool p29v1_system_source_reuse)
    : guid_(guid), next_session_serial_(first_session_serial), trace_(trace),
      p29v1_max_tu_bytes_(p29v1_max_tu_bytes),
      p29v1_system_source_reuse_(p29v1_system_source_reuse) {
    if (first_session_serial == 0)
        throw std::invalid_argument("first session serial must be nonzero");
    if (p29v1_max_tu_bytes_ == 0 ||
        p29v1_max_tu_bytes_ > std::numeric_limits<size_t>::max())
        throw std::invalid_argument("P29V1 F-store TU limit is not addressable");
}

FStore::~FStore() = default;

void FStore::abandon_pending(Namespace& space) noexcept {
    if (!space.route || !space.route->pending)
        return;
    Namespace::Route& route = *space.route;
    if (route.pending->begin.profile == ProfileId::P29V1 && route.p29v1 &&
        route.p29v1->deserializer.has_pending()) {
        try {
            route.p29v1->deserializer.abandon();
        } catch (...) {
            route.p29v1->terminal = true;
        }
    }
    route.pending.reset();
}

void FStore::record(ActionType action, SessionHandle session, const TxBegin* begin,
                    std::optional<Key64> key, Digest128 content_digest,
                    uint64_t remaining_need,
                    bool duplicate, std::span<const Key64> need_keys) {
    if (!trace_) return;
    ActionRecord record;
    record.action = action;
    record.actor = ActorSide::F;
    record.c_store_guid = session.c_store_guid;
    record.f_store_guid = guid_;
    record.session_serial = session.serial;
    record.key = key;
    record.content_digest = content_digest;
    record.need_keys.assign(need_keys.begin(), need_keys.end());
    record.remaining_need = remaining_need;
    record.duplicate = duplicate;
    const auto position = namespaces_.find(session.c_store_guid);
    if (position != namespaces_.end() && position->second->route) {
        record.history_nonce = position->second->route->history_nonce;
        record.rel_seq = position->second->route->next_rel_seq;
        record.state_digest = position->second->route->state_digest;
    }
    if (begin) {
        record.history_nonce = begin->history_nonce;
        record.rel_seq = begin->rel_seq;
        record.tu_seq = begin->tu_seq;
        record.transaction_digest = begin->transaction_digest;
        record.raw_digest = begin->raw_digest;
        if (action != ActionType::INPUT_COMMITTED)
            record.state_digest = begin->pre_state_digest;
    }
    trace_->record(std::move(record));
}

SessionHandle FStore::connect(CStoreGuid c_store_guid) {
    if (session_serial_exhausted_)
        throw std::overflow_error("session serial space exhausted until F store reset");
    const uint64_t serial = next_session_serial_;
    if (serial == std::numeric_limits<uint64_t>::max())
        session_serial_exhausted_ = true;
    else
        ++next_session_serial_;
    auto [position, inserted] = namespaces_.try_emplace(c_store_guid);
    if (inserted) position->second = std::make_unique<Namespace>();
    Namespace& space = *position->second;
    const bool replaces = space.active_session_serial != 0;
    abandon_pending(space);
    space.active_session_serial = serial;
    const SessionHandle session{c_store_guid, guid_, serial};
    record(replaces ? ActionType::SESSION_REPLACED : ActionType::SESSION_OPENED,
           session, nullptr);
    return session;
}

void FStore::disconnect(SessionHandle session) {
    Namespace& space = require_namespace(session);
    abandon_pending(space);
    record(ActionType::SESSION_DISCONNECTED, session, nullptr);
    space.active_session_serial = 0;
}

SessionState FStore::resume(SessionHandle session) const {
    const Namespace& space = require_namespace(session);
    SessionState result;
    result.f_store_guid = guid_;
    result.namespace_present = space.established;
    result.route_present = space.route.has_value();
    if (space.route) {
        result.history_nonce = space.route->history_nonce;
        result.next_rel_seq = space.route->next_rel_seq;
        result.state_digest = space.route->state_digest;
        result.last_commit = space.route->last_commit;
    }
    return result;
}

void FStore::start_route(SessionHandle session, HistoryNonce history_nonce,
                         Digest128 initial_state_digest) {
    Namespace& space = require_namespace(session);
    if (space.route) throw std::logic_error("F route already exists");
    if (initial_state_digest !=
        initial_route_digest(session.c_store_guid, history_nonce))
        throw std::logic_error("HISTORY_RESET initial digest was not derived from its route");
    space.established = true;
    space.route = Namespace::Route{};
    space.route->history_nonce = history_nonce;
    space.route->state_digest = initial_state_digest;
    record(ActionType::HISTORY_RESET, session, nullptr);
}

void FStore::begin(SessionHandle session, const TxBegin& begin, bool replay) {
    Namespace& space = require_namespace(session);
    if (!space.established || !space.route)
        throw std::logic_error("F route has not been established");
    Namespace::Route& route = *space.route;
    if (begin.history_nonce != route.history_nonce || begin.rel_seq != route.next_rel_seq ||
        begin.pre_state_digest != route.state_digest)
        throw std::logic_error("TX_BEGIN does not match F's route cursor");
    if (begin.profile != ProfileId::P29V1 ||
        begin.body.encoding != static_cast<uint16_t>(ProfileId::P29V1) ||
        begin.raw_bytes > p29v1_max_tu_bytes_)
        throw std::invalid_argument("P29V1 TX_BEGIN descriptors are invalid");
    if (route.next_rel_seq.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("F REL_SEQ space exhausted");
    if (route.pending) {
        if (!same_begin(route.pending->begin, begin))
            throw std::logic_error("F route already has a different ACTIVE_TX");
        return;
    }
    if (!route.p29v1)
        route.p29v1 = std::make_unique<P29FRouteCodec>(
            static_cast<size_t>(p29v1_max_tu_bytes_));
    if (route.p29v1->terminal)
        throw std::logic_error("P29V1 receiver is terminal until route reset");
    const bool reuse = p29v1_system_source_reuse_;
    if (route.p29v1->fixed_system_source_reuse &&
        *route.p29v1->fixed_system_source_reuse != reuse) {
        route.p29v1->terminal = true;
        throw std::logic_error("P29V1 system-source reuse changed on the route");
    }
    if (!route.p29v1->fixed_system_source_reuse)
        route.p29v1->fixed_system_source_reuse = reuse;
    route.p29v1->provider.receiver_route().system_source_reuse = reuse;
    route.pending.emplace(begin);
    record(replay ? ActionType::ACTIVE_REPLAYED : ActionType::TX_BEGIN,
           session, &begin);
    if (begin.body.encoded_bytes == 0)
        append_component(session, std::span<const uint8_t>{});
}

void FStore::append_component(SessionHandle session,
                              std::span<const uint8_t> bytes) {
    Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending)
        throw std::logic_error("BODY data has no F ACTIVE_TX");
    Namespace::FPending& pending = *space.route->pending;
    const ComponentDescriptor& descriptor = pending.begin.body;
    if (pending.body_complete) {
        if (!bytes.empty())
            throw std::logic_error("BODY received bytes after completion");
        return;
    }
    if (pending.body.size() > descriptor.encoded_bytes ||
        bytes.size() > descriptor.encoded_bytes - pending.body.size())
        throw std::length_error("BODY exceeds its declared byte count");
    pending.body.insert(pending.body.end(), bytes.begin(), bytes.end());
    if (pending.body.size() != descriptor.encoded_bytes)
        return;
    if (digest128(pending.body) != descriptor.digest)
        throw std::logic_error("BODY digest does not match TX_BEGIN");
    pending.body_complete = true;
    if (!space.route->p29v1 || space.route->p29v1->terminal)
        throw std::logic_error("P29V1 receiver state is unavailable");
    try {
        pending.p29v1_need =
            space.route->p29v1->deserializer.receive_body(pending.body);
        pending.p29v1_need.push_back(
            static_cast<uint8_t>(codec::P29WireKind::TuEnd));
        pending.p29v1_need.insert(pending.p29v1_need.end(), 4, 0);
        if (space.route->p29v1->deserializer.root_reference_count() !=
            descriptor.decoded_bytes)
            throw std::logic_error(
                "P29V1 BODY decoded count differs from its descriptor");
        if (compute_transaction_digest(pending.begin, pending.body) !=
            pending.begin.transaction_digest)
            throw std::logic_error(
                "P29V1 transaction digest does not match its BODY");
        record(ActionType::BODY_COMPLETE, session, &pending.begin);
        record(ActionType::NEED_RECORDED, session, &pending.begin);
    } catch (...) {
        space.route->p29v1->terminal = true;
        if (space.route->p29v1->deserializer.has_pending()) {
            try {
                space.route->p29v1->deserializer.abandon();
            } catch (...) {
            }
        }
        throw;
    }
}

void FStore::append_body(SessionHandle session, std::span<const uint8_t> bytes) {
    append_component(session, bytes);
}

std::vector<uint8_t> FStore::p29v1_need_frames(SessionHandle session) const {
    const Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending ||
        space.route->pending->begin.profile != ProfileId::P29V1 ||
        !space.route->pending->body_complete ||
        space.route->pending->p29v1_need.empty())
        throw std::logic_error("P29V1 NEED is unavailable before exact BODY");
    return space.route->pending->p29v1_need;
}

bool FStore::p29v1_system_source_reuse(SessionHandle session) const {
    const Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending || !space.route->p29v1 ||
        space.route->pending->begin.profile != ProfileId::P29V1 ||
        !space.route->p29v1->fixed_system_source_reuse)
        throw std::logic_error("P29V1 reuse decision is unavailable");
    return *space.route->p29v1->fixed_system_source_reuse;
}

void FStore::append_fill_v1(SessionHandle session,
                            std::vector<uint8_t> inner_fill) {
    Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending ||
        space.route->pending->begin.profile != ProfileId::P29V1)
        throw std::logic_error("P29V1 FILL has no active transaction");
    Namespace::FPending& pending = *space.route->pending;
    if (!pending.p29v1_fill.empty())
        throw std::logic_error("P29V1 FILL was received twice");
    if (inner_fill.empty())
        throw std::invalid_argument("P29V1 FILL inner stream is empty");
    pending.p29v1_fill = std::move(inner_fill);
}

std::vector<uint8_t> FStore::materialize_and_verify(SessionHandle session) {
    Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending)
        throw std::logic_error("F route has no ACTIVE_TX to materialize");
    Namespace::FPending& pending = *space.route->pending;
    if (pending.begin.profile != ProfileId::P29V1 || !space.route->p29v1 ||
        space.route->p29v1->terminal || !pending.body_complete ||
        pending.p29v1_fill.empty())
        throw std::logic_error(
            "P29V1 input cannot materialize before BODY, NEED, and FILL");
    try {
        (void)space.route->p29v1->deserializer.receive_fill(
            pending.p29v1_fill, false,
            static_cast<size_t>(pending.begin.raw_bytes));
#if defined(ICECC_P29V1_MUTANT_DOUBLE_MATERIALIZE)
        const auto scratch =
            space.route->p29v1->deserializer.rematerialize_for_mutant();
        if (scratch.bytes.size() != pending.begin.raw_bytes ||
            digest128(scratch.bytes) != pending.begin.raw_digest ||
            scratch.occurrences.size() !=
                space.route->p29v1->deserializer.mutant_occurrence_count() ||
            compute_transaction_digest(pending.begin, pending.body) !=
                pending.begin.transaction_digest)
            throw std::logic_error(
                "P29V1 mutant rematerialization does not match TX_BEGIN");
#endif
        std::vector<uint8_t> result =
            space.route->p29v1->deserializer.take_materialized();
        if (result.size() != pending.begin.raw_bytes ||
            digest128(result) != pending.begin.raw_digest ||
            compute_transaction_digest(pending.begin, pending.body) !=
                pending.begin.transaction_digest)
            throw std::logic_error(
                "P29V1 materialized input does not match TX_BEGIN exactly");
        const std::span<const uint8_t> segment =
            space.route->p29v1->deserializer.pending_segment();
        if (segment.size() > result.size())
            throw std::logic_error(
                "P29V1 staged segment exceeds materialized input");
        pending.p29v1_segment_digest =
            space.route->p29v1->deserializer.pending_segment_digest();
        pending.p29v1_materialized = true;
        record(ActionType::INPUT_MATERIALIZED, session, &pending.begin);
        return result;
    } catch (...) {
        space.route->p29v1->terminal = true;
        if (space.route->p29v1->deserializer.has_pending()) {
            try {
                space.route->p29v1->deserializer.abandon();
            } catch (...) {
            }
        }
        throw;
    }
}

TxCommit FStore::commit_input(SessionHandle session) {
    Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending ||
        space.route->pending->begin.profile != ProfileId::P29V1 ||
        !space.route->pending->p29v1_materialized || !space.route->p29v1 ||
        space.route->p29v1->terminal)
        throw std::logic_error("exact P29V1 input has not materialized");
    Namespace::Route& route = *space.route;
    const TxBegin begin = route.pending->begin;
    if (route.next_rel_seq.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("F REL_SEQ space exhausted");
    TxCommit commit{begin.history_nonce, begin.rel_seq, begin.tu_seq,
                    begin.transaction_digest, begin.raw_digest,
                    compute_post_state_digest(begin.pre_state_digest,
                                              begin.history_nonce, begin.rel_seq,
                                              begin.tu_seq,
                                              begin.transaction_digest)};
    try {
        route.p29v1->deserializer.commit();
    } catch (...) {
        route.p29v1->terminal = true;
        throw;
    }
    route.state_digest = commit.post_state_digest;
    ++route.next_rel_seq.value;
    route.last_commit = commit;
    record(ActionType::INPUT_COMMITTED, session, &begin);
    route.pending.reset();
    return commit;
}

void FStore::abandon_input(SessionHandle session) noexcept {
    try {
        Namespace& space = require_namespace(session);
        abandon_pending(space);
    } catch (...) {
    }
}

uint64_t FStore::pending_segment_bytes(SessionHandle session) const noexcept {
    try {
        const Namespace& space = require_namespace(session);
        if (!space.route || !space.route->pending ||
            space.route->pending->begin.profile != ProfileId::P29V1 ||
            !space.route->pending->p29v1_materialized || !space.route->p29v1)
            return 0;
        return space.route->p29v1->deserializer.pending_segment().size();
    } catch (...) {
        return 0;
    }
}

Digest128 FStore::pending_segment_digest(SessionHandle session) const noexcept {
    try {
        const Namespace& space = require_namespace(session);
        if (!space.route || !space.route->pending ||
            space.route->pending->begin.profile != ProfileId::P29V1 ||
            !space.route->pending->p29v1_materialized)
            return {};
        return space.route->pending->p29v1_segment_digest;
    } catch (...) {
        return {};
    }
}

void FStore::forget_route(SessionHandle session) {
    Namespace& space = require_namespace(session);
    if (space.route && space.route->pending)
        throw std::logic_error("HISTORY_RESET is only legal outside ACTIVE_TX");
    space.route.reset();
    space.established = true;
}

void FStore::destructive_cache_reset(FStoreGuid new_guid) {
    if (new_guid == guid_)
        throw std::invalid_argument("F store reset requires a new F_STORE_GUID");
    namespaces_.clear();
    guid_ = new_guid;
    next_session_serial_ = 1;
    session_serial_exhausted_ = false;
}

FStore::Namespace& FStore::require_namespace(SessionHandle session) {
    const auto position = namespaces_.find(session.c_store_guid);
    if (session.f_store_guid != guid_ || position == namespaces_.end() ||
        session.serial == 0 ||
        position->second->active_session_serial != session.serial)
        throw std::logic_error("stale or unknown F session");
    return *position->second;
}

const FStore::Namespace& FStore::require_namespace(SessionHandle session) const {
    const auto position = namespaces_.find(session.c_store_guid);
    if (session.f_store_guid != guid_ || position == namespaces_.end() ||
        session.serial == 0 ||
        position->second->active_session_serial != session.serial)
        throw std::logic_error("stale or unknown F session");
    return *position->second;
}

}  // namespace icecc::p50
