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

bool parse_p29_interner_fault_injection(
    const char* value, P29InternerFaultInjection& result) noexcept {
    result = P29InternerFaultInjection::Disabled;
    if (value == nullptr)
        return true;
    if (std::strcmp(value, "P29_INTERNER_FAIL_ONCE") != 0)
        return false;
    result = P29InternerFaultInjection::FailOnce;
    return true;
}

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
// The cache service has a five-second pre-READY budget.  Keep fingerprint
// startup well inside it so SIGTERM cannot strand the daemon in an unbounded
// flock/hash wait before it can retire without publishing READY.
constexpr auto kP29FingerprintReadyBudget = std::chrono::milliseconds(2000);
constexpr int kMaxBacklog = 16;
// The complete source open/arm phase remains bounded by RuntimeConfig's
// five-second default.  A blackholed TCP/protocol four-tuple must not own that
// whole budget: retry only before P50SourceArmMsg exists on the wire, against
// the same selected numeric endpoint.  Any failure after channel creation is
// potentially post-arm and remains fail-closed without replay.
constexpr auto kSourceConnectAttemptBudget = std::chrono::seconds(1);
// A bounded control farm keeps an authenticated idle dispatcher or an active
// cache-wire handoff from consuming the only worker needed by compiler input.
// This is a hard concurrent cap, not a per-connection unbounded thread fork.
constexpr size_t kMaxControlWorkers = 64;

std::string bytes_hex(std::span<const uint8_t> bytes);

std::string daemon_cache_directory_from_socket(
    std::string_view socket_path) {
    const size_t leaf_separator = socket_path.rfind('/');
    if (leaf_separator == std::string_view::npos || leaf_separator == 0)
        return {};
    const std::string_view attempt_directory =
        socket_path.substr(0, leaf_separator);
    const size_t root_separator = attempt_directory.rfind('/');
    if (root_separator == std::string_view::npos)
        return {};
    return root_separator == 0
               ? std::string("/")
               : std::string(attempt_directory.substr(0, root_separator));
}

