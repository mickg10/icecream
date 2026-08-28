#include "p50_cache_service.h"
#include "p50_control_operation.h"
#include "p50_input_fd_attachment.h"
#include "services/comm.h"

#include <array>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <span>
#include <grp.h>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <string_view>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "services/digest128.h"
#include "p50_incarnation_identity.h"

#include <utility>
#include <future>
#include <thread>
#include <vector>

namespace icecc::p50::service {
namespace {

namespace asio = boost::asio;

constexpr std::string_view kReadyEnvironment = "ICECC_CACHE_SERVICE_READY_FD";
constexpr std::string_view kListenerEnvironment = "ICECC_CACHE_SERVICE_LISTENER_FD";
constexpr std::string_view kReadyFormatEnvironment =
    "ICECC_CACHE_SERVICE_READY_FORMAT";
constexpr std::string_view kExpectedGenerationEnvironment =
    "ICECC_CACHE_SERVICE_EXPECTED_GENERATION";
constexpr std::string_view kExpectedAttemptEnvironment =
    "ICECC_CACHE_SERVICE_EXPECTED_ATTEMPT";
constexpr std::string_view kExpectedFStoreGenerationEnvironment =
    "ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GENERATION";
constexpr std::string_view kExpectedDerivationVersionEnvironment =
    "ICECC_CACHE_SERVICE_EXPECTED_DERIVATION_VERSION";
constexpr std::string_view kExpectedFStoreGuidEnvironment =
    "ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID";
constexpr std::string_view kExpectedCStoreGuidEnvironment =
    "ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID";
constexpr std::string_view kExpectedSocketEnvironment =
    "ICECC_CACHE_SERVICE_EXPECTED_SOCKET";
constexpr std::string_view kExpectedSocketDigestEnvironment =
    "ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST";
constexpr std::string_view kReadyMessage = "READY\n";
constexpr int kPollMilliseconds = 100;
constexpr int kHandshakeMilliseconds = 500;
constexpr int kMaxBacklog = 16;
// A bounded control farm keeps an authenticated idle dispatcher or an active
// cache-wire handoff from consuming the only worker needed by compiler input.
// This is a hard concurrent cap, not a per-connection unbounded thread fork.
constexpr size_t kMaxControlWorkers = 4;

void append_terminal_lifecycle_test_trace(
    const InputLifecycleRequest& request,
    InputLifecycleApplyStatus status,
    P50ServerOwnerUsage before,
    P50ServerOwnerUsage after) noexcept {
    const char* required = ::getenv("ICECC_P50_C1F1_REQUIRED");
    const char* path = ::getenv("ICECC_P50_TEST_LIFECYCLE_TRACE");
    if (required == nullptr || std::strcmp(required, "1") != 0 ||
        path == nullptr || *path == '\0')
        return;

    char line[1024];
    const int length = std::snprintf(
        line, sizeof(line),
        "P50_LIFECYCLE pid=%lld generation=%llu attempt=%llu job=%llu "
        "epoch=%llu nonce=%llu request=%llu action=%u status=%u "
        "before_records=%zu before_bytes=%llu after_records=%zu "
        "after_bytes=%llu\n",
        static_cast<long long>(::getpid()),
        static_cast<unsigned long long>(request.identity.generation),
        static_cast<unsigned long long>(request.identity.attempt),
        static_cast<unsigned long long>(request.owner.logical_job),
        static_cast<unsigned long long>(request.owner.assignment_epoch),
        static_cast<unsigned long long>(request.owner.assignment_nonce),
        static_cast<unsigned long long>(request.operation_id),
        static_cast<unsigned int>(request.action),
        static_cast<unsigned int>(status),
        before.retained_input_records,
        static_cast<unsigned long long>(before.retained_input_bytes),
        after.retained_input_records,
        static_cast<unsigned long long>(after.retained_input_bytes));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line))
        return;

    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    size_t offset = 0;
    while (offset < static_cast<size_t>(length)) {
        const ssize_t written = ::write(
            fd, line + offset, static_cast<size_t>(length) - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        break;
    }
    (void)::close(fd);
}

volatile sig_atomic_t g_stop_requested = 0;
volatile sig_atomic_t g_signal_wake_fd = -1;

bool queued_control_data(int fd) noexcept {
#if defined(MSG_DONTWAIT)
    pollfd descriptor{fd, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, 0);
    if (ready <= 0 || (descriptor.revents & POLLIN) == 0)
        return false;
    uint8_t byte = 0;
    const ssize_t count =
        ::recv(fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    return count > 0;
#else
    (void)fd;
    return true;
#endif
}

void request_stop(int) noexcept {
    // The handler performs only async-signal-safe operations.  All C++ state
    // transitions, including SidecarRuntime::stop(), happen on the normal
    // service thread after it observes this byte through poll().
    g_stop_requested = 1;
    const int fd = g_signal_wake_fd;
    if (fd >= 0) {
        const uint8_t wake = 1;
        const ssize_t ignored = ::write(fd, &wake, sizeof(wake));
        (void)ignored;
    }
}

struct SignalGuard {
    struct sigaction old_term{};
    struct sigaction old_int{};
    bool installed = false;

    SignalGuard() = default;

    bool install(int wake_fd) noexcept {
        g_signal_wake_fd = wake_fd;
        struct sigaction action{};
        action.sa_handler = request_stop;
        if (::sigemptyset(&action.sa_mask) != 0) {
            g_signal_wake_fd = -1;
            return false;
        }
        // Deliberately omit SA_RESTART so poll/read wake for graceful stop.
        if (::sigaction(SIGTERM, &action, &old_term) != 0) {
            g_signal_wake_fd = -1;
            return false;
        }
        if (::sigaction(SIGINT, &action, &old_int) != 0) {
            (void)::sigaction(SIGTERM, &old_term, nullptr);
            g_signal_wake_fd = -1;
            return false;
        }
        installed = true;
        return true;
    }

    ~SignalGuard() {
        if (installed) {
            g_signal_wake_fd = -1;
            (void)::sigaction(SIGTERM, &old_term, nullptr);
            (void)::sigaction(SIGINT, &old_int, nullptr);
        }
    }

    SignalGuard(const SignalGuard&) = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;
};

struct OwnedFd {
    int fd = -1;
    ~OwnedFd() {
        if (fd >= 0)
            (void)::close(fd);
    }
    OwnedFd() = default;
    explicit OwnedFd(int value) : fd(value) {}
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
};

bool make_completion_pipe(int descriptors[2]) noexcept {
    descriptors[0] = -1;
    descriptors[1] = -1;
    if (::pipe(descriptors) != 0)
        return false;
    for (int index = 0; index != 2; ++index) {
        const int descriptor = descriptors[index];
        const int flags = ::fcntl(descriptor, F_GETFD);
        if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0) {
            (void)::close(descriptors[0]);
            (void)::close(descriptors[1]);
            descriptors[0] = descriptors[1] = -1;
            return false;
        }
    }
    return true;
}

struct CompletionWake {
    int fd = -1;
    ~CompletionWake() {
        if (fd < 0)
            return;
        const uint8_t wake = 1;
        for (;;) {
            const ssize_t written = ::write(fd, &wake, sizeof(wake));
            if (written == static_cast<ssize_t>(sizeof(wake)) ||
                (written < 0 && errno == EPIPE))
                break;
            if (written < 0 && errno == EINTR)
                continue;
            break;
        }
        (void)::close(fd);
    }
};

struct WakePipe {
    OwnedFd read;
    OwnedFd write;

