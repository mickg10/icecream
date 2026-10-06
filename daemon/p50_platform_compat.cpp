#include "p50_platform_compat.h"

#include "digest128.h"
#include "p50_daemon_sidecar_adapter.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <span>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/syscall.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <time.h>
#endif

namespace icecc::p50::platform {

namespace {

#if defined(ICECC_P50_PLATFORM_COMPAT_TEST_HOOKS)
TestHooks hooks;
#endif

const char* backend_name(Backend backend) noexcept {
    switch (backend) {
    case Backend::Native: return "native";
    case Backend::Compat: return "compat";
    case Backend::FailClosed: return "fail-closed";
    }
    return "unknown";
}

} // namespace

Mode parse_mode(const char* value) noexcept {
    if (value == nullptr || value[0] == '\0')
        return Mode::Auto;
    if (std::strcmp(value, "auto") == 0)
        return Mode::Auto;
    if (std::strcmp(value, "native") == 0)
        return Mode::Native;
    if (std::strcmp(value, "compat") == 0)
        return Mode::Compat;
    return Mode::Invalid;
}

bool probe_pidfd_child(bool* out_unsupported) noexcept {
    if (out_unsupported != nullptr)
        *out_unsupported = false;
#if defined(__linux__) && defined(SYS_pidfd_open) && defined(WEXITED)
#if defined(ICECC_P50_PLATFORM_COMPAT_TEST_HOOKS)
    if (hooks.force_pidfd_open_errno != 0) {
        errno = hooks.force_pidfd_open_errno;
        if (out_unsupported != nullptr)
            *out_unsupported = (errno == ENOSYS);
        return false;
    }
    if (hooks.force_waitid_pidfd_errno != 0) {
        errno = hooks.force_waitid_pidfd_errno;
        if (out_unsupported != nullptr)
            *out_unsupported = (errno == ENOSYS);
        return false;
    }
#endif
    const pid_t child = ::fork();
    if (child < 0)
        return false;
    if (child == 0)
        ::_exit(0);
    errno = 0;
    const long raw_pidfd = ::syscall(SYS_pidfd_open, child, 0u);
    if (raw_pidfd < 0 || raw_pidfd > std::numeric_limits<int>::max()) {
        const int open_errno = errno;
        int reap_status = 0;
        while (::waitpid(child, &reap_status, 0) < 0 && errno == EINTR) {}
        if (out_unsupported != nullptr)
            *out_unsupported = (open_errno == ENOSYS);
        return false;
    }
    const int pidfd = static_cast<int>(raw_pidfd);
    siginfo_t info{};
    errno = 0;
    const int wait_result =
        ::waitid(static_cast<idtype_t>(3), static_cast<id_t>(pidfd), &info, WEXITED);
    const int wait_errno = errno;
    (void)::close(pidfd);
    if (wait_result != 0) {
        int reap_status = 0;
        while (::waitpid(child, &reap_status, 0) < 0 && errno == EINTR) {}
        if (out_unsupported != nullptr)
            *out_unsupported = (wait_errno == ENOSYS);
        return false;
    }
    return info.si_pid == child;
#else
    (void)out_unsupported;
    errno = ENOSYS;
    if (out_unsupported != nullptr)
        *out_unsupported = true;
    return false;
#endif
}

bool probe_sealed_memfd(bool* out_unsupported) noexcept {
    if (out_unsupported != nullptr)
        *out_unsupported = false;
#if defined(__linux__) && defined(SYS_memfd_create) && defined(MFD_ALLOW_SEALING) && \
    defined(F_ADD_SEALS) && defined(F_GET_SEALS) && defined(F_SEAL_SEAL) &&          \
    defined(F_SEAL_SHRINK) && defined(F_SEAL_GROW) && defined(F_SEAL_WRITE)
#if defined(ICECC_P50_PLATFORM_COMPAT_TEST_HOOKS)
    if (hooks.force_memfd_create_errno != 0) {
        errno = hooks.force_memfd_create_errno;
        if (out_unsupported != nullptr)
            *out_unsupported = (errno == ENOSYS);
        return false;
    }
#endif
    errno = 0;
    const long raw_fd = ::syscall(SYS_memfd_create, "p50-platform-probe",
                                  static_cast<unsigned>(MFD_ALLOW_SEALING));
    if (raw_fd < 0 || raw_fd > std::numeric_limits<int>::max()) {
        const int create_errno = errno;
        if (out_unsupported != nullptr)
            *out_unsupported = (create_errno == ENOSYS);
        return false;
    }
    const int fd = static_cast<int>(raw_fd);
    const int immutable_seals =
        F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    bool ok = true;
#if defined(ICECC_P50_PLATFORM_COMPAT_TEST_HOOKS)
    if (hooks.force_seal_roundtrip_failure)
        ok = false;
#endif
    if (ok && ::fcntl(fd, F_ADD_SEALS, immutable_seals) != 0)
        ok = false;
    if (ok) {
        const int seals = ::fcntl(fd, F_GET_SEALS);
        ok = seals >= 0 && (seals & immutable_seals) == immutable_seals;
    }
    (void)::close(fd);
    return ok;
#else
    (void)out_unsupported;
    errno = ENOSYS;
    if (out_unsupported != nullptr)
        *out_unsupported = true;
    return false;
#endif
}

bool probe_getrandom(bool* out_unsupported) noexcept {
    if (out_unsupported != nullptr)
        *out_unsupported = false;
#if defined(__linux__) && defined(SYS_getrandom)
#if defined(ICECC_P50_PLATFORM_COMPAT_TEST_HOOKS)
    if (hooks.force_getrandom_errno != 0) {
        errno = hooks.force_getrandom_errno;
        if (out_unsupported != nullptr)
            *out_unsupported = (errno == ENOSYS);
        return false;
    }
#endif
    unsigned char buffer[32];
    errno = 0;
    const long result = ::syscall(SYS_getrandom, buffer, sizeof(buffer), 0u);
    if (result == static_cast<long>(sizeof(buffer)))
        return true;
    if (out_unsupported != nullptr)
        *out_unsupported = (errno == ENOSYS);
    return false;
#else
    (void)out_unsupported;
    errno = ENOSYS;
    if (out_unsupported != nullptr)
        *out_unsupported = true;
    return false;
#endif
}

namespace {

MitigationDecision resolve_one(Mode effective_mode, const char* raw_value,
                               const char* native_ok_label,
                               bool (*probe)(bool*) noexcept) noexcept {
    MitigationDecision decision;
    if (effective_mode == Mode::Invalid) {
        decision.backend = Backend::FailClosed;
        decision.reason =
            std::string("invalid value '") + (raw_value != nullptr ? raw_value : "") + "'";
        return decision;
    }
    if (effective_mode == Mode::Compat) {
        decision.backend = Backend::Compat;
        decision.reason = "forced compat";
        return decision;
    }
    bool unsupported = false;
    if (probe(&unsupported)) {
        decision.backend = Backend::Native;
        decision.reason = native_ok_label;
        return decision;
    }
    if (effective_mode == Mode::Native) {
        decision.backend = Backend::FailClosed;
        decision.reason =
            unsupported ? "ENOSYS (native forced, no compat)" : "probe error (native forced, no compat)";
        return decision;
    }
    // Mode::Auto
    if (unsupported) {
        decision.backend = Backend::Compat;
        decision.reason = "ENOSYS";
        return decision;
    }
    decision.backend = Backend::FailClosed;
    decision.reason = "probe error (not ENOSYS)";
    return decision;
}

} // namespace

PlatformDecision decide_platform() noexcept {
    const char* shorthand_raw = ::getenv("ICECC_P50_PLATFORM");
    const Mode shorthand = parse_mode(shorthand_raw);

    const char* child_raw = ::getenv("ICECC_P50_PLATFORM_CHILD");
    const char* input_raw = ::getenv("ICECC_P50_PLATFORM_INPUT");
    const char* random_raw = ::getenv("ICECC_P50_PLATFORM_RANDOM");

    const Mode child_mode = (child_raw != nullptr) ? parse_mode(child_raw) : shorthand;
    const Mode input_mode = (input_raw != nullptr) ? parse_mode(input_raw) : shorthand;
    const Mode random_mode = (random_raw != nullptr) ? parse_mode(random_raw) : shorthand;

    // When a per-mitigation override is absent, the effective mode falls
    // back to the shorthand -- so the raw string that actually produced
    // that mode (for an "invalid value '...'" message) is the shorthand's,
    // not the per-mitigation variable's (which is null in that case).
    const char* child_effective_raw = (child_raw != nullptr) ? child_raw : shorthand_raw;
    const char* input_effective_raw = (input_raw != nullptr) ? input_raw : shorthand_raw;
    const char* random_effective_raw = (random_raw != nullptr) ? random_raw : shorthand_raw;

    PlatformDecision decision;
    decision.child = resolve_one(child_mode, child_effective_raw, "pidfd ok", &probe_pidfd_child);
    decision.input = resolve_one(input_mode, input_effective_raw, "seal round-trip ok", &probe_sealed_memfd);
    decision.random = resolve_one(random_mode, random_effective_raw, "getrandom ok", &probe_getrandom);
    return decision;
}

bool any_fail_closed(const PlatformDecision& decision) noexcept {
    return decision.child.backend == Backend::FailClosed ||
           decision.input.backend == Backend::FailClosed ||
           decision.random.backend == Backend::FailClosed;
}

std::string format_platform_log_line(const PlatformDecision& decision) noexcept {
    std::string line = "p50 platform: child=";
    line += backend_name(decision.child.backend);
    line += "(";
    line += decision.child.reason;
    line += ") input=";
    line += backend_name(decision.input.backend);
    line += "(";
    line += decision.input.reason;
    line += ") random=";
    line += backend_name(decision.random.backend);
    line += "(";
    line += decision.random.reason;
    line += ")";
    return line;
}

// CompatChildHandle::arm, ::still_same_process, and compat_wait_bounded now
// live in cache/p50_compat_child_handle.cpp; see the comment at the top of
// cache/p50_compat_child_handle.h for why.

ssize_t compat_store_identity_entropy(void* buffer, size_t length, unsigned flags) noexcept {
    (void)flags;
    if (buffer == nullptr)
        return -1;
    int fd = -1;
    for (;;) {
        fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
            break;
        if (errno == EINTR)
            continue;
        return -1;
    }
    size_t total = 0;
    unsigned char* out = static_cast<unsigned char*>(buffer);
    while (total < length) {
        const ssize_t result = ::read(fd, out + total, length - total);
        if (result > 0) {
            total += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        (void)::close(fd);
        return -1;
    }
    (void)::close(fd);
    return static_cast<ssize_t>(total);
}

#if defined(__linux__)
namespace {

constexpr size_t kCompatInputCopyChunkBytes = size_t{1} << 20;  // 1 MiB.

// Builds (idempotently; EEXIST is fine) a 0700 owner-only directory
// `/dev/shm/icecc-p50-compat-daemon-<euid>` -- deliberately a different
// literal path than the sidecar's own `/dev/shm/icecc-p50-compat-<euid>`
// directory (cache/p50_input_fd_attachment.cpp's
// open_compat_private_write_fd()) so the daemon's and the sidecar's compat
// private files never collide under one path even when both processes
// happen to share an euid. Returns a writable fd for a brand new file
// inside it, created via mkstemp() and immediately unlink()ed, matching the
// sidecar's own producer: not O_TMPFILE (Linux 3.11+), mkstemp()+unlink()
// is the one path that works on every target kernel.
int open_compat_daemon_private_write_fd() noexcept {
    char dir_path[72];
    const int dir_len = std::snprintf(dir_path, sizeof(dir_path),
                                      "/dev/shm/icecc-p50-compat-daemon-%d",
                                      static_cast<int>(::geteuid()));
    if (dir_len <= 0 || static_cast<size_t>(dir_len) >= sizeof(dir_path))
        return -1;
    if (::mkdir(dir_path, 0700) != 0 && errno != EEXIST)
        return -1;
    struct stat dir_info{};
    if (::lstat(dir_path, &dir_info) != 0 || !S_ISDIR(dir_info.st_mode) ||
        (dir_info.st_mode & 0777) != 0700 || dir_info.st_uid != ::geteuid())
        return -1;
    char path[112];
    const int path_len =
        std::snprintf(path, sizeof(path), "%s/input-XXXXXX", dir_path);
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path))
        return -1;
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return -1;
    if (::unlink(path) != 0) {
        (void)::close(fd);
        return -1;
    }
    return fd;
}

// Reopens `writable_fd` O_RDONLY via /proc/self/fd without ever naming the
// unlinked file on disk, verifying its exact size. Deliberately duplicated
// rather than shared with cache/p50_input_fd_attachment.cpp's
// reopen_readonly_memfd(): that helper has internal linkage in its own
// translation unit (anonymous namespace), and this module intentionally
// stays free of any dependency on the sidecar's TU (see this file's header
// comment on probes being self-contained and unit-testable).
int reopen_compat_copy_readonly(int writable_fd, uintmax_t exact_size) noexcept {
    char proc_path[64]{};
    const int length = std::snprintf(proc_path, sizeof(proc_path),
                                     "/proc/self/fd/%d", writable_fd);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(proc_path))
        return -1;
    const int readonly_fd = ::open(proc_path, O_RDONLY | O_CLOEXEC);
    if (readonly_fd < 0)
        return -1;
    struct stat info{};
    if (::fstat(readonly_fd, &info) != 0 || info.st_size < 0 ||
        static_cast<uintmax_t>(info.st_size) != exact_size ||
        ::lseek(readonly_fd, 0, SEEK_SET) != 0) {
        (void)::close(readonly_fd);
        return -1;
    }
    return readonly_fd;
}