void append_ready_test_trace(std::string_view message) noexcept {
    const char* path = ::getenv("ICECC_P50_TEST_READY_TRACE");
    /* Supplying a private trace path is itself the opt-in.  Readiness is an
       infrastructure witness needed before canaries and must not depend on a
       workload's later strict-C1F1 policy knob. */
    if (path == nullptr || *path == '\0')
        return;
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    size_t offset = 0;
    while (offset < message.size()) {
        const ssize_t written = ::write(fd, message.data() + offset,
                                        message.size() - offset);
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

void append_source_result_trace(
    const local::P50SourceTransferRequest& request,
    CStoreGuid c_store_guid,
    ProfileId profile,
    const ZstdSourceTransferResult& transfer,
    uint64_t source_mutex_wait_ns,
    uint64_t source_mutex_service_ns) noexcept {
    const char* path = ::getenv("ICECC_P50_SOURCE_RESULT_TRACE");
    if (path == nullptr || *path == '\0')
        return;
    const std::string_view label =
        profile == ProfileId::P29V1
            ? std::string_view("P29V1")
            : profile == ProfileId::ZSTD_TU
                  ? std::string_view("ZSTD_TU")
                  : profile == ProfileId::ZSTD_ROUTE
                        ? std::string_view("ZSTD_ROUTE")
                        : std::string_view("UNKNOWN");
    const char* reuse = "null";
    if (transfer.system_source_reuse.has_value())
        reuse = *transfer.system_source_reuse ? "true" : "false";
    const uint16_t terminal_error_code =
        transfer.terminal_error.has_value() ? transfer.terminal_error->code : 0;
    const char* terminal_error_name =
        terminal_error_code == static_cast<uint16_t>(ErrorCode::WIRE_REVISION_MISMATCH)
            ? "\"WIRE_REVISION_MISMATCH\""
            : "null";
    const std::string c_guid = bytes_hex(std::span<const uint8_t>(
        c_store_guid.bytes.data(), c_store_guid.bytes.size()));
    const std::string raw_digest = icecc::digest128_hex(transfer.raw_digest);
    char line[1024];
    const int length = std::snprintf(
        line, sizeof(line),
        "{\"schema\":\"icecream-p50-source-result-v2\","
        "\"wire_job_id\":%llu,\"logical_job\":%llu,"
        "\"assignment_epoch\":%llu,\"assignment_nonce\":%llu,"
        "\"c_store_guid\":\"%s\","
        "\"profile\":\"%.*s\",\"status\":%u,\"attempts\":%u,"
        "\"tu_seq\":%llu,\"raw_bytes\":%llu,"
        "\"raw_digest\":\"%s\","
        "\"c_to_f_bytes\":%llu,\"f_to_c_bytes\":%llu,"
        "\"source_mutex_wait_ns\":%llu,"
        "\"source_mutex_service_ns\":%llu,"
        "\"terminal_error_code\":%u,"
        "\"terminal_error_name\":%s,"
        "\"system_source_reuse\":%s}\n",
        static_cast<unsigned long long>(request.wire_job_id),
        static_cast<unsigned long long>(request.logical_job),
        static_cast<unsigned long long>(request.assignment_epoch),
        static_cast<unsigned long long>(request.assignment_nonce),
        c_guid.c_str(),
        static_cast<int>(label.size()), label.data(),
        static_cast<unsigned>(transfer.status),
        static_cast<unsigned>(transfer.attempts),
        static_cast<unsigned long long>(
            transfer.committed_input.has_value()
                ? transfer.committed_input->tu_seq.value
                : 0),
        static_cast<unsigned long long>(transfer.raw_bytes),
        raw_digest.c_str(),
        static_cast<unsigned long long>(transfer.c_to_f_bytes),
        static_cast<unsigned long long>(transfer.f_to_c_bytes),
        static_cast<unsigned long long>(source_mutex_wait_ns),
        static_cast<unsigned long long>(source_mutex_service_ns),
        static_cast<unsigned>(terminal_error_code), terminal_error_name, reuse);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line))
        return;
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC |
                                   O_NOFOLLOW,
                          0600);
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

    const std::string c_store_guid = bytes_hex(std::span<const uint8_t>(
        request.key.c_store_guid.bytes.data(),
        request.key.c_store_guid.bytes.size()));
    char line[1024];
    const int length = std::snprintf(
        line, sizeof(line),
        "P50_LIFECYCLE pid=%lld generation=%llu attempt=%llu job=%llu "
        "epoch=%llu nonce=%llu c_store_guid=%s input_tu=%llu request=%llu "
        "action=%u status=%u "
        "before_records=%zu before_bytes=%llu after_records=%zu "
        "after_bytes=%llu\n",
        static_cast<long long>(::getpid()),
        static_cast<unsigned long long>(request.identity.generation),
        static_cast<unsigned long long>(request.identity.attempt),
        static_cast<unsigned long long>(request.owner.logical_job),
        static_cast<unsigned long long>(request.owner.assignment_epoch),
        static_cast<unsigned long long>(request.owner.assignment_nonce),
        c_store_guid.c_str(),
        static_cast<unsigned long long>(request.key.tu_seq.value),
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
    const bool written = write_exact(fd, message);
    if (written)
        append_ready_test_trace(message);
    return written;
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
        const local::Status receive_status =
            connection.receive_until(operation_frame, deadline);
        if (receive_status != local::Status::Ok) {
            // A daemon may close an authenticated idle relationship without
            // starting an operation.  That ordinary EOF needs no diagnostic.
            if (receive_status != local::Status::CleanEof) {
                std::fprintf(stderr,
                             "P50_CONTROL_REFUSED stage=operation-frame "
                             "receive=%s\n",
                             local::status_name(receive_status));
                std::fflush(stderr);
            }
            return true;
        }
        const local::Status identity_status =
            local::validate_identity(operation_frame, options.identity);
        if (operation_frame.type != local::MessageType::Data ||
            identity_status != local::Status::Ok) {
            std::fprintf(stderr,
                         "P50_CONTROL_REFUSED stage=operation-frame receive=ok "
                         "type=%u identity=%s\n",
                         static_cast<unsigned>(operation_frame.type),
                         local::status_name(identity_status));
            std::fflush(stderr);
            return true;
        }
        local::ControlOperation operation;
        const bool operation_decoded =
            local::decode_control_operation(operation_frame.payload, operation);
        if (!operation_decoded || operation.identity != options.identity ||
            operation.request_id == 0) {
            std::fprintf(stderr,
                         "P50_CONTROL_REFUSED stage=operation-decode decoded=%u "
                         "payload_bytes=%zu generation=%llu attempt=%llu request=%llu\n",
                         operation_decoded ? 1u : 0u,
                         operation_frame.payload.size(),
                         static_cast<unsigned long long>(operation.identity.generation),
                         static_cast<unsigned long long>(operation.identity.attempt),
                         static_cast<unsigned long long>(operation.request_id));
            std::fflush(stderr);
            return true;
        }

        if (operation.kind == local::ControlOperationKind::CacheSession) {
            /*
             * Complete the authenticated one-shot public-descriptor handoff,
             * then move the adopted socket directly to the endpoint owner.
             * This is the positive production bridge; run_one() is retained
             * only for historical fixtures and is intentionally not called.
             */
            local::FdHandoffReceiver receiver;
            const local::FdHandoffResult handoff =
                receiver.receive_and_ack(connection, operation.request_id == 0
                                                     ? local::HandoffRequest{}
                                                     : local::HandoffRequest{
                                                           options.identity,
                                                         operation.request_id},
                                         deadline);
            if (handoff.status != local::FdHandoffStatus::Accepted) {
                std::fprintf(
                    stderr,
                    "P50_CACHE_SESSION_REFUSED stage=fd-handoff request=%llu "
                    "status=%s state=%u\n",
                    static_cast<unsigned long long>(operation.request_id),
                    local::fd_handoff_status_name(handoff.status),
                    static_cast<unsigned>(handoff.sender_state));
                std::fflush(stderr);
                return true;
            }
            local::HandoffFd adopted = receiver.take_adopted_fd();
            if (!adopted.valid()) {
                std::fprintf(
                    stderr,
                    "P50_CACHE_SESSION_REFUSED stage=adopt request=%llu\n",
                    static_cast<unsigned long long>(operation.request_id));
                std::fflush(stderr);
                return true;
            }
            errno = 0;
            if (!send_cache_session_ready(adopted.get(), deadline)) {
                std::fprintf(
                    stderr,
                    "P50_CACHE_SESSION_REFUSED stage=ready request=%llu "
                    "errno=%d\n",
                    static_cast<unsigned long long>(operation.request_id), errno);
                std::fflush(stderr);
                return true;
            }
            std::fprintf(
                stderr, "P50_CACHE_SESSION_READY request=%llu\n",
                static_cast<unsigned long long>(operation.request_id));
            std::fflush(stderr);
            runtime.start_adopted_endpoint(adopted.release());
            return true;
        }
        if (operation.kind == local::ControlOperationKind::SourceTransfer) {
            const auto clock = sidecar::process_monotonic_clock_identity();
            const int64_t now_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            const bool source_present = operation.source_arm.has_value();
            const bool deadline_valid = operation.absolute_deadline.valid();
            const bool clock_matches =
                operation.absolute_deadline.matches_clock(clock);
            const bool deadline_expired = operation.absolute_deadline.expired(
                now_ns, clock.clock_domain_id, clock.time_namespace_id);
            if (!source_present || !deadline_valid || !clock_matches ||
                deadline_expired) {
                std::fprintf(
                    stderr,
                    "P50_SOURCE_TRANSFER_REFUSED stage=deadline request=%llu "
                    "source=%u valid=%u clock_match=%u expired=%u now_ns=%lld "
                    "expires_ns=%lld operation_clock=%llu "
                    "operation_time_namespace=%llu local_clock=%llu "
                    "local_time_namespace=%llu\n",
                    static_cast<unsigned long long>(operation.request_id),
                    source_present ? 1u : 0u, deadline_valid ? 1u : 0u,
                    clock_matches ? 1u : 0u, deadline_expired ? 1u : 0u,
                    static_cast<long long>(now_ns),
                    static_cast<long long>(operation.absolute_deadline.expires_at_ns),
                    static_cast<unsigned long long>(
                        operation.absolute_deadline.clock_domain_id),
                    static_cast<unsigned long long>(
                        operation.absolute_deadline.time_namespace_id),
                    static_cast<unsigned long long>(clock.clock_domain_id),
                    static_cast<unsigned long long>(clock.time_namespace_id));
                std::fflush(stderr);
                return true;
            }
            local::FdHandoffReceiver receiver;
            const local::FdHandoffResult handoff = receiver.receive_and_ack(
                connection, local::HandoffRequest{options.identity, operation.request_id},
                operation.absolute_deadline.as_steady_time_point());
            if (handoff.status != local::FdHandoffStatus::Accepted) {
                std::fprintf(stderr,
                             "P50_SOURCE_TRANSFER_REFUSED stage=fd-handoff "
                             "request=%llu status=%s state=%u\n",
                             static_cast<unsigned long long>(operation.request_id),
                             local::fd_handoff_status_name(handoff.status),
                             static_cast<unsigned>(handoff.sender_state));
                std::fflush(stderr);
                return true;
            }
            local::HandoffFd source = receiver.take_adopted_fd();
            const local::P50SourceTransferResult transfer =
                runtime.transfer_source_on_owner(
                    *operation.source_arm, operation.absolute_deadline,
                    std::move(source));
            if (transfer.code != local::SourceTransferResultCode::Committed) {
                std::fprintf(stderr,
                             "P50_SOURCE_TRANSFER_REFUSED stage=owner-result "
                             "request=%llu code=%u error=%u attempts=%u\n",
                             static_cast<unsigned long long>(operation.request_id),
                             static_cast<unsigned>(transfer.code),
                             static_cast<unsigned>(transfer.error_code),
                             static_cast<unsigned>(transfer.attempts));
                std::fflush(stderr);
            }
            const std::vector<uint8_t> response_payload =
                local::encode_control_operation(
                    local::make_source_transfer_reply_operation(operation, transfer));
            if (response_payload.empty()) {
                std::fprintf(stderr,
                             "P50_SOURCE_TRANSFER_REFUSED stage=response-encode "
                             "request=%llu\n",
                             static_cast<unsigned long long>(operation.request_id));
                std::fflush(stderr);
                return true;
            }
            const auto operation_deadline = operation.absolute_deadline.as_steady_time_point();
            const local::Frame response{local::kProtocolVersion,
                                        local::MessageType::Data,
                                        options.identity, response_payload};
            const local::Status response_status =
                connection.send_until(response, operation_deadline);
            if (response_status != local::Status::Ok) {
                std::fprintf(stderr,
                             "P50_SOURCE_TRANSFER_REFUSED stage=response-send "
                             "request=%llu status=%s\n",
                             static_cast<unsigned long long>(operation.request_id),
                             local::status_name(response_status));
                std::fflush(stderr);
                return true;
            }
            local::Frame acknowledgement;
            const local::Status acknowledgement_status =
                connection.receive_until(acknowledgement, operation_deadline);
            const local::Status acknowledgement_identity =
                acknowledgement_status == local::Status::Ok
                    ? local::validate_identity(acknowledgement, options.identity)
                    : local::Status::IdentityMismatch;
            if (acknowledgement_status != local::Status::Ok ||
                acknowledgement.type != local::MessageType::Goodbye ||
                !acknowledgement.payload.empty() ||
                acknowledgement_identity != local::Status::Ok) {
                std::fprintf(stderr,
                             "P50_SOURCE_TRANSFER_REFUSED stage=response-ack "
                             "request=%llu receive=%s type=%u payload_bytes=%zu "
                             "identity=%s\n",
                             static_cast<unsigned long long>(operation.request_id),
                             local::status_name(acknowledgement_status),
                             static_cast<unsigned>(acknowledgement.type),
                             acknowledgement.payload.size(),
                             local::status_name(acknowledgement_identity));
                std::fflush(stderr);
                return true;
            }
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
            if (std::getenv("ICECC_P50_DEBUG_ATTACH") != nullptr) {
                std::fprintf(stderr,
                             "P50 sidecar input attachment request=%llu status=%s handoff=%s fd=%s\n",
                             static_cast<unsigned long long>(request.request_id),
                             input_fd_attachment_status_name(result.status),
                             fd_handoff_status_name(result.handoff.status),
                             result.fd.valid() ? "valid" : "invalid");
                std::fflush(stderr);
            }
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
        config.max_route_completed_requests == 0 ||
        config.max_route_relationships == 0 ||
        config.max_route_endpoint_identities == 0 ||
        config.source_open_arm_timeout <= std::chrono::milliseconds::zero() ||
        config.source_open_arm_timeout > std::chrono::seconds(60) ||
        config.cancellation_grace <= std::chrono::milliseconds::zero())
        throw std::invalid_argument(
            "sidecar runtime bounds must be nonzero");
    if (config.c_store_guid == CStoreGuid{})
        throw std::invalid_argument("sidecar runtime requires a nonzero C_STORE_GUID");
    if (config.c_store_guid == config.f_store_guid)
        throw std::invalid_argument("sidecar runtime requires distinct C/F store GUIDs");
    return config;
}