    bool create() noexcept {
        int descriptors[2] = {-1, -1};
        if (::pipe(descriptors) != 0)
            return false;
        read.fd = descriptors[0];
        write.fd = descriptors[1];
        for (const int fd : descriptors) {
            const int flags = ::fcntl(fd, F_GETFD);
            if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
                return false;
            const int status_flags = ::fcntl(fd, F_GETFL);
            if (status_flags < 0 || ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0)
                return false;
        }
        return true;
    }
};

bool parse_uint(std::string_view text, uint64_t& value) noexcept {
    if (text.empty() || text.front() == '+' || text.front() == '-')
        return false;
    for (const char character : text) {
        if (character < '0' || character > '9')
            return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_int(std::string_view text, int& value) noexcept {
    uint64_t parsed = 0;
    if (!parse_uint(text, parsed) || parsed > static_cast<uint64_t>(std::numeric_limits<int>::max()))
        return false;
    value = static_cast<int>(parsed);
    return true;
}

template <typename Guid>
bool parse_guid_hex(std::string_view text, Guid& target) noexcept {
    if (text.size() != target.bytes.size() * 2)
        return false;
    for (size_t index = 0; index != target.bytes.size(); ++index) {
        auto nibble = [](char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        };
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        target.bytes[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return target != Guid{};
}

bool next_value(int argc, char* const argv[], int& index, std::string_view& value) noexcept {
    if (index + 1 >= argc || argv[index + 1] == nullptr)
        return false;
    value = argv[++index];
    return !value.empty();
}

bool parse_option_uint(std::string_view name, std::string_view value, uint64_t& target) noexcept {
    if (name == "--generation" || name == "--attempt" || name == "--f-store-generation" ||
        name == "--peer-uid" || name == "--peer-gid" || name == "--expected-uid" ||
        name == "--expected-gid" ||
        name == "--drop-uid" || name == "--drop-gid" ||
        name == "--store-derivation-version")
        return parse_uint(value, target);
    return false;
}

bool parse_ready_fd(OwnedFd& ready) noexcept {
    const char* raw = ::getenv(kReadyEnvironment.data());
    if (raw == nullptr || *raw == '\0')
        return false;
    int fd = -1;
    if (!parse_int(raw, fd) || fd < 3)
        return false;
    if (::fcntl(fd, F_GETFD) < 0)
        return false;
    struct stat info{};
    // READY is a one-way publication pipe, never an inherited listener or a
    // connected control socket.  This also makes accidental fd aliasing fail
    // closed before any structured launch state is accepted.
    if (::fstat(fd, &info) != 0 || !S_ISFIFO(info.st_mode))
        return false;
    ready.fd = fd;
    return true;
}

bool parse_listener_fd(OwnedFd& listener) noexcept {
    const char* raw = ::getenv(kListenerEnvironment.data());
    if (raw == nullptr || *raw == '\0')
        return false;
    int fd = -1;
    if (!parse_int(raw, fd) || fd < 3 || ::fcntl(fd, F_GETFD) < 0)
        return false;
    const char* ready_raw = ::getenv(kReadyEnvironment.data());
    int ready_fd = -1;
    if (ready_raw != nullptr && parse_int(ready_raw, ready_fd) && ready_fd == fd)
        return false;
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISSOCK(info.st_mode))
        return false;
    listener.fd = fd;
    return true;
}

std::string bytes_hex(std::span<const uint8_t> bytes);

struct StructuredLaunch {
    bool active = false;
    local::Identity identity{};
    uint64_t f_store_generation = 0;
    StoreIdentityRoot store_root{};
    uint64_t store_derivation_version = 0;
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    std::string socket_path;
    icecc::Digest128 socket_path_digest{};
};

bool read_structured_launch(StructuredLaunch& launch) noexcept {
    try {
        constexpr std::array<std::string_view, 10> names{
            kReadyFormatEnvironment,
            kExpectedGenerationEnvironment,
            kExpectedAttemptEnvironment,
            kExpectedFStoreGenerationEnvironment,
            kExpectedDerivationVersionEnvironment,
            kExpectedFStoreGuidEnvironment,
            kExpectedCStoreGuidEnvironment,
            kExpectedSocketEnvironment,
            kExpectedSocketDigestEnvironment,
            kListenerEnvironment,
        };
        std::array<const char*, names.size()> values{};
        size_t present = 0;
        for (size_t index = 0; index != names.size(); ++index) {
            values[index] = ::getenv(names[index].data());
            if (values[index] != nullptr)
                ++present;
        }
        if (present == 0) {
            launch = StructuredLaunch{};
            return true;
        }
        // One inherited or manually supplied fragment must never silently
        // select a partly structured launch. The supervisor scrubs all
        // managed names and publishes the complete immutable tuple together.
        if (present != names.size())
            return false;
        const std::string_view format(values[0]);
        const std::string_view generation_text(values[1]);
        const std::string_view attempt_text(values[2]);
        const std::string_view f_store_generation_text(values[3]);
        const std::string_view derivation_text(values[4]);
        const std::string_view expected_guid(values[5]);
        const std::string_view expected_c_guid(values[6]);
        const std::string_view expected_socket(values[7]);
        const std::string_view expected_digest(values[8]);
        const std::string_view listener_fd_text(values[9]);
        uint64_t generation = 0;
        uint64_t attempt = 0;
        uint64_t f_store_generation = 0;
        uint64_t derivation_version = 0;
        if (format != "2" || !parse_uint(generation_text, generation) ||
            !parse_uint(attempt_text, attempt) ||
            !parse_uint(f_store_generation_text, f_store_generation) ||
            !parse_uint(derivation_text, derivation_version) ||
            derivation_version != kStoreIdentityDerivationVersion ||
            generation == 0 || attempt == 0 || f_store_generation == 0 ||
            generation == std::numeric_limits<uint64_t>::max() ||
            attempt == std::numeric_limits<uint64_t>::max() ||
            expected_socket.empty() || expected_socket.front() != '/' ||
            expected_socket.size() > local::kMaxUnixPath ||
            expected_socket.find_first_of(" \t\r\n") != std::string_view::npos ||
            listener_fd_text.empty())
            return false;
        int listener_fd = -1;
        if (!parse_int(listener_fd_text, listener_fd) || listener_fd < 3)
            return false;
        const char* ready_fd_text = ::getenv(kReadyEnvironment.data());
        int ready_fd = -1;
        if (ready_fd_text == nullptr || !parse_int(ready_fd_text, ready_fd) ||
            ready_fd < 3 || ready_fd == listener_fd)
            return false;
        CStoreGuid c_guid{};
        FStoreGuid guid{};
        if (!parse_guid_hex(expected_guid, guid) ||
            !parse_guid_hex(expected_c_guid, c_guid) || c_guid == guid)
            return false;
        const StoreIdentityRoot root = store_identity_root_from_f_guid(guid);
        if (!root.valid() || c_guid != c_store_guid_for_root(root) ||
            guid != f_store_guid_for_root(root))
            return false;
        const local::Identity identity{generation, attempt};
        const icecc::Digest128 digest = icecc::digest128(expected_socket);
        if (expected_guid != bytes_hex(std::span<const uint8_t>(
                                 guid.bytes.data(), guid.bytes.size())) ||
            expected_c_guid != bytes_hex(std::span<const uint8_t>(
                                  c_guid.bytes.data(), c_guid.bytes.size())) ||
            expected_digest != icecc::digest128_hex(digest))
            return false;
        launch.active = true;
        launch.identity = identity;
        launch.f_store_generation = f_store_generation;
        launch.store_root = root;
        launch.store_derivation_version = derivation_version;
        launch.c_store_guid = c_guid;
        launch.f_store_guid = guid;
        launch.socket_path.assign(expected_socket);
        launch.socket_path_digest = digest;
        return true;
    } catch (...) {
        return false;
    }
}

bool write_exact(int fd, std::string_view bytes, int* failure_errno = nullptr) noexcept {
    size_t written = 0;
    while (written != bytes.size()) {
        const ssize_t result = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (result > 0) {
            written += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        if (failure_errno != nullptr)
            *failure_errno = result < 0 ? errno : EIO;
        return false;
    }
    return true;
}

// A pipe has no MSG_NOSIGNAL equivalent.  Block SIGPIPE only in this
// service thread while publishing readiness, then consume a signal generated
// by this write before restoring the caller's mask.  In particular, do not
// install SIG_IGN: the service is also used as a small library-shaped entry
// point in tests, and unrelated SIGPIPE behavior must remain untouched.
struct ReadySignalGuard {
    sigset_t signal_set{};
    sigset_t old_mask{};
    bool was_pending = false;
    bool active = false;

    ReadySignalGuard() = default;

    bool enter() noexcept {
        if (::sigemptyset(&signal_set) != 0 || ::sigaddset(&signal_set, SIGPIPE) != 0)
            return false;
        if (::pthread_sigmask(SIG_BLOCK, &signal_set, &old_mask) != 0)
            return false;
        active = true;

        sigset_t pending{};
        const int member = ::sigpending(&pending) == 0 ? ::sigismember(&pending, SIGPIPE) : -1;
        if (member < 0) {
            (void)finish(false);
            return false;
        }
        was_pending = member != 0;
        return true;
    }

    bool finish(bool consume_write_signal) noexcept {
        if (!active)
            return true;
        bool ok = true;
        if (consume_write_signal && !was_pending) {
            sigset_t pending{};
            const int member =
                ::sigpending(&pending) == 0 ? ::sigismember(&pending, SIGPIPE) : -1;
            if (member < 0) {
                ok = false;
            } else if (member != 0) {
                const struct timespec no_wait{0, 0};
                for (;;) {
                    const int result = ::sigtimedwait(&signal_set, nullptr, &no_wait);
                    if (result == SIGPIPE)
                        break;
                    if (result < 0 && errno == EINTR)
                        continue;
                    if (result < 0 && errno == EAGAIN)
                        break;
                    ok = false;
                    break;
                }
            }
        }
        if (::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr) != 0)
            ok = false;
        active = false;
        return ok;
    }

    ~ReadySignalGuard() { (void)finish(false); }
    ReadySignalGuard(const ReadySignalGuard&) = delete;
    ReadySignalGuard& operator=(const ReadySignalGuard&) = delete;
};

bool write_ready(int fd) noexcept {
    ReadySignalGuard signal_guard;
    if (!signal_guard.enter())
        return false;
    int failure_errno = 0;
    const bool written = write_exact(fd, kReadyMessage, &failure_errno);
    const bool signal_state_ok = signal_guard.finish(failure_errno == EPIPE);
    return written && signal_state_ok;
}

std::string bytes_hex(std::span<const uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const uint8_t byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

bool drop_and_prove(const Options& options) noexcept {
    if (!options.expected_peer.uid.has_value() || !options.expected_peer.gid.has_value())
        return false;
    if (*options.expected_peer.uid > static_cast<uint64_t>(std::numeric_limits<uid_t>::max()) ||
        *options.expected_peer.gid > static_cast<uint64_t>(std::numeric_limits<gid_t>::max()))
        return false;
    const bool privileged = ::getuid() == 0 || ::geteuid() == 0 || ::getgid() == 0 ||
                            ::getegid() == 0;
    const bool has_drop_uid = options.drop_uid.has_value();
    const bool has_drop_gid = options.drop_gid.has_value();
    if (has_drop_uid != has_drop_gid)
        return false;
    if (privileged && !has_drop_uid)
        return false;
    if (has_drop_uid) {
        // A non-root process cannot safely promise an explicit transition;
        // refusing here avoids a partial setgroups/setgid/setuid sequence.
        if (!privileged || *options.drop_uid == 0 || *options.drop_gid == 0 ||
            *options.drop_uid > static_cast<uint64_t>(std::numeric_limits<uid_t>::max()) ||
            *options.drop_gid > static_cast<uint64_t>(std::numeric_limits<gid_t>::max()))
            return false;
        const gid_t gid = static_cast<gid_t>(*options.drop_gid);
        const uid_t uid = static_cast<uid_t>(*options.drop_uid);
        if (::setgroups(0, nullptr) != 0 || ::setgid(gid) != 0 || ::setuid(uid) != 0)
            return false;
    }

    if (::getuid() != ::geteuid() || ::getgid() != ::getegid())
        return false;
    if (has_drop_uid) {
        const int supplementary_count = ::getgroups(0, nullptr);
        if (supplementary_count != 0)
            return false;
    }
#if defined(__linux__)
    uid_t real_uid = 0;
    uid_t effective_uid = 0;
    uid_t saved_uid = 0;
    gid_t real_gid = 0;
    gid_t effective_gid = 0;
    gid_t saved_gid = 0;
    if (::getresuid(&real_uid, &effective_uid, &saved_uid) != 0 ||
        ::getresgid(&real_gid, &effective_gid, &saved_gid) != 0 ||
        real_uid != ::getuid() || effective_uid != ::geteuid() || real_gid != ::getgid() ||
        effective_gid != ::getegid() ||
        (has_drop_uid && (saved_uid != ::geteuid() || saved_gid != ::getegid())))
        return false;
#endif
    return ::geteuid() != 0 && ::getegid() != 0;
}

struct ListenerIdentity {
    dev_t listener_device = 0;
    ino_t listener_inode = 0;
    dev_t pathname_device = 0;
    ino_t pathname_inode = 0;
};

bool write_ready_lease(int fd, const Options& options, const ListenerIdentity& listener) noexcept {
    const CStoreGuid c_guid = options.c_store_guid;
    const FStoreGuid guid = options.f_store_guid;
    const std::string digest = digest128_hex(digest128(options.socket_path));
    const std::string message =
        "READY v2 generation=" + std::to_string(options.identity.generation) +
        " attempt=" + std::to_string(options.identity.attempt) +
        " F_STORE_GENERATION=" + std::to_string(options.f_store_generation) +
        " DERIVATION_VERSION=" + std::to_string(options.store_derivation_version) +
        " pid=" + std::to_string(static_cast<long long>(::getpid())) +
        " C_STORE_GUID=" + bytes_hex(std::span<const uint8_t>(c_guid.bytes.data(),
                                                                  c_guid.bytes.size())) +
        " F_STORE_GUID=" + bytes_hex(std::span<const uint8_t>(guid.bytes.data(),
                                                                 guid.bytes.size())) +
        " PATH=" + options.socket_path + " DIGEST=" + digest +
        " DEV=" + std::to_string(static_cast<unsigned long long>(
            listener.pathname_device != 0 ? listener.pathname_device : listener.listener_device)) +
        " INO=" + std::to_string(static_cast<unsigned long long>(
            listener.pathname_inode != 0 ? listener.pathname_inode : listener.listener_inode)) +
        "\n";
    return write_exact(fd, message);
}

bool capture_listener_identity(int fd, const std::string& path,
                               ListenerIdentity& identity) noexcept {
    struct stat info{};
    struct stat pathname{};
    if (::fstat(fd, &info) != 0 || !S_ISSOCK(info.st_mode) ||
        ::lstat(path.c_str(), &pathname) != 0 || !S_ISSOCK(pathname.st_mode))
        return false;
    identity.listener_device = info.st_dev;
    identity.listener_inode = info.st_ino;
    identity.pathname_device = pathname.st_dev;
    identity.pathname_inode = pathname.st_ino;
    return true;
}

bool capture_prebound_listener_identity(int fd, const std::string& path,
                                        ListenerIdentity& identity) noexcept {
    struct stat info{};
    struct stat pathname{};
    if (::fstat(fd, &info) != 0 || !S_ISSOCK(info.st_mode) || info.st_dev == 0 ||
        info.st_ino == 0 || ::lstat(path.c_str(), &pathname) != 0 ||
        !S_ISSOCK(pathname.st_mode) || pathname.st_dev == 0 || pathname.st_ino == 0)
        return false;
    // Linux AF_UNIX socket fstat() identity is not the pathname dentry's
    // lstat() identity.  Prove the association through the kernel's bound
    // address before dropping privileges, then retain both identities for
    // strict READY and replacement-safe cleanup checks.
    sockaddr_un bound{};
    socklen_t bound_length = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0 ||
        bound.sun_family != AF_UNIX || path.size() >= sizeof(bound.sun_path))
        return false;
    const size_t bound_path_length = ::strnlen(bound.sun_path, sizeof(bound.sun_path));
    if (bound_path_length != path.size() ||
        std::memcmp(bound.sun_path, path.data(), path.size()) != 0)
        return false;
    identity.listener_device = info.st_dev;
    identity.listener_inode = info.st_ino;
    identity.pathname_device = pathname.st_dev;
    identity.pathname_inode = pathname.st_ino;
    return true;
}

bool prove_prebound_listener_after_drop(int fd, const std::string& path) noexcept {
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISSOCK(info.st_mode))
        return false;
    sockaddr_un bound{};
    socklen_t bound_length = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0 ||
        bound.sun_family != AF_UNIX || path.size() >= sizeof(bound.sun_path))
        return false;
    const size_t bound_path_length = ::strnlen(bound.sun_path, sizeof(bound.sun_path));
    if (bound_path_length != path.size() ||
        std::memcmp(bound.sun_path, path.data(), path.size()) != 0)
        return false;
    int accepting = 0;
    socklen_t accepting_length = sizeof(accepting);
    return ::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &accepting_length) == 0 &&
           accepting != 0;
}

void cleanup_listener(int fd, const std::string& path, const ListenerIdentity& identity,
                      bool prebound) noexcept {
    if (prebound)
        return;
    struct stat listener{};
    struct stat pathname{};
    // Compare the still-open listener with the pathname before unlinking.
    // If another process replaced the node, pathname cleanup must be skipped.
    if (::fstat(fd, &listener) != 0 || !S_ISSOCK(listener.st_mode) ||
        ::lstat(path.c_str(), &pathname) != 0 || !S_ISSOCK(pathname.st_mode) ||
        listener.st_dev != identity.listener_device || listener.st_ino != identity.listener_inode ||
        pathname.st_dev != identity.pathname_device || pathname.st_ino != identity.pathname_inode)
        return;
    (void)::unlink(path.c_str());
}

bool handle_connection(local::Connection connection, const Options& options,
                        SidecarRuntime& runtime) noexcept {
    try {
        if (!connection.valid())
            return false;
        if (connection.verify_peer_credentials(options.expected_peer) != local::Status::Ok)
            return true;
        local::Frame hello;
        if (connection.receive_with_timeout(hello, kHandshakeMilliseconds) != local::Status::Ok)
            return true;
        if (local::validate_handshake(hello, local::MessageType::Hello,
                                      local::PeerRole::Daemon, options.identity) != local::Status::Ok)
            return true;
        const local::Frame ack =
            local::make_hello_ack(local::PeerRole::Sidecar, options.identity);
        if (connection.send(ack) != local::Status::Ok)
            return true;

        /* HELLO authenticates a persistent daemon relationship.  Idleness is
           not an operation timeout: wait without consuming frame bytes in
           short stop-cancellable poll slices, then give the complete operation
           and handoff one fresh bounded budget. */
        for (;;) {
            if (g_stop_requested != 0)
                return true;
            pollfd descriptor{connection.native_handle(), POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, kPollMilliseconds);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0 || (ready > 0 && (descriptor.revents & POLLNVAL) != 0))
                return true;
            if (ready == 0)
                continue;
            if ((descriptor.revents & POLLIN) != 0)
                break;
            if ((descriptor.revents & (POLLERR | POLLHUP)) != 0)
                return true;
        }

        /* Protocol discrimination on the authenticated relationship: a
           dedicated F-session control connection's first post-handshake bytes
           carry the P5FS envelope magic. Peek without consuming, then hand
           the descriptor to the endpoint owner executor -- no per-connection
           thread, and exactly one parser ever reads the stream. */
        {
            uint8_t magic_peek[4]{};
            const ssize_t peeked =
                ::recv(connection.native_handle(), magic_peek,
                       sizeof(magic_peek), MSG_PEEK | MSG_DONTWAIT);
            if (peeked == static_cast<ssize_t>(sizeof(magic_peek)) &&
                magic_peek[0] == 0x50 && magic_peek[1] == 0x35 &&
                magic_peek[2] == 0x46 && magic_peek[3] == 0x53) { // "P5FS"
                const int routed = ::dup(connection.native_handle());
                if (routed >= 0)
                    runtime.route_fsession_connection(routed);
                return true; // worker exits; the owner executor drives the fd
            }
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds{kHandshakeMilliseconds};
        local::Frame operation_frame;
        if (connection.receive_until(operation_frame, deadline) != local::Status::Ok ||
            operation_frame.type != local::MessageType::Data ||
            local::validate_identity(operation_frame, options.identity) != local::Status::Ok)
            return true;
        local::ControlOperation operation;
        if (!local::decode_control_operation(operation_frame.payload, operation) ||
            operation.identity != options.identity || operation.request_id == 0)
            return true;

        if (operation.kind == local::ControlOperationKind::CacheSession) {
            /*
             * Cache-session continuation is deliberately not admitted by the
             * legacy worker.  The only positive path is the shared-codec
             * AdoptedOutcomeWriter: it must retain this authenticated control
             * lease, flush a canonical P5CO/PHASE_OPEN incrementally, and only
             * then move the same descriptor into CacheWire.  run_one() uses
             * the old receive-and-ACK / send-magic whole-operation path and is
             * retained solely for historical fixtures.  Calling it here
             * would make that path live and would close the control lease at
             * the wrong boundary, so fail closed until the writer bridge is
             * installed.
             */
            return true;
        }
        if (!operation.input.has_value() || !operation.owner.has_value())
            return true;

        if (operation.kind == local::ControlOperationKind::InputFdAttachment) {
            const InputFdRequest request{options.identity, *operation.input,
                                         *operation.owner,
                                         operation.request_id};
            // Input lookup and attempt reservation are serialized on the
            // endpoint owner.  Materialization remains on this bounded worker;
            // only an ACKed descriptor commits compiler authorization.
            InputFdAttachmentService attachment(
                [&runtime, deadline, request](InputRecordKey key) {
                    if (key != request.key)
                        return InputCursor{};
                    std::optional<InputCursor> cursor =
                        runtime.attach_input_on_owner(request, deadline);
                    return cursor.has_value() ? std::move(*cursor) : InputCursor{};
                });
            const InputFdAttachmentResult result = attachment.serve_request(
                connection, options.identity, options.expected_peer, request,
                deadline);
            runtime.finish_input_attachment_on_owner(
                request, result.status == InputFdAttachmentStatus::Accepted,
                deadline);
            return true;
        }

        if (operation.kind != local::ControlOperationKind::InputLifecycle)
            return true;
        if (operation.lifecycle_result.has_value() ||
            queued_control_data(connection.native_handle()))
            return true;
        InputLifecycleRequest request;
        request.identity = options.identity;
        request.key = *operation.input;
        request.owner = *operation.owner;
        request.operation_id = operation.request_id;
        request.action = operation.lifecycle_action;
        request.f_store_generation = operation.f_store_generation;
        request.f_store_guid = operation.f_store_guid;
        request.immutable_size = operation.immutable_size;
        request.immutable_digest = operation.immutable_digest;
        request.retirement_id = operation.retirement_id;
        request.replacement_owner = operation.replacement_owner;
        const bool retirement =
            operation.lifecycle_action == InputLifecycleAction::PrepareAttemptRetirement ||
            operation.lifecycle_action == InputLifecycleAction::CommitAttemptReplacement ||
            operation.lifecycle_action == InputLifecycleAction::CloseLogicalInputLease;
        if (retirement) {
            const sidecar::MonotonicClockIdentity clock_identity =
                sidecar::process_monotonic_clock_identity();
            const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!operation.absolute_deadline.valid() ||
                !operation.absolute_deadline.matches_clock(clock_identity) ||
                operation.absolute_deadline.expired(
                    now_ns, clock_identity.clock_domain_id,
                    clock_identity.time_namespace_id))
                return true;
            // Preserve the exact wire value.  This conversion is only a
            // local representation of the same CLOCK_MONOTONIC nanoseconds;
            // no receive-time-plus-duration deadline is reconstructed.
            request.absolute_deadline = operation.absolute_deadline;
            request.deadline = operation.absolute_deadline.as_steady_time_point();
        } else {
            request.deadline = deadline;
        }
        // Once the operation envelope has been decoded, every retirement
        // phase uses that same absolute deadline for apply, response, and
        // final ACK.  The pre-decode handshake budget above is only the
        // bounded envelope receive; it cannot renew the retirement budget.
        const auto operation_deadline = retirement
            ? operation.absolute_deadline.as_steady_time_point()
            : deadline;
        const std::optional<InputLifecycleApplyStatus> result =
            runtime.apply_input_lifecycle_on_owner(request, operation_deadline);
        if (!result.has_value())
            return true;
        const std::vector<uint8_t> response_payload =
            local::encode_control_operation(
                local::make_input_lifecycle_reply_operation(request, *result));
        if (response_payload.empty())
            return true;
        const local::Frame response{local::kProtocolVersion,
                                    local::MessageType::Data,
                                    options.identity, response_payload};
        if (connection.send_until(response, operation_deadline) != local::Status::Ok)
            return true;
        local::Frame acknowledgement;
        if (connection.receive_until(acknowledgement, operation_deadline) !=
                local::Status::Ok ||
            acknowledgement.type != local::MessageType::Goodbye ||
            !acknowledgement.payload.empty() ||
            local::validate_identity(acknowledgement, options.identity) !=
                local::Status::Ok)
            return true;
        return true;
    } catch (...) {
        return true;
    }
}

} // namespace