bool read_fully(int fd, void* buffer, size_t length) noexcept {
    size_t total = 0;
    unsigned char* out = static_cast<unsigned char*>(buffer);
    while (total < length) {
        const ssize_t result = ::read(fd, out + total, length - total);
        if (result > 0) {
            total += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        return false;  // 0 (short EOF) or a real read error.
    }
    return true;
}

bool write_fully(int fd, const void* buffer, size_t length) noexcept {
    size_t total = 0;
    const unsigned char* in = static_cast<const unsigned char*>(buffer);
    while (total < length) {
        const ssize_t result = ::write(fd, in + total, length - total);
        if (result > 0) {
            total += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

}  // namespace

CompatInputCopy copy_compat_input_from_sidecar(int source_fd) noexcept {
    if (source_fd < 0)
        return CompatInputCopy{-1, false};
    struct stat source_info{};
    if (::fstat(source_fd, &source_info) != 0 || !S_ISREG(source_info.st_mode) ||
        source_info.st_size < 0)
        return CompatInputCopy{-1, false};
    const uintmax_t expected_size = static_cast<uintmax_t>(source_info.st_size);

    const int writable_fd = open_compat_daemon_private_write_fd();
    if (writable_fd < 0)
        return CompatInputCopy{-1, false};

    icecc::Digest128Builder write_digest;
    std::vector<uint8_t> chunk(kCompatInputCopyChunkBytes);
    uintmax_t total_copied = 0;
    bool copy_ok = true;
    while (total_copied < expected_size) {
        const size_t want = static_cast<size_t>(
            std::min<uintmax_t>(chunk.size(), expected_size - total_copied));
        if (!read_fully(source_fd, chunk.data(), want) ||
            !write_fully(writable_fd, chunk.data(), want)) {
            copy_ok = false;
            break;
        }
        write_digest.append(std::span<const uint8_t>(chunk.data(), want));
        total_copied += want;
    }
    if (!copy_ok || total_copied != expected_size) {
        (void)::close(writable_fd);
        return CompatInputCopy{-1, false};
    }
    // Catches a source that changed size between the fstat() above and the
    // last byte of the copy loop. source_fd should already be immutable by
    // the time it reaches here (its only writable fd was closed on the
    // sidecar side before handoff), but this check costs one more fstat()
    // and keeps that assumption from being load-bearing on its own.
    struct stat source_info_after{};
    if (::fstat(source_fd, &source_info_after) != 0 ||
        source_info_after.st_size < 0 ||
        static_cast<uintmax_t>(source_info_after.st_size) != expected_size) {
        (void)::close(writable_fd);
        return CompatInputCopy{-1, false};
    }
    const icecc::Digest128 expected_digest = write_digest.finish();

    if (::fchmod(writable_fd, 0400) != 0) {
        (void)::close(writable_fd);
        return CompatInputCopy{-1, false};
    }
    const int readonly_fd = reopen_compat_copy_readonly(writable_fd, expected_size);
    (void)::close(writable_fd);
    if (readonly_fd < 0)
        return CompatInputCopy{-1, false};

    struct stat final_info{};
    if (::fstat(readonly_fd, &final_info) != 0 || final_info.st_nlink != 0 ||
        (final_info.st_mode & 0777) != 0400 || final_info.st_size < 0 ||
        static_cast<uintmax_t>(final_info.st_size) != expected_size) {
        (void)::close(readonly_fd);
        return CompatInputCopy{-1, false};
    }

    // Read the new file back end-to-end and re-hash it: the "digest" half
    // of "verify length plus digest" -- proof that what the kernel now
    // serves back from the daemon's own copy is byte-for-byte what was
    // written, not merely that the write() calls above returned success.
    icecc::Digest128Builder read_back_digest;
    uintmax_t total_verified = 0;
    bool verify_ok = true;
    while (total_verified < expected_size) {
        const size_t want = static_cast<size_t>(
            std::min<uintmax_t>(chunk.size(), expected_size - total_verified));
        if (!read_fully(readonly_fd, chunk.data(), want)) {
            verify_ok = false;
            break;
        }
        read_back_digest.append(std::span<const uint8_t>(chunk.data(), want));
        total_verified += want;
    }
    if (!verify_ok || total_verified != expected_size ||
        ::lseek(readonly_fd, 0, SEEK_SET) != 0 ||
        read_back_digest.finish() != expected_digest) {
        (void)::close(readonly_fd);
        return CompatInputCopy{-1, false};
    }
    return CompatInputCopy{readonly_fd, true};
}
#else
CompatInputCopy copy_compat_input_from_sidecar(int source_fd) noexcept {
    (void)source_fd;
    return CompatInputCopy{-1, false};
}
#endif

#if defined(ICECC_P50_PLATFORM_COMPAT_TEST_HOOKS)
void set_test_hooks(TestHooks value) noexcept { hooks = value; }
void reset_test_hooks() noexcept { hooks = TestHooks{}; }
#endif

void apply_platform_decision_to_config(const PlatformDecision& decision,
                                       icecc::p50::daemon::Config& config) noexcept {
    config.child_handle_backend =
        decision.child.backend == Backend::Compat
            ? icecc::p50::ChildHandleBackend::Compat
            : icecc::p50::ChildHandleBackend::Native;
    config.input_backend =
        decision.input.backend == Backend::Compat
            ? icecc::p50::InputMaterializationBackend::Compat
            : icecc::p50::InputMaterializationBackend::Native;
    config.entropy_provider =
        decision.random.backend == Backend::Compat
            ? compat_store_identity_entropy
            : icecc::p50::system_store_identity_entropy;
}

} // namespace icecc::p50::platform