local::P50SourceTransferResult source_transfer_error(uint16_t code,
                                                     uint8_t attempts = 0) noexcept {
    local::P50SourceTransferResult result;
    result.code = local::SourceTransferResultCode::Error;
    result.error_code = code == 0 ? 1 : code;
    result.attempts = attempts;
    return result;
}

local::P50SourceTransferResult source_transfer_result(
    const ZstdSourceTransferResult& transfer, CStoreGuid expected_c_guid) noexcept {
    if (transfer.replacement_required) {
        return source_transfer_error(static_cast<uint16_t>(
            local::SourceTransferErrorCode::RouteReplacementRequired),
            transfer.attempts);
    }
    if (transfer.status != ZstdSourceTransferStatus::Committed ||
        !transfer.committed_input.has_value() ||
        transfer.raw_bytes == 0 || transfer.raw_digest == Digest128{} ||
        transfer.committed_input->c_store_guid != expected_c_guid)
        return source_transfer_error(static_cast<uint16_t>(transfer.status) + 1,
                                     transfer.attempts);
    local::P50SourceTransferResult result;
    result.code = local::SourceTransferResultCode::Committed;
    result.tu_seq = transfer.committed_input->tu_seq.value;
    result.raw_bytes = transfer.raw_bytes;
    result.raw_digest = transfer.raw_digest;
    result.attempts = transfer.attempts;
    result.c_store_guid = expected_c_guid;
    return result;
}