namespace {

RuntimeConfig validate_runtime_config(RuntimeConfig config) {
    if (config.f_store_guid == FStoreGuid{})
        throw std::invalid_argument("sidecar runtime requires a nonzero F_STORE_GUID");
    if (config.max_live_handoffs != 1)
        throw std::invalid_argument("sidecar runtime supports exactly one live handoff");
    if (config.endpoint_config.owner_limits.max_retained_input_records == 0 ||
        config.max_input_lifecycle_replays == 0 ||
        config.cancellation_grace <= std::chrono::milliseconds::zero())
        throw std::invalid_argument(
            "sidecar runtime bounds must be nonzero");
    if (config.c_store_guid == CStoreGuid{})
        throw std::invalid_argument("sidecar runtime requires a nonzero C_STORE_GUID");
    if (config.c_store_guid == config.f_store_guid)
        throw std::invalid_argument("sidecar runtime requires distinct C/F store GUIDs");
    return config;
}

} // namespace

SidecarRuntime::SidecarRuntime(RuntimeConfig config)
    : config_(validate_runtime_config(std::move(config))),
      input_lifecycle_(
          config_.endpoint_config.owner_limits.max_retained_input_records,
          config_.max_input_lifecycle_replays),
      endpoint_work_guard_(asio::make_work_guard(context_)),
      fsession_owner_(4, config_.f_store_generation) {
    const auto fsession_ready = fsession::mint_fsession_admission_ready(
        fsession_owner_.service_generation(), 1);
    if (!fsession_owner_.open_admission(fsession_ready))
        throw std::logic_error("failed to open production F-session admission");
    input_lifecycle_.bind_store_identity(config_.f_store_generation,
                                         config_.f_store_guid);
    InputJobStateSelector configured_selector =
        std::move(config_.endpoint_config.input_job_state);
    config_.endpoint_config.input_job_state =
        [this, configured_selector = std::move(configured_selector)](
            CStoreGuid guid, const TxBegin& begin, const TxCommit& commit,
            std::span<const uint8_t> exact) {
            const InputRecordKey key{guid, begin.tu_seq};
            const InputJobState configured_state =
                configured_selector
                    ? configured_selector(guid, begin, commit, exact)
                    : InputJobState::Open;
            const InputLifecycleCommitDecision decision =
                input_lifecycle_.prepare_route_commit(key);
            if (decision == InputLifecycleCommitDecision::CapacityExceeded)
                throw std::length_error(
                    "bounded input lifecycle table exhausted before commit");
            if (decision == InputLifecycleCommitDecision::Closed ||
                configured_state == InputJobState::Closed)
                return InputJobState::Closed;
            return InputJobState::Open;
        };
    if (config_.sidecar_launch) {
        config_.endpoint_config.sidecar_launch = config_.sidecar_launch;
        config_.endpoint_config.on_run_admitted = [this](EndpointCancelPermit permit) {
            std::lock_guard lock(endpoint_cancel_mutex_);
            endpoint_cancel_permit_ = std::move(permit);
        };
    }
    endpoint_ = std::make_unique<P50ServerEndpoint>(
        config_.f_store_guid, config_.endpoint_caps, nullptr, nullptr,
        config_.endpoint_config);
    endpoint_owner_thread_ = std::thread([this] { endpoint_owner_loop(); });
}