std::optional<std::shared_ptr<const std::vector<uint8_t>>> read_source_fd(
    int fd, uint64_t limit) noexcept {
    if (fd < 0 || limit > SIZE_MAX)
        return std::nullopt;
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > limit)
        return std::nullopt;
    try {
        auto result = std::make_shared<std::vector<uint8_t>>(
            static_cast<size_t>(info.st_size));
        size_t offset = 0;
        while (offset != result->size()) {
            const ssize_t count = ::pread(fd, result->data() + offset,
                                          result->size() - offset,
                                          static_cast<off_t>(offset));
            if (count > 0) {
                offset += static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            return std::nullopt;
        }
        return std::const_pointer_cast<const std::vector<uint8_t>>(result);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

SidecarRuntime::SidecarRuntime(RuntimeConfig config)
    : config_(validate_runtime_config(std::move(config))),
      input_lifecycle_(
          config_.endpoint_config.owner_limits.max_retained_input_records,
          config_.max_input_lifecycle_replays),
      endpoint_work_guard_(asio::make_work_guard(context_)),
      // One C sidecar may serve twenty persistent relationships, with two
      // compile slots per F.  Keep the operation table bounded while leaving
      // room for every concurrently admitted C/F session (40) plus control
      // churn during replacement.
      fsession_owner_(64, config_.f_store_generation) {
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
    config_.endpoint_config.on_input_committed =
        [this](InputRecordKey key, bool retained) {
            // Publication and lifecycle ownership are one owner-affine edge.
            // This runs before TX_COMMIT is written, so an F-side attachment
            // cannot observe the record in its pre-commit state.
            const bool observed = input_lifecycle_.observe_route_commit(key, retained);
            if (std::getenv("ICECC_P50_DEBUG_ATTACH") != nullptr)
                std::fprintf(stderr, "P50 sidecar lifecycle commit tu=%llu retained=%d observed=%d\n",
                             static_cast<unsigned long long>(key.tu_seq.value),
                             int(retained), int(observed));
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
    P50RouteOwnerConfig route_config;
    route_config.endpoint_caps = config_.endpoint_caps;
    route_config.max_completed_requests = config_.max_route_completed_requests;
    route_config.max_relationships = config_.max_route_relationships;
    route_config.compression_level = 3;
    route_config.p29_interner_fault_injection =
        config_.p29_interner_fault_injection;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    route_config.before_prepare_for_route_for_test =
        std::move(config_.before_route_prepare_for_test);
#else
    if (route_config.before_prepare_for_route_for_test)
        throw std::logic_error(
            "production sidecar cannot install the route-poison test hook");
#endif
    route_owner_ = std::make_unique<P50CRouteOwner>(std::move(route_config));
    endpoint_owner_thread_ = std::thread([this] { endpoint_owner_loop(); });
}

local::P50SourceTransferResult SidecarRuntime::transfer_source_on_owner(
    local::P50SourceTransferRequest request,
    sidecar::AbsoluteMonotonicDeadline deadline,
    local::HandoffFd source) noexcept {
    const auto clock = sidecar::process_monotonic_clock_identity();
    if (!route_owner_ || !request.valid() || !config_.sidecar_launch.has_value() ||
        !config_.sidecar_launch->valid() ||
        config_.sidecar_launch->c_store_guid != config_.c_store_guid ||
        config_.f_store_guid == FStoreGuid{} || config_.f_store_generation == 0 ||
        !deadline.valid() ||
        !deadline.matches_clock(clock) ||
        deadline.expired(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count(),
                         clock.clock_domain_id, clock.time_namespace_id) ||
        !source.valid())
        return source_transfer_error(1);

    const auto transfer_deadline = deadline.as_steady_time_point();
    const auto source_mutex_wait_start = std::chrono::steady_clock::now();
    std::unique_lock<std::timed_mutex> source_transfer_lock(
        source_transfer_mutex_, std::defer_lock);
    constexpr auto kSourceTransferLockPoll = std::chrono::milliseconds(50);
    for (;;) {
        if (stop_requested_.load(std::memory_order_acquire))
            return source_transfer_error(7);
        const auto now = std::chrono::steady_clock::now();
        if (now >= transfer_deadline)
            return source_transfer_error(7);
        if (source_transfer_lock.try_lock_until(
                std::min(transfer_deadline, now + kSourceTransferLockPoll)))
            break;
    }
    const auto source_mutex_service_start = std::chrono::steady_clock::now();
    const auto source_mutex_wait_elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            source_mutex_service_start - source_mutex_wait_start)
            .count();
    const uint64_t source_mutex_wait_ns =
        source_mutex_wait_elapsed > 0
            ? static_cast<uint64_t>(source_mutex_wait_elapsed)
            : 0;
    if (stop_requested_.load(std::memory_order_acquire) ||
        std::chrono::steady_clock::now() >= transfer_deadline)
        return source_transfer_error(7);
    if (route_replacement_required_.load(std::memory_order_acquire))
        return source_transfer_error(static_cast<uint16_t>(
            local::SourceTransferErrorCode::RouteReplacementRequired));

    // Capacity is decidable from the immutable selected endpoint before
    // reading C's source or opening/claiming F.  Refuse a novel endpoint at
    // the process-lifetime bound without creating the detached F owner that
    // the later identity-binding check exists to protect.
    std::optional<RouteStoreIdentity> known_endpoint_identity;
    for (const auto& [endpoint, identity] : route_endpoint_identities_) {
        if (endpoint.host == request.selected_f_host &&
            endpoint.cache_port == request.selected_f_cache_port) {
            known_endpoint_identity = identity;
            break;
        }
    }
    if (!known_endpoint_identity.has_value() &&
        route_endpoint_identities_.size() >=
            config_.max_route_endpoint_identities) {
        route_replacement_required_.store(true, std::memory_order_release);
        return source_transfer_error(static_cast<uint16_t>(
            local::SourceTransferErrorCode::RouteReplacementRequired));
    }

    P50SourceArmFields arm;
    arm.wire_job_id = request.wire_job_id;
    arm.assignment_epoch = request.assignment_epoch;
    arm.assignment_nonce = request.assignment_nonce;
    arm.selected_f_host = request.selected_f_host;
    arm.selected_f_ordinary_port = request.selected_f_ordinary_port;
    arm.selected_f_cache_port = request.selected_f_cache_port;
    arm.cache_protocol = request.cache_protocol;
    arm.cache_profile = request.cache_profile;
    arm.logical_job = request.logical_job;
    arm.compiler_attempt = request.compiler_attempt;
    arm.c_store_generation = config_.sidecar_launch->store_generation;
    arm.c_store_derivation_version = kStoreIdentityDerivationVersion;
    arm.c_store_guid = config_.c_store_guid.bytes;
    arm.source_request_id = request.source_request_id;
    arm.source_mode = request.source_mode;
    arm.c_control_generation = config_.sidecar_launch->identity.generation;
    arm.c_control_attempt = config_.sidecar_launch->identity.attempt;
    if (!arm.valid())
        return source_transfer_error(1);

    ProfileId profile = ProfileId::ZSTD_TU;
    if (arm.cache_profile == CACHE_PROFILE_ZSTD_ROUTE)
        profile = ProfileId::ZSTD_ROUTE;
    else if (arm.cache_profile == CACHE_PROFILE_P29V1)
        profile = ProfileId::P29V1;
    else if (arm.cache_profile == CACHE_PROFILE_ZSTD_TU)
        profile = ProfileId::ZSTD_TU;
    else
        return source_transfer_error(2);

    // For an already authenticated endpoint, its exact F incarnation and the
    // requested profile make the relationship key knowable before opening F.
    // Do not claim/arm a worker when the bounded owner table cannot retain
    // that new key.  A novel endpoint cannot use this preflight: its address
    // may name an already retained incarnation, which is learned only from
    // the authenticated arm acknowledgement.
    if (known_endpoint_identity.has_value()) {
        const P50RouteRelationship known_relationship{
            config_.c_store_guid, known_endpoint_identity->guid,
            known_endpoint_identity->generation, profile};
        if (!route_owner_->owns(known_relationship) &&
            route_owner_->owner_count() >= config_.max_route_relationships) {
            route_replacement_required_.store(true, std::memory_order_release);
            return source_transfer_error(static_cast<uint16_t>(
                local::SourceTransferErrorCode::RouteReplacementRequired));
        }
    }
    const auto source_read_start = std::chrono::steady_clock::now();
    const auto source_bytes = read_source_fd(
        source.get(), config_.endpoint_caps.zstd.max_raw_bytes);
    const auto source_read_elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - source_read_start)
            .count();
    const uint64_t source_read_ns =
        source_read_elapsed > 0 ? static_cast<uint64_t>(source_read_elapsed) : 0;
    if (!source_bytes.has_value()) {
        std::fprintf(stderr,
                     "P50_SOURCE_TRANSFER_REFUSED stage=source-read request=%llu "
                     "f_host=%s f_cache_port=%u source_read_ns=%llu\n",
                     static_cast<unsigned long long>(arm.source_request_id),
                     arm.selected_f_host.c_str(), arm.selected_f_cache_port,
                     static_cast<unsigned long long>(source_read_ns));
        std::fflush(stderr);
        return source_transfer_error(3);
    }

    const PrepareRequestKey route_request{arm.assignment_epoch,
                                          arm.assignment_nonce};

    struct PendingFd {
        int fd = -1;
        ~PendingFd() { if (fd >= 0) ::close(fd); }
    };
    auto open_armed = [arm, source_read_ns,
                       open_arm_timeout = config_.source_open_arm_timeout](
                          std::chrono::steady_clock::time_point outer_limit,
                          FStoreGuid& remote_guid,
                          uint64_t& remote_generation) {
        const auto open_arm_start = std::chrono::steady_clock::now();
        const auto limit = std::min(
            outer_limit, open_arm_start + open_arm_timeout);
        auto stage_start = open_arm_start;
        const auto refused = [&arm, source_read_ns, open_arm_start,
                              &stage_start, open_arm_timeout](
                                 const char* stage, long long detail = 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto open_arm_elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - open_arm_start)
                    .count();
            const auto stage_elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - stage_start)
                    .count();
            std::fprintf(stderr,
                         "P50_SOURCE_TRANSFER_REFUSED stage=%s request=%llu "
                         "f_host=%s f_cache_port=%u detail=%lld "
                         "source_read_ns=%llu open_arm_budget_ms=%lld "
                         "open_arm_elapsed_ns=%llu stage_elapsed_ns=%llu\n",
                         stage,
                         static_cast<unsigned long long>(arm.source_request_id),
                         arm.selected_f_host.c_str(), arm.selected_f_cache_port,
                         detail,
                         static_cast<unsigned long long>(source_read_ns),
                         static_cast<long long>(open_arm_timeout.count()),
                         static_cast<unsigned long long>(
                             open_arm_elapsed > 0 ? open_arm_elapsed : 0),
                         static_cast<unsigned long long>(
                             stage_elapsed > 0 ? stage_elapsed : 0));
            std::fflush(stderr);
            return -1;
        };
        try {
            std::unique_ptr<MsgChannel> channel(Service::createChannelRetryUntil(
                arm.selected_f_host,
                static_cast<unsigned short>(arm.selected_f_cache_port), limit,
                kSourceConnectAttemptBudget,
                Service::ChannelRetryPolicy::HedgeAfterFirst));
            if (!channel)
                return refused("f-connect");
            if (channel->protocol != PROTOCOL_VERSION_CACHE_ADVERTISEMENT)
                return refused("f-protocol", channel->protocol);
            if (std::chrono::steady_clock::now() >= limit)
                return refused("f-connect-deadline");
            stage_start = std::chrono::steady_clock::now();
            const P50SourceArmMsg request_message(arm);
            if (!channel->send_msg(request_message, MsgChannel::SendNonBlocking))
                return refused("f-arm-send");
            stage_start = std::chrono::steady_clock::now();
            std::unique_ptr<Msg> response(channel->get_msg_until(limit));
            const auto* acknowledgement = response != nullptr
                                              ? dynamic_cast<P50SourceArmedMsg*>(response.get())
                                              : nullptr;
            if (acknowledgement == nullptr)
                return refused("f-arm-reply");
            if (!acknowledgement->acknowledges(request_message))
                return refused("f-arm-mismatch");
            if (std::chrono::steady_clock::now() >= limit)
                return refused("f-arm-deadline");
            stage_start = std::chrono::steady_clock::now();
            remote_guid.bytes = acknowledgement->f_store_guid;
            remote_generation = acknowledgement->f_store_generation;
            if (remote_guid == FStoreGuid{} || remote_generation == 0)
                return refused("f-store-identity");
            if (!channel->send_msg(CacheSessionMsg(), MsgChannel::SendNonBlocking))
                return refused("f-cache-session-send");
            stage_start = std::chrono::steady_clock::now();
            const int ready_fd = channel->release_fd_after_cache_session_ready(limit);
            return ready_fd >= 0 ? ready_fd : refused("f-cache-session-ready");
        } catch (...) {
            return refused("f-open-exception");
        }
    };

    FStoreGuid remote_f_guid;
    uint64_t remote_f_generation = 0;
    const int first_fd = open_armed(transfer_deadline, remote_f_guid,
                                    remote_f_generation);
    if (first_fd < 0)
        return source_transfer_error(4);
    auto first = std::make_shared<PendingFd>();
    first->fd = first_fd;
    const P50RouteRelationship relationship{
        config_.c_store_guid, remote_f_guid, remote_f_generation, profile};
    const ConnectedFdFactory connection =
        [first, open_armed, expected_guid = remote_f_guid,
         expected_generation = remote_f_generation](
            std::chrono::steady_clock::time_point limit) mutable {
            if (first->fd >= 0) {
                const int fd = first->fd;
                first->fd = -1;
                return fd;
            }
            FStoreGuid observed_guid;
            uint64_t observed_generation = 0;
            const int fd = open_armed(limit, observed_guid, observed_generation);
            if (fd < 0 || observed_guid != expected_guid ||
                observed_generation != expected_generation) {
                if (fd >= 0)
                    ::close(fd);
                return -1;
            }
            return fd;
        };

    auto completion = std::make_shared<std::promise<local::P50SourceTransferResult>>();
    std::future<local::P50SourceTransferResult> result = completion->get_future();
    try {
        asio::co_spawn(
            context_,
            [this, relationship, request, connection, transfer_deadline,
             source_bytes = *source_bytes, completion, route_request,
             expected_c_guid = config_.c_store_guid, source_mutex_wait_ns,
             source_mutex_service_start]() mutable
                -> asio::awaitable<void> {
                local::P50SourceTransferResult value = source_transfer_error(4);
                ZstdSourceTransferResult observed;
                try {
                    const RouteEndpointKey endpoint_key{
                        request.selected_f_host, request.selected_f_cache_port};
                    const RouteStoreIdentity store_identity{
                        relationship.f_store_guid,
                        relationship.f_store_generation};
                    if (!bind_route_endpoint_identity(endpoint_key,
                                                      store_identity)) {
                        observed.status = ZstdSourceTransferStatus::Unavailable;
                        observed.profile = relationship.profile;
                        observed.replacement_required = true;
                    } else {
                        observed = co_await route_owner_->transfer(
                            relationship, route_request, connection,
                            transfer_deadline,
                            std::span<const uint8_t>(*source_bytes));
                    }
                    if (observed.replacement_required)
                        route_replacement_required_.store(
                            true, std::memory_order_release);
                    value = source_transfer_result(observed, expected_c_guid);
                } catch (const P29V1CapabilityUnavailable&) {
                    value = source_transfer_error(static_cast<uint16_t>(
                        local::SourceTransferErrorCode::
                            PermanentLocalProfileUnavailable));
                } catch (...) {
                    observed.status = ZstdSourceTransferStatus::TerminalError;
                    observed.profile = relationship.profile;
                    observed.replacement_required = true;
                    route_replacement_required_.store(
                        true, std::memory_order_release);
                    value = source_transfer_result(observed, expected_c_guid);
                }
                const auto source_mutex_service_elapsed =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        source_mutex_service_start)
                        .count();
                const uint64_t source_mutex_service_ns =
                    source_mutex_service_elapsed > 0
                        ? static_cast<uint64_t>(source_mutex_service_elapsed)
                        : 0;
                append_source_result_trace(
                    request, expected_c_guid, relationship.profile, observed,
                    source_mutex_wait_ns, source_mutex_service_ns);
                completion->set_value(value);
                co_return;
            },
            asio::detached);
    } catch (...) {
        route_replacement_required_.store(true, std::memory_order_release);
        return source_transfer_error(static_cast<uint16_t>(
            local::SourceTransferErrorCode::RouteReplacementRequired));
    }
    // The route owner uses the same absolute deadline for connect, arm, and
    // CacheWire.  A bounded grace lets the owner coroutine publish its typed
    // terminal result without allowing a control worker to wait forever.
    const auto wait_limit = transfer_deadline + config_.cancellation_grace;
    if (result.wait_until(wait_limit) != std::future_status::ready) {
        // The coroutine still owns the retained route state.  Returning would
        // release source_transfer_mutex_ and admit a successor concurrently
        // with that live operation, recreating the overlap this gate forbids.
        // Retire the supervised sidecar instead of exposing ambiguous state.
        if (config_.fail_stop)
            config_.fail_stop();
        std::_Exit(125);
    }
    return result.get();
}