SidecarRuntime::~SidecarRuntime() {
    stop();
    endpoint_work_guard_.reset();
    if (endpoint_owner_thread_.joinable())
        endpoint_owner_thread_.join();
}

void SidecarRuntime::endpoint_owner_loop() noexcept {
    try {
        if (config_.owner_failure)
            config_.owner_failure();
        context_.run();
    } catch (...) {
        // Every endpoint coroutine converts its own failure into a result. If
        // an unexpected executor failure still escapes, let teardown drain
        // rather than allowing an exception to cross the worker boundary.
        endpoint_owner_failed_.store(true, std::memory_order_release);
        context_.stop();
    }
}

boost::asio::awaitable<void> SidecarRuntime::run_endpoint_on_owner(
    int adopted_fd, EndpointIoControl endpoint_control,
    std::promise<EndpointOwnerResult> completion, int completion_wake_fd) {
    CompletionWake completion_wake{completion_wake_fd};
    int owned_fd = adopted_fd;
    try {
        EndpointOwnerResult owner_result;
        if (stop_requested_.load(std::memory_order_acquire)) {
            (void)::close(owned_fd);
            owned_fd = -1;
            owner_result.status = RuntimeStatus::Stopped;
            completion.set_value(std::move(owner_result));
            co_return;
        }

        const auto executor = co_await boost::asio::this_coro::executor;
        boost::system::error_code adoption_error;
        const int fd_for_adoption = owned_fd;
        owned_fd = -1;
        std::optional<asio::ip::tcp::socket> socket =
            P50ServerEndpoint::adopt_connected_fd(executor, fd_for_adoption, adoption_error);
        if (!socket) {
            owner_result.status = RuntimeStatus::AdoptionFailed;
            completion.set_value(std::move(owner_result));
            co_return;
        }

        if (stop_requested_.load(std::memory_order_acquire)) {
            cancel_endpoint_run();
            owner_result.status = RuntimeStatus::Stopped;
            completion.set_value(std::move(owner_result));
            co_return;
        }

        live_sessions_.store(1, std::memory_order_release);
        if (config_.owner_failure_after_live) {
            // Post outside this guarded coroutine: the injected exception must
            // escape the owner executor while run_adopted is suspended on the
            // live session, not be converted into an ordinary endpoint result.
            context_.post([this] {
                if (config_.owner_failure_after_live)
                    config_.owner_failure_after_live();
            });
        }
        const ServerRunResult endpoint_result =
            co_await endpoint_->run_adopted(std::move(*socket), std::move(endpoint_control));
        if (endpoint_result.candidate_input.has_value() &&
            !endpoint_result.completed_input.has_value())
            input_lifecycle_.abort_route_commit(*endpoint_result.candidate_input);
        if (endpoint_result.completed_input.has_value()) {
            if (endpoint_result.committed_input.has_value() &&
                endpoint_result.committed_input != endpoint_result.completed_input)
                throw std::logic_error(
                    "retained InputRecord witness differs from completed route input");
            const bool retained = endpoint_result.committed_input.has_value();
            if (!input_lifecycle_.observe_route_commit(
                    *endpoint_result.completed_input, retained))
                throw std::logic_error(
                    "completed input route could not settle lifecycle ownership");
        }
        release_endpoint_run();
        live_sessions_.store(endpoint_->live_session_count(), std::memory_order_release);
        owner_result.endpoint = endpoint_result;
        owner_result.status = stop_requested_.load(std::memory_order_acquire)
                                  ? RuntimeStatus::Stopped
                                  : endpoint_result.status == ServerRunStatus::TerminalError
                                  ? RuntimeStatus::EndpointFailed
                                  : RuntimeStatus::Completed;
        completion.set_value(std::move(owner_result));
    } catch (...) {
        if (owned_fd >= 0)
            (void)::close(owned_fd);
        release_endpoint_run();
        try {
            live_sessions_.store(endpoint_->live_session_count(), std::memory_order_release);
        } catch (...) {
            live_sessions_.store(0, std::memory_order_release);
        }
        EndpointOwnerResult owner_result;
        owner_result.status = RuntimeStatus::EndpointFailed;
        try {
            completion.set_value(std::move(owner_result));
        } catch (...) {
        }
    }
    co_return;
}

RuntimeResult SidecarRuntime::run_one(
    local::Connection& control, const local::HandoffRequest& expected,
    std::chrono::steady_clock::time_point deadline, EndpointIoControl endpoint_control,
    local::ControlBindingPlaceholder binding_placeholder) {
    RuntimeResult result;
    if (stop_requested_.load(std::memory_order_acquire)) {
        result.status = RuntimeStatus::Stopped;
        return result;
    }
    if (busy_.test_and_set(std::memory_order_acq_rel)) {
        result.status = RuntimeStatus::Busy;
        result.handoff.status = local::FdHandoffStatus::AlreadyConsumed;
        return result;
    }
    struct BusyGuard {
        std::atomic_flag& flag;
        ~BusyGuard() { flag.clear(std::memory_order_release); }
    } busy_guard{busy_};

    const int control_cancel_fd = control.valid() ? ::dup(control.native_handle()) : -1;
    if (control_cancel_fd < 0) {
        result.status = RuntimeStatus::HandoffRejected;
        result.handoff.status = local::FdHandoffStatus::Disconnected;
        return result;
    }
    const int control_flags = ::fcntl(control_cancel_fd, F_GETFD);
    if (control_flags < 0 ||
        ::fcntl(control_cancel_fd, F_SETFD, control_flags | FD_CLOEXEC) < 0) {
        (void)::close(control_cancel_fd);
        result.status = RuntimeStatus::HandoffRejected;
        result.handoff.status = local::FdHandoffStatus::Disconnected;
        return result;
    }
    active_control_cancel_fd_.store(control_cancel_fd, std::memory_order_release);
    struct ActiveControlGuard {
        SidecarRuntime& runtime;
        ~ActiveControlGuard() { runtime.close_active_control(); }
    } active_control_guard{*this};
    // Close/shutdown from stop() can race this setup only by setting the
    // stop flag first.  Re-check after publication so a signal arriving in
    // this narrow window is still converted into a normal cancellation.
    if (stop_requested_.load(std::memory_order_acquire)) {
        cancel_active_control();
        result.status = RuntimeStatus::Stopped;
        result.handoff.status = local::FdHandoffStatus::Disconnected;
        return result;
    }

    local::FdHandoffReceiver receiver;
    result.handoff = receiver.receive_and_ack(control, expected, deadline);
    if (result.handoff.status != local::FdHandoffStatus::Accepted) {
        result.status = stop_requested_.load(std::memory_order_acquire)
                           ? RuntimeStatus::Stopped
                           : RuntimeStatus::HandoffRejected;
        return result;
    }

    local::HandoffFd adopted = receiver.take_adopted_fd();
    if (!adopted.valid()) {
        result.status = RuntimeStatus::AdoptionFailed;
        result.handoff.status = local::FdHandoffStatus::AdoptionFailed;
        return result;
    }
    if (stop_requested_.load(std::memory_order_acquire)) {
        result.status = RuntimeStatus::Stopped;
        result.handoff.status = local::FdHandoffStatus::Disconnected;
        return result;
    }

    /* The client must not emit one CacheWire byte until this exact descriptor
       is owned by the sidecar.  Publish that transition on the adopted socket
       itself; failure closes locally and never starts the endpoint. */
    if (!send_cache_session_ready(adopted.get(), deadline)) {
        result.status = RuntimeStatus::AdoptionFailed;
        return result;
    }

    const int adopted_fd = adopted.release();
    std::promise<EndpointOwnerResult> completion;
    std::future<EndpointOwnerResult> completion_result = completion.get_future();
    int completion_pipe[2] = {-1, -1};
    if (!make_completion_pipe(completion_pipe)) {
        (void)::close(adopted_fd);
        result.status = RuntimeStatus::EndpointFailed;
        return result;
    }
    int dispatch_fd = adopted_fd;
    try {
        asio::co_spawn(context_,
                       run_endpoint_on_owner(dispatch_fd, std::move(endpoint_control),
                                             std::move(completion), completion_pipe[1]),
                       asio::detached);
        dispatch_fd = -1;
        completion_pipe[1] = -1;
    } catch (...) {
        if (dispatch_fd >= 0)
            (void)::close(dispatch_fd);
        (void)::close(completion_pipe[0]);
        (void)::close(completion_pipe[1]);
        result.status = RuntimeStatus::EndpointFailed;
        return result;
    }

    RuntimeCancellationReason cancellation = RuntimeCancellationReason::None;
    std::chrono::steady_clock::time_point quiescence_deadline = deadline;
    const auto cancellation_trigger_now = std::chrono::steady_clock::now();
    const auto cancellation_trigger_deadline =
        deadline - cancellation_trigger_now > config_.cancellation_grace
            ? deadline - config_.cancellation_grace
            : cancellation_trigger_now;
    const auto request_cancellation = [&](RuntimeCancellationReason reason) {
        if (stop_requested_.load(std::memory_order_acquire) &&
            reason != RuntimeCancellationReason::Requested)
            reason = RuntimeCancellationReason::Stopped;
        if (cancellation == RuntimeCancellationReason::None) {
            cancellation = reason;
            const auto now = std::chrono::steady_clock::now();
            quiescence_deadline = std::min(
                deadline, now + config_.cancellation_grace);
            cancel_endpoint_run();
        }
    };
    const auto fail_stop = [&]() noexcept {
        if (config_.fail_stop)
            config_.fail_stop();
        // A policy that returns cannot make it safe to return from run_one:
        // the owner coroutine may still be executing against this runtime.
        // The installed sidecar therefore has a bounded supervised fail-stop
        // even when no test policy was injected.
        std::_Exit(125);
    };
    const auto poll_timeout = [&]() {
        const auto remaining = (cancellation == RuntimeCancellationReason::None
                                    ? cancellation_trigger_deadline
                                    : quiescence_deadline) -
                               std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero())
            return 0;
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        if (milliseconds < remaining)
            ++milliseconds;
        return static_cast<int>(std::min<int64_t>(milliseconds.count(),
                                                  std::numeric_limits<int>::max()));
    };
    for (;;) {
        if (completion_result.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready)
            break;
        if (endpoint_owner_failed_.load(std::memory_order_acquire)) {
            (void)::close(completion_pipe[0]);
            // The executor reported an escaped owner-context failure.  Do
            // not return while its coroutine/session registration might still
            // reference this runtime; the supervised sidecar fail-stop is the
            // only bounded safe outcome for this ownership violation.
            fail_stop();
        }
        if (cancellation != RuntimeCancellationReason::None &&
            std::chrono::steady_clock::now() >= quiescence_deadline)
            fail_stop();
        if (cancellation == RuntimeCancellationReason::None) {
            uint8_t probe = 0;
            const ssize_t peeked = ::recv(control.native_handle(), &probe, 1,
                                          MSG_PEEK | MSG_DONTWAIT);
            if (peeked == 0) {
                request_cancellation(RuntimeCancellationReason::ControlEof);
                continue;
            }
        }
        if (cancellation == RuntimeCancellationReason::None &&
            std::chrono::steady_clock::now() >= cancellation_trigger_deadline) {
            request_cancellation(RuntimeCancellationReason::Deadline);
            continue;
        }
        pollfd descriptors[2] = {
            {cancellation == RuntimeCancellationReason::None ? control.native_handle() : -1,
             static_cast<short>(cancellation == RuntimeCancellationReason::None
                                    ? (POLLIN | POLLERR | POLLHUP)
                                    : 0),
             0},
            {completion_pipe[0], POLLIN | POLLERR | POLLHUP, 0},
        };
        const int ready = ::poll(descriptors, 2, poll_timeout());
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            request_cancellation(RuntimeCancellationReason::ControlEof);
            continue;
        }
        if (cancellation == RuntimeCancellationReason::None &&
            (descriptors[0].revents & (POLLERR | POLLHUP)) != 0 &&
            (descriptors[0].revents & POLLIN) == 0) {
            request_cancellation(RuntimeCancellationReason::ControlEof);
        } else if (cancellation == RuntimeCancellationReason::None &&
                   (descriptors[0].revents & POLLIN) != 0) {
            local::Frame cancel_frame;
            const local::Status status = control.receive_until(cancel_frame, deadline);
            if (status == local::Status::CleanEof || status == local::Status::Truncated) {
                request_cancellation(RuntimeCancellationReason::ControlEof);
            } else if (status == local::Status::Timeout) {
                // Some stream implementations report a peer shutdown as
                // readable without surfacing HUP through the framed reader.
                // Re-check the borrowed control descriptor before classifying
                // that wakeup as the cumulative operation deadline.
                uint8_t probe = 0;
                const ssize_t peeked = ::recv(control.native_handle(), &probe, 1,
                                              MSG_PEEK | MSG_DONTWAIT);
                if (peeked == 0)
                    request_cancellation(RuntimeCancellationReason::ControlEof);
                else
                    request_cancellation(RuntimeCancellationReason::Deadline);
            } else if (status != local::Status::Ok) {
                // Once the authenticated handoff is complete, any transport
                // status other than the explicit EOF/truncation/timeout cases
                // is an operation-terminal protocol failure.  Returning to
                // the endpoint with an unclassified frame would leave the
                // operation ambiguous.
                uint8_t probe = 0;
                const ssize_t peeked = ::recv(control.native_handle(), &probe, 1,
                                              MSG_PEEK | MSG_DONTWAIT);
                request_cancellation(peeked == 0
                                         ? RuntimeCancellationReason::ControlEof
                                         : RuntimeCancellationReason::Malformed);
            } else if (cancel_frame.type != local::MessageType::Data) {
                request_cancellation(RuntimeCancellationReason::Malformed);
            } else {
                local::ControlOperation cancel_operation;
                if (!local::decode_control_operation(cancel_frame.payload, cancel_operation)) {
                    request_cancellation(RuntimeCancellationReason::Malformed);
                } else if (cancel_operation.kind == local::ControlOperationKind::OperationCancel &&
                           cancel_frame.identity == expected.identity &&
                           cancel_operation.identity == expected.identity &&
                           cancel_operation.cancel_target_role ==
                               local::ControlCancelTargetRole::FSession &&
                           cancel_operation.sender_role ==
                               local::ControlOperationRole::Daemon &&
                           cancel_operation.request_id == expected.request_id &&
                           cancel_operation.binding_placeholder == binding_placeholder) {
                    switch (cancel_operation.cancellation_reason) {
                    case local::ControlCancellationReason::Requested:
                        request_cancellation(RuntimeCancellationReason::Requested);
                        break;
                    case local::ControlCancellationReason::Deadline:
                        request_cancellation(RuntimeCancellationReason::Deadline);
                        break;
                    case local::ControlCancellationReason::ControlEof:
                        request_cancellation(RuntimeCancellationReason::ControlEof);
                        break;
                    }
                }
            }
        }
        if ((descriptors[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0 &&
            completion_result.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready)
            break;
    }
    (void)::close(completion_pipe[0]);
    const EndpointOwnerResult owner_result = completion_result.get();
    result.status = owner_result.status;
    result.endpoint = owner_result.endpoint;
    result.cancellation = cancellation;
    if (cancellation != RuntimeCancellationReason::None) {
        result.status = cancellation == RuntimeCancellationReason::Stopped
                            ? RuntimeStatus::Stopped
                            : RuntimeStatus::Cancelled;
    }
    if (result.status == RuntimeStatus::EndpointFailed)
        live_sessions_.store(0, std::memory_order_release);
    return result;
}