bool SidecarRuntime::bind_route_endpoint_identity(
    const RouteEndpointKey& endpoint,
    RouteStoreIdentity observed) noexcept {
    if (!route_owner_ || endpoint.host.empty() || endpoint.cache_port == 0 ||
        observed.guid == FStoreGuid{} || observed.generation == 0)
        return false;
    const auto position = route_endpoint_identities_.find(endpoint);
    if (position == route_endpoint_identities_.end()) {
        if (route_endpoint_identities_.size() >=
            config_.max_route_endpoint_identities)
            return false;
        try {
            return route_endpoint_identities_.emplace(endpoint, observed).second;
        } catch (...) {
            return false;
        }
    }
    if (position->second == observed)
        return true;

    // The map still names the predecessor until every one of its profile
    // owners is retired.  Passing the newly observed GUID here would leak old
    // history into the successor incarnation.
    if (!route_owner_->reset_f_store_exact(position->second.guid,
                                           position->second.generation))
        return false;
    position->second = observed;
    return true;
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
            std::fprintf(stderr,
                         "P50_CACHE_SESSION_ENDPOINT_REFUSED stage=adopt errno=%d\n",
                         adoption_error.value());
            std::fflush(stderr);
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
        std::fprintf(stderr, "P50_CACHE_SESSION_ENDPOINT_START\n");
        std::fflush(stderr);
        const ServerRunResult endpoint_result =
            co_await endpoint_->run_adopted(std::move(*socket), std::move(endpoint_control));
        std::fprintf(stderr,
                     "P50_CACHE_SESSION_ENDPOINT_DONE status=%u completed=%u "
                     "committed=%u\n",
                     static_cast<unsigned>(endpoint_result.status),
                     endpoint_result.completed_input.has_value() ? 1u : 0u,
                     endpoint_result.committed_input.has_value() ? 1u : 0u);
        std::fflush(stderr);
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
        std::fprintf(stderr, "P50_CACHE_SESSION_ENDPOINT_EXCEPTION\n");
        std::fflush(stderr);
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

void SidecarRuntime::start_adopted_endpoint(
    int adopted_fd, EndpointIoControl endpoint_control) noexcept {
    if (adopted_fd < 0)
        return;
    std::promise<EndpointOwnerResult> completion;
    try {
        // The coroutine is the sole owner of the adopted descriptor after
        // this call.  No worker thread runs endpoint code or re-enters the
        // public CacheWire parser; the existing owner executor does so.
        asio::co_spawn(context_,
                       run_endpoint_on_owner(adopted_fd,
                                              std::move(endpoint_control),
                                              std::move(completion), -1),
                       asio::detached);
    } catch (...) {
        (void)::close(adopted_fd);
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

#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
bool SidecarRuntime::seed_route_endpoint_identity_for_test(
    std::string host, uint32_t cache_port, FStoreGuid guid,
    uint64_t generation) noexcept {
    std::unique_lock<std::timed_mutex> lock(source_transfer_mutex_,
                                            std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    return bind_route_endpoint_identity(
        RouteEndpointKey{std::move(host), cache_port},
        RouteStoreIdentity{guid, generation});
}

bool SidecarRuntime::seed_route_relationship_for_test(
    std::string host, uint32_t cache_port, FStoreGuid guid,
    uint64_t generation, ProfileId profile) noexcept {
    std::unique_lock<std::timed_mutex> lock(source_transfer_mutex_,
                                            std::try_to_lock);
    if (!lock.owns_lock() || !route_owner_)
        return false;
    const RouteEndpointKey endpoint{std::move(host), cache_port};
    const RouteStoreIdentity identity{guid, generation};
    if (!bind_route_endpoint_identity(endpoint, identity))
        return false;
    return route_owner_->seed_relationship_for_test(P50RouteRelationship{
        config_.c_store_guid, guid, generation, profile});
}
#endif

void SidecarRuntime::cancel_endpoint_run() noexcept {
    // The permit is captured at admission and posted to the endpoint owner;
    // no current-socket lookup or descriptor-derived authority is permitted.
    try {
        std::optional<EndpointCancelPermit> permit;
        {
            std::lock_guard lock(endpoint_cancel_mutex_);
            permit = endpoint_cancel_permit_;
        }
        if (permit) {
            context_.post([this, permit = std::move(*permit)] {
                (void)endpoint_->request_cancel(permit);
            });
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
        } else {
            // Historical raw-fd fixtures do not carry a P5CO operation and
            // therefore cannot mint a production cancellation permit.  Keep
            // their owner-wakeup coverage inside the test-hooks build only.
            context_.post([this] { endpoint_->request_cancel_for_test(); });
#endif
        }
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
    // Start the system-source digest after the credential transition and
    // complete it before READY.  Route state pins its fingerprint at first
    // admission, so publishing READY while the digest is still zero would
    // disable reuse for that relationship's lifetime.  Structured launches
    // persist their per-file digest cache in the daemon-owned runtime
    // directory, one level above the per-incarnation attempt leaf.
    start_p29_system_source_fingerprint(
        structured_launch.active
            ? daemon_cache_directory_from_socket(
                  effective_options.socket_path)
            : std::string{});
    const P29FingerprintOutcome fingerprint_outcome =
        wait_p29_system_source_fingerprint_for(kP29FingerprintReadyBudget);
    switch (fingerprint_outcome) {
    case P29FingerprintOutcome::Completed:
    case P29FingerprintOutcome::Unavailable:
    case P29FingerprintOutcome::TimedOut:
        break;
    case P29FingerprintOutcome::Cancelled:
        return 2;
    }
    if (g_stop_requested != 0) {
        cancel_p29_system_source_fingerprint();
        return 2;
    }
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
    const char* p29_fault = ::getenv("ICECC_P50_FAULT_INJECTION");
    if (!parse_p29_interner_fault_injection(
            p29_fault, runtime_config.p29_interner_fault_injection)) {
        cleanup_listener(listener, effective_options.socket_path, identity,
                         prebound);
        return 2;
    }
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
    if (structured_launch.active) {
        runtime_config.sidecar_launch = SidecarLaunchIdentity{
            structured_launch.identity,
            structured_launch.f_store_generation,
            structured_launch.store_root,
            structured_launch.c_store_guid,
            structured_launch.f_store_guid,
        };
    }
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