std::optional<InputCursor> SidecarRuntime::attach_input_on_owner(
    InputFdRequest request,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (stop_requested_.load(std::memory_order_acquire) ||
        std::chrono::steady_clock::now() >= deadline)
        return std::nullopt;
    auto completion = std::make_shared<std::promise<std::optional<InputCursor>>>();
    std::future<std::optional<InputCursor>> result = completion->get_future();
    try {
        asio::post(context_, [this, request, deadline, completion] {
            if (stop_requested_.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
                completion->set_value(std::nullopt);
                return;
            }
            try {
                if (!input_lifecycle_.begin_attachment(
                        request.key, request.owner, request.request_id)) {
                    completion->set_value(std::nullopt);
                    return;
                }
                try {
                    completion->set_value(endpoint_->attach_input(request.key));
                } catch (...) {
                    input_lifecycle_.finish_attachment(
                        request.key, request.owner, request.request_id, false);
                    throw;
                }
            } catch (...) {
                completion->set_value(std::nullopt);
            }
        });
    } catch (...) {
        return std::nullopt;
    }
    for (;;) {
        if (result.wait_for(std::chrono::milliseconds(2)) == std::future_status::ready)
            return result.get();
        if (stop_requested_.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
    }
}

void SidecarRuntime::finish_input_attachment_on_owner(
    InputFdRequest request, bool authorized,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (stop_requested_.load(std::memory_order_acquire))
        return;
    auto completion = std::make_shared<std::promise<void>>();
    std::future<void> result = completion->get_future();
    try {
        asio::post(context_, [this, request, authorized, completion] {
            input_lifecycle_.finish_attachment(
                request.key, request.owner, request.request_id, authorized);
            try {
                endpoint_->collect_input_garbage();
            } catch (...) {
            }
            completion->set_value();
        });
    } catch (...) {
        return;
    }
    while (result.wait_for(std::chrono::milliseconds(2)) !=
           std::future_status::ready) {
        if (stop_requested_.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= deadline)
            return;
    }
}

std::optional<InputLifecycleApplyStatus>
SidecarRuntime::apply_input_lifecycle_on_owner(
    InputLifecycleRequest request,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (stop_requested_.load(std::memory_order_acquire) ||
        std::chrono::steady_clock::now() >= deadline)
        return std::nullopt;
    auto completion =
        std::make_shared<std::promise<InputLifecycleApplyStatus>>();
    std::future<InputLifecycleApplyStatus> result = completion->get_future();
    try {
        asio::post(context_, [this, request, completion] {
            const P50ServerOwnerUsage before = endpoint_->owner_usage();
            InputLifecycleApplyResult decision =
                input_lifecycle_.begin_apply(request);
            if (decision.status != InputLifecycleApplyStatus::Applied) {
                append_terminal_lifecycle_test_trace(
                    request, decision.status, before, endpoint_->owner_usage());
                completion->set_value(decision.status);
                return;
            }
            bool mutated = true;
            try {
                const bool retirement =
                    request.action == InputLifecycleAction::PrepareAttemptRetirement ||
                    request.action == InputLifecycleAction::CommitAttemptReplacement ||
                    request.action == InputLifecycleAction::CloseLogicalInputLease;
                if (retirement) {
                    // The sidecar endpoint is the one FInputStoreOwner.  This
                    // read-only cursor check binds retirement to the exact
                    // retained bytes without creating a daemon-local core or
                    // reopening a closed record.
                    const InputCursor cursor = endpoint_->attach_input(request.key);
                    if (cursor.remaining() != request.immutable_size ||
                        cursor.raw_digest() != request.immutable_digest)
                        mutated = false;
                }
                if (mutated && decision.close_record)
                    endpoint_->close_input_job(request.key);
                if (mutated && decision.collect_record)
                    endpoint_->collect_input_garbage();
            } catch (...) {
                mutated = false;
            }
            const bool registry_mutated =
                input_lifecycle_.finish_apply(request, mutated);
            InputLifecycleApplyStatus status = InputLifecycleApplyStatus::UnknownRecord;
            if (mutated && registry_mutated) {
                switch (request.action) {
                case InputLifecycleAction::PrepareAttemptRetirement:
                    status = InputLifecycleApplyStatus::AttemptQuiescedRecordRetained;
                    break;
                case InputLifecycleAction::CommitAttemptReplacement:
                    status = InputLifecycleApplyStatus::ReplacementInstalledRecordRetained;
                    break;
                case InputLifecycleAction::CloseLogicalInputLease:
                    status = endpoint_->owner_usage().retained_input_records <
                                     before.retained_input_records
                                 ? InputLifecycleApplyStatus::JobClosedRecordReclaimed
                                 : InputLifecycleApplyStatus::JobClosedRecordRetained;
                    break;
                default:
                    status = InputLifecycleApplyStatus::Applied;
                    break;
                }
            }
            append_terminal_lifecycle_test_trace(
                request, status, before, endpoint_->owner_usage());
            completion->set_value(status);
        });
    } catch (...) {
        return std::nullopt;
    }
    while (result.wait_for(std::chrono::milliseconds(2)) !=
           std::future_status::ready) {
        if (stop_requested_.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
    }
    return result.get();
}

void SidecarRuntime::cancel_active_control() noexcept {
    const int fd = active_control_cancel_fd_.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
        (void)::shutdown(fd, SHUT_RDWR);
        (void)::close(fd);
    }
}

void SidecarRuntime::close_active_control() noexcept {
    const int fd = active_control_cancel_fd_.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0)
        (void)::close(fd);
}

// --- dedicated F-session control connections (owner-affine) ---------------

struct SidecarRuntime::FSessionPump {
    FSessionPump(boost::asio::io_context& context, int fd)
        : stream(context, fd) {}
    boost::asio::posix::stream_descriptor stream;
    uint64_t cid = 0;
    std::array<uint8_t, 4096> buffer{};
    bool closed = false;
};

namespace {
int64_t fsession_now_ns() noexcept {
    timespec ts{};
    (void)::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}
} // namespace

void SidecarRuntime::route_fsession_connection(int connection_fd) noexcept {
    if (connection_fd < 0)
        return;
    const int flags = ::fcntl(connection_fd, F_GETFL);
    if (flags < 0 || ::fcntl(connection_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        (void)::close(connection_fd);
        return;
    }
    try {
        asio::post(context_, [this, connection_fd] {
            const uint64_t cid = fsession_owner_.connection_opened();
            if (cid == 0) {
                (void)::close(connection_fd); // bounded table full: refuse
                return;
            }
            std::shared_ptr<FSessionPump> pump;
            try {
                pump = std::make_shared<FSessionPump>(context_, connection_fd);
            } catch (...) {
                fsession_owner_.connection_closed(cid);
                (void)::close(connection_fd);
                return;
            }
            pump->cid = cid;
            fsession_pumps_.push_back(pump);
            fsession_live_.store(fsession_owner_.live_operations(),
                                 std::memory_order_release);
            fsession_arm_read(std::move(pump));
        });
    } catch (...) {
        (void)::close(connection_fd);
    }
}

void SidecarRuntime::fsession_arm_read(std::shared_ptr<FSessionPump> pump) noexcept {
    if (pump->closed)
        return;
    auto& stream = pump->stream;
    stream.async_wait(
        boost::asio::posix::stream_descriptor::wait_read,
        [this, pump = std::move(pump)](const boost::system::error_code& ec) mutable {
            if (pump->closed)
                return;
            if (ec) {
                fsession_close(std::move(pump));
                return;
            }
            // Bounded per-turn ingest: at most a few reads, then rearm.
            for (int round = 0; round < 4; ++round) {
                const ssize_t got =
                    ::recv(pump->stream.native_handle(), pump->buffer.data(),
                           pump->buffer.size(), MSG_DONTWAIT);
                if (got > 0) {
                    const fsession::ServiceIngestStatus status = fsession_owner_.on_bytes(
                        pump->cid,
                        {pump->buffer.data(), static_cast<size_t>(got)},
                        fsession_now_ns());
                    if (status != fsession::ServiceIngestStatus::Progress) {
                        fsession_close(std::move(pump));
                        return;
                    }
                    fsession_drain(pump);
                    if (pump->closed)
                        return;
                    if (static_cast<size_t>(got) < pump->buffer.size())
                        break;
                    continue;
                }
                if (got == 0) { // clean EOF
                    fsession_close(std::move(pump));
                    return;
                }
                if (errno == EINTR)
                    continue;
                break; // EAGAIN: wait for the next readability event
            }
            fsession_arm_read(std::move(pump));
        });
}

void SidecarRuntime::fsession_drain(std::shared_ptr<FSessionPump> pump) noexcept {
    if (pump->closed)
        return;
    const int fd = pump->stream.native_handle();
    const fsession::ServiceWriteFn write_fn =
        [fd](std::span<const uint8_t> bytes) -> long {
        const ssize_t wrote =
            ::send(fd, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (wrote >= 0)
            return static_cast<long>(wrote);
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        return -1;
    };
    if (!fsession_owner_.drain_outbound(pump->cid, write_fn, 64 * 1024)) {
        fsession_close(std::move(pump));
        return;
    }
    if (fsession_owner_.has_pending_outbound(pump->cid)) {
        // Would-block residue: resume on the next writability event.
        pump->stream.async_wait(
            boost::asio::posix::stream_descriptor::wait_write,
            [this, pump](const boost::system::error_code& ec) mutable {
                if (pump->closed)
                    return;
                if (ec) {
                    fsession_close(std::move(pump));
                    return;
                }
                fsession_drain(std::move(pump));
            });
    }
}

void SidecarRuntime::fsession_close(std::shared_ptr<FSessionPump> pump) noexcept {
    if (pump->closed)
        return;
    pump->closed = true;
    boost::system::error_code ignored;
    pump->stream.close(ignored);
    fsession_owner_.connection_closed(pump->cid);
    (void)fsession_owner_.reclaim(pump->cid); // retained if reconciliation pends
    fsession_live_.store(fsession_owner_.live_operations(),
                         std::memory_order_release);
    for (auto iterator = fsession_pumps_.begin();
         iterator != fsession_pumps_.end(); ++iterator) {
        if (iterator->get() == pump.get()) {
            fsession_pumps_.erase(iterator);
            break;
        }
    }
}

size_t SidecarRuntime::live_fsession_operations() const noexcept {
    return fsession_live_.load(std::memory_order_acquire);
}

void SidecarRuntime::cancel_endpoint_run() noexcept {
    // The permit is captured at admission and posted to the endpoint owner;
    // no current-socket lookup or descriptor-derived authority is permitted.
    try {
        std::optional<EndpointCancelPermit> permit;
        {
            std::lock_guard lock(endpoint_cancel_mutex_);
            permit = endpoint_cancel_permit_;
        }
        if (permit)
            context_.post([this, permit = std::move(*permit)] {
                (void)endpoint_->request_cancel(permit);
            });
    } catch (...) {
    }
}

void SidecarRuntime::release_endpoint_run() noexcept {
}

void SidecarRuntime::cancel_endpoint_incarnation() noexcept {
    if (!config_.sidecar_launch)
        return;
    try {
        context_.post([this, incarnation = *config_.sidecar_launch] {
            (void)endpoint_->cancel_all_for_incarnation(incarnation);
        });
    } catch (...) {
    }
}

void SidecarRuntime::stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    cancel_active_control();
    cancel_endpoint_incarnation();
    // Whole-incarnation teardown of dedicated F-session connections: posted to
    // the owner executor (never a cross-thread socket mutation for an ordinary
    // per-operation cancel; this is the exact incarnation-failure authority).
    try {
        asio::post(context_, [this] {
            std::vector<std::shared_ptr<FSessionPump>> pumps = fsession_pumps_;
            for (auto& pump : pumps)
                fsession_close(pump);
        });
    } catch (...) {
    }
}

size_t SidecarRuntime::live_session_count() const {
    return live_sessions_.load(std::memory_order_acquire);
}

bool parse_options(int argc, char* const argv[], Options& options, bool& show_help) noexcept {
    show_help = false;
    try {
        options = Options{};
        bool have_socket = false;
        bool have_generation = false;
        bool have_attempt = false;
        bool have_f_store_generation = false;
        bool have_uid = false;
        bool have_gid = false;
        bool have_store_derivation_version = false;
        bool have_c_store_guid = false;
        bool have_f_store_guid = false;
        for (int index = 1; index < argc; ++index) {
            if (argv[index] == nullptr)
                return false;
            const std::string_view name(argv[index]);
            if (name == "--help") {
                if (argc != 2)
                    return false;
                show_help = true;
                return true;
            }
            std::string_view value;
            if (name == "--socket") {
                if (!next_value(argc, argv, index, value) || have_socket)
                    return false;
                options.socket_path = value;
                have_socket = true;
                continue;
            }
            if (name == "--generation" || name == "--attempt" || name == "--f-store-generation" ||
                name == "--peer-uid" || name == "--peer-gid" || name == "--expected-uid" ||
                name == "--expected-gid" ||
                name == "--drop-uid" || name == "--drop-gid" || name == "--backlog" ||
                name == "--store-derivation-version" || name == "--c-store-guid" ||
                name == "--f-store-guid") {
                if (!next_value(argc, argv, index, value))
                    return false;
                uint64_t parsed = 0;
                if (name == "--backlog") {
                    if (!parse_int(value, options.backlog) || options.backlog < 1 ||
                        options.backlog > kMaxBacklog)
                        return false;
                } else if (name == "--c-store-guid") {
                    if (have_c_store_guid || !parse_guid_hex(value, options.c_store_guid))
                        return false;
                    have_c_store_guid = true;
                } else if (name == "--f-store-guid") {
                    if (have_f_store_guid || !parse_guid_hex(value, options.f_store_guid))
                        return false;
                    have_f_store_guid = true;
                } else if (!parse_option_uint(name, value, parsed)) {
                    return false;
                } else if (name == "--generation") {
                    if (have_generation || parsed == 0)
                        return false;
                    options.identity.generation = parsed;
                    have_generation = true;
                } else if (name == "--attempt") {
                    if (have_attempt || parsed == 0)
                        return false;
                    options.identity.attempt = parsed;
                    have_attempt = true;
                } else if (name == "--f-store-generation") {
                    if (have_f_store_generation || parsed == 0)
                        return false;
                    options.f_store_generation = parsed;
                    have_f_store_generation = true;
                } else if (name == "--store-derivation-version") {
                    if (have_store_derivation_version || options.store_derivation_version != 0 ||
                        parsed != kStoreIdentityDerivationVersion)
                        return false;
                    options.store_derivation_version = parsed;
                    have_store_derivation_version = true;
                } else if (name == "--peer-uid" || name == "--expected-uid") {
                    if (have_uid)
                        return false;
                    options.expected_peer.uid = parsed;
                    have_uid = true;
                } else if (name == "--peer-gid" || name == "--expected-gid") {
                    if (have_gid)
                        return false;
                    options.expected_peer.gid = parsed;
                    have_gid = true;
                } else if (name == "--drop-uid") {
                    if (options.drop_uid.has_value())
                        return false;
                    options.drop_uid = parsed;
                } else if (name == "--drop-gid") {
                    if (options.drop_gid.has_value())
                        return false;
                    options.drop_gid = parsed;
                }
                continue;
            }
            return false;
        }
        return have_socket && have_generation && have_attempt && have_uid && have_gid &&
               options.socket_path.front() == '/' && options.socket_path.find('\0') == std::string::npos &&
               options.socket_path.size() <= local::kMaxUnixPath && options.expected_peer.specified();
    } catch (...) {
        return false;
    }
}

int run(const Options& options) noexcept {
    Options effective_options = options;
    // The supervisor owns the per-incarnation pathname.  A command-line
    // default is ignored whenever the structured lease contract is active;
    // this prevents accidental static-path reuse across restart/controller
    // recreation.
    StructuredLaunch structured_launch;
    if (!read_structured_launch(structured_launch))
        return 2;
    if (structured_launch.active) {
        // The supervisor publishes the same immutable tuple in argv and the
        // authenticated environment.  Both channels must agree exactly;
        // neither an old GUID nor a current GUID paired with an old launch
        // may be silently overwritten by the environment copy.
        if (options.identity != structured_launch.identity ||
            options.f_store_generation != structured_launch.f_store_generation ||
            options.store_derivation_version != structured_launch.store_derivation_version ||
            options.c_store_guid != structured_launch.c_store_guid ||
            options.f_store_guid != structured_launch.f_store_guid ||
            options.socket_path != structured_launch.socket_path)
        {
            return 2;
        }
        effective_options.identity = structured_launch.identity;
        effective_options.f_store_generation = structured_launch.f_store_generation;
        effective_options.store_derivation_version = structured_launch.store_derivation_version;
        effective_options.c_store_guid = structured_launch.c_store_guid;
        effective_options.f_store_guid = structured_launch.f_store_guid;
        effective_options.socket_path = structured_launch.socket_path;
    }
    OwnedFd ready;
    if (!parse_ready_fd(ready))
        return 2;
    WakePipe wake;
    if (!wake.create())
        return 2;
    SignalGuard signals;
    g_stop_requested = 0;
    if (!signals.install(wake.write.fd))
        return 2;

    local::Status listen_status = local::Status::Ok;
    OwnedFd listener_owner;
    const bool prebound = structured_launch.active;
    ListenerIdentity identity{};
    if (prebound) {
        if (!parse_listener_fd(listener_owner))
            return 2;
        const int listener = listener_owner.fd;
        // Capture pathname identity before dropping privileges: the lease
        // directory is intentionally 0700 and may not be searchable by the
        // service UID. The inherited listener remains owned by this process
        // and is used only after drop_and_prove() succeeds.
        if (!capture_prebound_listener_identity(listener, effective_options.socket_path,
                                                identity))
            return 2;
    }
    if (!drop_and_prove(effective_options))
        return 2;
    if (prebound && !prove_prebound_listener_after_drop(listener_owner.fd,
                                                        effective_options.socket_path))
        return 2;
    if (!prebound) {
        const int listener = local::listen_unix(effective_options.socket_path,
                                                effective_options.backlog, &listen_status);
        if (listener < 0)
            return 2;
        listener_owner.fd = listener;
    }
    const int listener = listener_owner.fd;
    if (!prebound && !capture_listener_identity(listener, effective_options.socket_path,
                                                identity)) {
        cleanup_listener(listener, effective_options.socket_path, identity, prebound);
        return 2;
    }
    RuntimeConfig runtime_config;
    // The store identity is explicit launch state allocated by the supervisor;
    // it is independent of the control launch identity and rotates on every
    // restart. Never derive it from generation/attempt or a local clock.
    StoreIdentityRoot legacy_root{};
    if (!structured_launch.active && !fresh_store_identity_root(legacy_root))
        return 2;
    if (structured_launch.active &&
        (effective_options.store_derivation_version != kStoreIdentityDerivationVersion ||
         effective_options.c_store_guid == CStoreGuid{} ||
         effective_options.f_store_guid == FStoreGuid{} ||
         effective_options.c_store_guid != c_store_guid_for_root(
             store_identity_root_from_f_guid(effective_options.f_store_guid)) ||
         effective_options.f_store_guid != f_store_guid_for_root(
             store_identity_root_from_f_guid(effective_options.f_store_guid))))
        return 2;
    runtime_config.f_store_guid = structured_launch.active
                                      ? effective_options.f_store_guid
                                      : f_store_guid_for_root(legacy_root);
    runtime_config.f_store_generation = structured_launch.active
                                            ? effective_options.f_store_generation
                                            : 0;
    runtime_config.c_store_guid = structured_launch.active
                                      ? effective_options.c_store_guid
                                      : c_store_guid_for_root(legacy_root);
    std::unique_ptr<SidecarRuntime> runtime;
    try {
        runtime = std::make_unique<SidecarRuntime>(std::move(runtime_config));
    } catch (...) {
        cleanup_listener(listener, effective_options.socket_path, identity, prebound);
        return 2;
    }
    const bool structured_ready = structured_launch.active;
    if (g_stop_requested != 0 ||
        !(structured_ready ? write_ready_lease(ready.fd, effective_options, identity)
                           : write_ready(ready.fd))) {
        cleanup_listener(listener, effective_options.socket_path, identity, prebound);
        return 2;
    }
    (void)::close(ready.fd);
    ready.fd = -1;

    struct ControlWorker {
        std::thread thread;
        std::atomic<bool> done{false};
        std::atomic<int> cancel_fd{-1};
    };
    std::vector<std::shared_ptr<ControlWorker>> workers;
    workers.reserve(kMaxControlWorkers);

    const auto cancel_workers = [&workers] {
        for (const auto& worker : workers) {
            const int fd = worker->cancel_fd.exchange(-1, std::memory_order_acq_rel);
            if (fd >= 0) {
                (void)::shutdown(fd, SHUT_RDWR);
                (void)::close(fd);
            }
        }
    };
    const auto reap_workers = [&workers] {
        for (auto iterator = workers.begin(); iterator != workers.end();) {
            if (!(*iterator)->done.load(std::memory_order_acquire)) {
                ++iterator;
                continue;
            }
            if ((*iterator)->thread.joinable())
                (*iterator)->thread.join();
            iterator = workers.erase(iterator);
        }
    };

    for (;;) {
        if (g_stop_requested != 0)
            runtime->stop();
        reap_workers();
        if (g_stop_requested != 0) {
            cancel_workers();
            reap_workers();
        }
        if (g_stop_requested != 0 && workers.empty())
            break;

        struct pollfd descriptors[2]{};
        descriptors[0] = {wake.read.fd, POLLIN | POLLERR | POLLHUP, 0};
        nfds_t descriptor_count = 1;
        const bool can_accept = workers.size() < kMaxControlWorkers;
        if (can_accept)
            descriptors[descriptor_count++] = {listener, POLLIN | POLLERR | POLLHUP, 0};
        const int result = ::poll(descriptors, descriptor_count, kPollMilliseconds);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if ((descriptors[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            uint8_t drained[64]{};
            while (::read(wake.read.fd, drained, sizeof(drained)) > 0) {
            }
            if (g_stop_requested != 0)
                runtime->stop();
        }
        if (result == 0 || !can_accept || g_stop_requested != 0 || descriptor_count < 2 ||
            (descriptors[1].revents & POLLIN) == 0)
            continue;
        local::Status accept_status = local::Status::Ok;
        local::Connection connection = local::accept_unix(listener, &accept_status);
        if (!connection.valid())
            continue;
        auto worker = std::make_shared<ControlWorker>();
        const int cancel_fd = ::dup(connection.native_handle());
        if (cancel_fd < 0) {
            continue;
        }
        const int cancel_flags = ::fcntl(cancel_fd, F_GETFD);
        if (cancel_flags < 0 || ::fcntl(cancel_fd, F_SETFD, cancel_flags | FD_CLOEXEC) < 0) {
            (void)::close(cancel_fd);
            continue;
        }
        worker->cancel_fd.store(cancel_fd, std::memory_order_release);
        try {
            worker->thread = std::thread(
                [connection = std::move(connection), &effective_options,
                 runtime_ptr = runtime.get(),
                 worker]() mutable {
                    (void)handle_connection(std::move(connection), effective_options,
                                            *runtime_ptr);
                    const int fd = worker->cancel_fd.exchange(-1, std::memory_order_acq_rel);
                    if (fd >= 0)
                        (void)::close(fd);
                    worker->done.store(true, std::memory_order_release);
                });
            workers.emplace_back(std::move(worker));
        } catch (...) {
            const int fd = worker->cancel_fd.exchange(-1, std::memory_order_acq_rel);
            if (fd >= 0)
                (void)::close(fd);
            // The accepted connection is still owned by the local variable and
            // is closed on scope exit.  Keep the service alive for other slots.
        }
    }

    runtime->stop();
    cancel_workers();
    for (const auto& worker : workers) {
        if (worker->thread.joinable())
            worker->thread.join();
    }
    workers.clear();

    cleanup_listener(listener, effective_options.socket_path, identity, prebound);
    return 0;
}

} // namespace icecc::p50::service

#if !defined(ICECC_P50_CACHE_SERVICE_NO_MAIN)
int main(int argc, char** argv) {
    icecc::p50::service::Options options;
    bool show_help = false;
    if (!icecc::p50::service::parse_options(argc, argv, options, show_help)) {
        std::fprintf(stderr,
                     "usage: icecc-cache-service --socket ABSOLUTE --peer-uid UID "
                     "--peer-gid GID --generation N --attempt N [--drop-uid UID --drop-gid GID]\n");
        return 2;
    }
    if (show_help) {
        std::puts("usage: icecc-cache-service --socket ABSOLUTE --peer-uid UID --peer-gid GID "
                  "--generation N --attempt N [--drop-uid UID --drop-gid GID]");
        return 0;
    }
    return icecc::p50::service::run(options);
}
#endif
