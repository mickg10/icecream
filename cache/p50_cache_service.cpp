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

std::optional<ProfileId> profile_from_cache_profile_mask(
    uint32_t cache_profile_mask) noexcept {
    switch (cache_profile_mask) {
    case CACHE_PROFILE_P29V1:
        return ProfileId::P29V1;
    case CACHE_PROFILE_ZSTD_TU:
        return ProfileId::ZSTD_TU;
    case CACHE_PROFILE_ZSTD_ROUTE:
        return ProfileId::ZSTD_ROUTE;
    default:
        return std::nullopt;
    }
}

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

void append_test_trace(const char* environment, std::string_view message) noexcept {
    const char* path = ::getenv(environment);
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

void append_ready_test_trace(std::string_view message) noexcept {
    append_test_trace("ICECC_P50_TEST_READY_TRACE", message);
}

void append_fingerprint_test_trace(std::string_view message) noexcept {
    append_test_trace("ICECC_P50_TEST_FINGERPRINT_TRACE", message);
}

void append_source_result_trace(
    const local::P50SourceTransferRequest& request,
    CStoreGuid c_store_guid,
    ProfileId profile,
    const ZstdSourceTransferResult& transfer,
    uint64_t admission_wait_ns,
    uint64_t admitted_service_ns) noexcept {
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
        "{\"schema\":\"icecream-p50-source-result-v3\","
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
        static_cast<unsigned long long>(admission_wait_ns),
        static_cast<unsigned long long>(admitted_service_ns),
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
        const local::Status peer_status =
            connection.verify_peer_credentials(options.expected_peer);
        if (peer_status != local::Status::Ok) {
            std::fprintf(stderr, "P51_F_HELLO_REFUSED stage=credentials status=%u\n",
                         static_cast<unsigned>(peer_status));
            std::fflush(stderr);
            return true;
        }
        local::Frame hello;
        const local::Status hello_receive_status =
            connection.receive_with_timeout(hello, kHandshakeMilliseconds);
        if (hello_receive_status != local::Status::Ok) {
            std::fprintf(stderr, "P51_F_HELLO_REFUSED stage=receive status=%u\n",
                         static_cast<unsigned>(hello_receive_status));
            std::fflush(stderr);
            return true;
        }
        const local::Status hello_status = local::validate_handshake(
            hello, local::MessageType::Hello, local::PeerRole::Daemon,
            options.identity);
        if (hello_status != local::Status::Ok) {
            std::fprintf(stderr,
                         "P51_F_HELLO_REFUSED stage=validate status=%u got=%llu/%llu want=%llu/%llu\n",
                         static_cast<unsigned>(hello_status),
                         static_cast<unsigned long long>(hello.identity.generation),
                         static_cast<unsigned long long>(hello.identity.attempt),
                         static_cast<unsigned long long>(options.identity.generation),
                         static_cast<unsigned long long>(options.identity.attempt));
            std::fflush(stderr);
            return true;
        }
        const local::Frame ack =
            local::make_hello_ack(local::PeerRole::Sidecar, options.identity);
        const local::Status ack_status = connection.send(ack);
        if (ack_status != local::Status::Ok) {
            std::fprintf(stderr, "P51_F_HELLO_REFUSED stage=ack-send status=%u\n",
                         static_cast<unsigned>(ack_status));
            std::fflush(stderr);
            return true;
        }

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

        if (operation.kind == local::ControlOperationKind::CacheSession ||
            operation.kind == local::ControlOperationKind::CacheLinkSession) {
            const bool r2_link =
                operation.kind == local::ControlOperationKind::CacheLinkSession;
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
            if (!r2_link && !send_cache_session_ready(adopted.get(), deadline)) {
                std::fprintf(
                    stderr,
                    "P50_CACHE_SESSION_REFUSED stage=ready request=%llu "
                    "errno=%d\n",
                    static_cast<unsigned long long>(operation.request_id), errno);
                std::fflush(stderr);
                return true;
            }
            std::fprintf(
                stderr, r2_link ? "P51_CACHE_LINK_READY request=%llu\n"
                                : "P50_CACHE_SESSION_READY request=%llu\n",
                static_cast<unsigned long long>(operation.request_id));
            std::fflush(stderr);
            if (r2_link)
                runtime.start_adopted_r2_endpoint(adopted.release());
            else
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
        if (operation.kind == local::ControlOperationKind::SourceReservation) {
            if (!operation.p51_reservation.has_value())
                return true;
            const auto operation_deadline =
                operation.p51_reservation->absolute_deadline.as_steady_time_point();
            local::ControlOperation response_operation = operation;
            response_operation.p51_reservation_result =
                runtime.reserve_p51_source_on_owner(*operation.p51_reservation);
            const std::vector<uint8_t> response_payload =
                local::encode_control_operation(response_operation);
            if (response_payload.empty())
                return true;
            const local::Frame response{local::kProtocolVersion,
                                        local::MessageType::Data,
                                        options.identity, response_payload};
            if (connection.send_until(response, operation_deadline) !=
                local::Status::Ok)
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
        }
        if (operation.kind ==
            local::ControlOperationKind::SourceReservationCancel) {
            if (!operation.p51_reservation_cancel.has_value())
                return true;
            const auto& cancel = *operation.p51_reservation_cancel;
            const auto operation_deadline =
                cancel.absolute_deadline.as_steady_time_point();
            local::ControlOperation response_operation = operation;
            const bool cancelled = runtime.cancel_p51_source_on_owner(
                cancel.arm, cancel.armed.reservation_id, operation_deadline);
            response_operation.p51_reservation_cancel_result = cancelled;
            response_operation.p51_reservation_cancel->cancelled = cancelled;
            const std::vector<uint8_t> response_payload =
                local::encode_control_operation(response_operation);
            if (response_payload.empty())
                return true;
            const local::Frame response{local::kProtocolVersion,
                                        local::MessageType::Data,
                                        options.identity, response_payload};
            if (connection.send_until(response, operation_deadline) !=
                local::Status::Ok)
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
        }
        if (operation.kind ==
            local::ControlOperationKind::P51SourceTransfer) {
            if (!operation.p51_source_transfer.has_value())
                return true;
            const auto operation_deadline =
                operation.p51_source_transfer->absolute_deadline
                    .as_steady_time_point();
            local::FdHandoffReceiver receiver;
            const local::FdHandoffResult handoff = receiver.receive_and_ack(
                connection,
                local::HandoffRequest{options.identity, operation.request_id},
                operation_deadline);
            if (handoff.status != local::FdHandoffStatus::Accepted)
                return true;
            local::HandoffFd source = receiver.take_adopted_fd();
            if (!runtime.enqueue_p51_source_transfer(
                    std::move(connection), options.identity,
                    std::move(operation), std::move(source))) {
                std::fprintf(stderr,
                             "P51_SOURCE_TRANSFER_REFUSED stage=queue-full\n");
                std::fflush(stderr);
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

struct SourceSetupTaskSlot {
    explicit SourceSetupTaskSlot(
        std::shared_ptr<std::atomic<size_t>> outstanding_tasks)
        : outstanding(std::move(outstanding_tasks)) {}
    ~SourceSetupTaskSlot() {
        outstanding->fetch_sub(1, std::memory_order_relaxed);
    }
    std::shared_ptr<std::atomic<size_t>> outstanding;
};

struct PendingP51Transfer {
    PendingP51Transfer(local::Connection socket, local::Identity daemon_identity,
                       local::ControlOperation control_operation,
                       local::P51SourceTransferRequest source_request,
                       local::HandoffFd source_fd,
                       std::atomic<size_t>* operation_count)
        : connection(std::move(socket)), identity(daemon_identity),
          operation(std::move(control_operation)), request(std::move(source_request)),
          source(std::move(source_fd)), operation_count(operation_count) {}
    ~PendingP51Transfer() { release_slot(); }
    void release_slot() noexcept {
        if (!slot_released.exchange(true, std::memory_order_acq_rel) &&
            operation_count != nullptr)
            operation_count->fetch_sub(1, std::memory_order_acq_rel);
    }

    local::Connection connection;
    local::Identity identity{};
    local::ControlOperation operation{};
    local::P51SourceTransferRequest request{};
    local::HandoffFd source;
    std::atomic<size_t>* operation_count = nullptr;
    std::atomic<bool> slot_released{false};
};

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
        config.max_pending_p51_source_reservations == 0 ||
        config.max_pending_p51_source_reservations > 4096 ||
        config.max_active_source_transfers == 0 ||
        config.max_active_source_transfers > kMaxControlWorkers ||
        config.max_active_p51_source_transfers == 0 ||
        config.max_active_p51_source_transfers > 4096 ||
        config.max_pending_p51_source_operations == 0 ||
        config.max_pending_p51_source_operations > 4096 ||
        config.max_aggregate_source_raw_bytes == 0 ||
        config.max_aggregate_source_raw_bytes > SIZE_MAX ||
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
    const ZstdSourceTransferResult& transfer, CStoreGuid expected_c_guid,
    bool allow_empty = false) noexcept {
    if (transfer.replacement_required && !transfer.route_local_failure) {
        return source_transfer_error(static_cast<uint16_t>(
            local::SourceTransferErrorCode::RouteReplacementRequired),
            transfer.attempts);
    }
    if (transfer.status != ZstdSourceTransferStatus::Committed ||
        !transfer.committed_input.has_value() ||
        (!allow_empty && transfer.raw_bytes == 0) ||
        transfer.raw_digest == Digest128{} ||
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

std::optional<uint64_t> source_fd_size(int fd, uint64_t limit) noexcept {
    if (fd < 0 || limit > SIZE_MAX)
        return std::nullopt;
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > limit)
        return std::nullopt;
    return static_cast<uint64_t>(info.st_size);
}

std::optional<std::shared_ptr<const std::vector<uint8_t>>> read_source_fd(
    int fd, uint64_t limit, uint64_t reserved_size,
    std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>& stopped) noexcept {
    if (fd < 0 || reserved_size > limit || reserved_size > SIZE_MAX)
        return std::nullopt;
    struct stat before{};
    if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 ||
        static_cast<uint64_t>(before.st_size) != reserved_size)
        return std::nullopt;
    if (stopped.load(std::memory_order_acquire) ||
        std::chrono::steady_clock::now() >= deadline)
        return std::nullopt;
    try {
        auto result = std::make_shared<std::vector<uint8_t>>(
            static_cast<size_t>(reserved_size));
        size_t offset = 0;
        while (offset != result->size()) {
            if (stopped.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline)
                return std::nullopt;
            const size_t amount = std::min<size_t>(64 * 1024,
                                                   result->size() - offset);
            const ssize_t count = ::pread(fd, result->data() + offset,
                                          amount,
                                          static_cast<off_t>(offset));
            if (count > 0) {
                offset += static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            return std::nullopt;
        }
        if (stopped.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        struct stat after{};
        if (::fstat(fd, &after) != 0 || !S_ISREG(after.st_mode) ||
            after.st_size < 0 ||
            static_cast<uint64_t>(after.st_size) != reserved_size)
            return std::nullopt;
        return std::const_pointer_cast<const std::vector<uint8_t>>(result);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

struct SidecarRuntime::P51RawCredit {
    P51RawCredit(SidecarRuntime* runtime, uint64_t reserved_bytes) noexcept
        : owner(runtime), bytes(reserved_bytes) {}
    ~P51RawCredit() {
        if (owner != nullptr)
            owner->release_p51_source_credit(bytes);
    }
    SidecarRuntime* owner = nullptr;
    uint64_t bytes = 0;
};

SidecarRuntime::SidecarRuntime(RuntimeConfig config)
    : config_(validate_runtime_config(std::move(config))),
      input_lifecycle_(
          config_.endpoint_config.owner_limits.max_retained_input_records,
          config_.max_input_lifecycle_replays),
      source_setup_pool_(config_.max_active_source_transfers),
      p51_source_prepare_pool_(std::min<size_t>(
          config_.max_active_source_transfers, 4)),
      source_setup_inflight_(std::make_shared<std::atomic<size_t>>(0)),
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
    config_.endpoint_config.lookup_p51_link_reservation = [this](
        const LinkHello& hello) {
        return lookup_p51_link_reservation_on_owner(hello);
    };
    config_.endpoint_config.consume_p51_job_reservation = [this](
        const LinkHello& link, const JobBind& binding) {
        return consume_p51_job_reservation_on_owner(link, binding);
    };
    config_.endpoint_config.authorize_p51_job_publication = [this](
        const LinkHello& link, const JobBind& binding) {
        return authorize_p51_job_publication_on_owner(link, binding);
    };
    config_.endpoint_config.settle_p51_cancelled_job = [this](
        const LinkHello& link, const JobBind& binding) {
        settle_p51_cancelled_job_on_owner(link, binding);
    };
    config_.endpoint_config.record_p51_job_commit = [this](
        const LinkHello& link, const JobBind& binding,
        const R2TxCommit& commit) {
        return record_p51_job_commit_on_owner(link, binding, commit);
    };
    config_.endpoint_config.acknowledge_p51_receipt = [this](
        const LinkHello& link, const CommitAck& ack) {
        return acknowledge_p51_receipt_on_owner(link, ack);
    };
    config_.endpoint_config.recover_p51_receipts = [this](
        const LinkHello& link, const RecoverBegin& begin,
        std::span<const RecoverWitness> witnesses, const RecoverEnd& end) {
        return recover_p51_receipts_on_owner(link, begin, witnesses, end);
    };
    config_.endpoint_config.settle_p51_interrupted_job = [this](
        const LinkHello& link) {
        return settle_p51_interrupted_job_on_owner(link);
    };
    config_.endpoint_config.p51_source_reservation_terminal = [this](
        const JobBind& binding) {
        return p51_source_reservation_terminal_on_owner(binding);
    };
    config_.endpoint_config.validate_p51_reset = [this](
        const LinkHello& link, const ResetRequest& request) {
        return validate_p51_reset_on_owner(link, request);
    };
    config_.endpoint_config.commit_p51_reset = [this](
        const LinkHello& link, const ResetRequest& request,
        const ResetAck& ack) {
        return commit_p51_reset_on_owner(link, request, ack);
    };
    config_.endpoint_config.confirm_p51_reset = [this](
        const LinkHello& link, const ResetConfirm& confirm) {
        return confirm_p51_reset_on_owner(link, confirm);
    };
    config_.endpoint_config.on_p51_link_terminal = [this](
        const LinkHello& hello) {
        release_p51_link_on_owner(hello);
    };
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
    if (!request.valid() || !config_.sidecar_launch.has_value() ||
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
    uint64_t admission_wait_ns = 0;
    auto wait_started = std::chrono::steady_clock::now();
    const auto operation_started = wait_started;
    if (stop_requested_.load(std::memory_order_acquire) ||
        std::chrono::steady_clock::now() >= transfer_deadline)
        return source_transfer_error(7);
    if (route_replacement_required_.load(std::memory_order_acquire))
        return source_transfer_error(static_cast<uint16_t>(
            local::SourceTransferErrorCode::RouteReplacementRequired));

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

    const RouteEndpointKey endpoint_key{
        arm.selected_f_host, arm.selected_f_cache_port};
    bool address_admitted = false;
    bool incarnation_admitted = false;
    bool credit_admitted = false;
    bool relationship_reservation_owned = false;
    uint64_t reserved_raw_bytes = 0;
    std::optional<SourceIncarnationKey> admitted_incarnation;
    std::optional<SourceIncarnationKey> predecessor_incarnation;
    std::optional<P50RouteRelationship> admitted_relationship;
    auto cleanup = std::unique_ptr<int, std::function<void(int*)>>(
        reinterpret_cast<int*>(1),
        [this, &endpoint_key, &admitted_incarnation,
         &predecessor_incarnation, &reserved_raw_bytes, &credit_admitted,
         &address_admitted, &incarnation_admitted,
         &admitted_relationship, &relationship_reservation_owned](int*) {
            release_source_admission(
                endpoint_key,
                incarnation_admitted ? admitted_incarnation : std::nullopt,
                incarnation_admitted ? predecessor_incarnation : std::nullopt,
                relationship_reservation_owned ? admitted_relationship
                                               : std::nullopt,
                address_admitted, relationship_reservation_owned,
                reserved_raw_bytes, credit_admitted);
        });

    const auto wait_ns_since = [&wait_started, &admission_wait_ns] {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - wait_started).count();
        if (elapsed > 0)
            admission_wait_ns += static_cast<uint64_t>(elapsed);
        wait_started = std::chrono::steady_clock::now();
    };
    wait_started = std::chrono::steady_clock::now();
    if (!acquire_source_address(endpoint_key, transfer_deadline))
        return source_transfer_error(7);
    address_admitted = true;
    wait_ns_since();

    std::optional<RouteStoreIdentity> known_endpoint_identity;
    bool route_fatal = false;
    if (!owner_preflight_source_endpoint(endpoint_key,
                                         known_endpoint_identity,
                                         route_fatal,
                                         transfer_deadline)) {
        if (route_fatal)
            latch_route_replacement();
        return source_transfer_error(route_fatal
            ? static_cast<uint16_t>(
                  local::SourceTransferErrorCode::RouteReplacementRequired)
            : 7);
    }

    const auto source_size = source_fd_size(
        source.get(), config_.endpoint_caps.zstd.max_raw_bytes);
    if (!source_size.has_value())
        return source_transfer_error(3);
    reserved_raw_bytes = *source_size;

    if (known_endpoint_identity.has_value()) {
        admitted_incarnation = SourceIncarnationKey{
            known_endpoint_identity->guid,
            known_endpoint_identity->generation};
        wait_started = std::chrono::steady_clock::now();
        if (!acquire_source_incarnation(*admitted_incarnation,
                                        transfer_deadline))
            return source_transfer_error(7);
        incarnation_admitted = true;
        wait_ns_since();
        const P50RouteRelationship known_relationship{
            config_.c_store_guid, known_endpoint_identity->guid,
            known_endpoint_identity->generation, profile};
        auto inserted = std::make_shared<bool>(false);
        auto accepted = std::make_shared<bool>(false);
        admitted_relationship = known_relationship;
        const bool reserved = owner_round_trip(
            [this, known_relationship, inserted, accepted] {
                if (!route_owner_ || route_replacement_required_.load(
                                         std::memory_order_acquire))
                    return;
                if (route_owner_->owns(known_relationship)) {
                    *accepted = true;
                    return;
                }
                if (route_owner_->owner_count() +
                        pending_route_relationships_.size() >=
                    config_.max_route_relationships)
                    return;
                try {
                    *inserted = pending_route_relationships_
                                    .insert(known_relationship)
                                    .second;
                    *accepted = *inserted;
                } catch (...) {
                }
            },
            transfer_deadline);
        relationship_reservation_owned = *inserted;
        if (!reserved || !*accepted) {
            if (!reserved &&
                (std::chrono::steady_clock::now() >= transfer_deadline ||
                 stop_requested_.load(std::memory_order_acquire)))
                return source_transfer_error(7);
            latch_route_replacement();
            return source_transfer_error(static_cast<uint16_t>(
                local::SourceTransferErrorCode::RouteReplacementRequired));
        }
    }

    wait_started = std::chrono::steady_clock::now();
    if (!acquire_source_credit(reserved_raw_bytes, transfer_deadline))
        return source_transfer_error(7);
    credit_admitted = true;
    wait_ns_since();

    auto source_read_ns = std::make_shared<std::atomic<uint64_t>>(0);

    const PrepareRequestKey route_request{arm.assignment_epoch,
                                          arm.assignment_nonce};

    struct PendingFd {
        int fd = -1;
        ~PendingFd() { if (fd >= 0) ::close(fd); }
    };
    auto setup_cancelled = source_setup_cancelled_;
    auto setup_inflight = source_setup_inflight_;
    const size_t setup_limit = config_.max_active_source_transfers;
    auto open_armed = [arm, source_read_ns, setup_cancelled,
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
                         static_cast<unsigned long long>(
                             source_read_ns->load(std::memory_order_relaxed)),
                         static_cast<long long>(open_arm_timeout.count()),
                         static_cast<unsigned long long>(
                             open_arm_elapsed > 0 ? open_arm_elapsed : 0),
                         static_cast<unsigned long long>(
                             stage_elapsed > 0 ? stage_elapsed : 0));
            std::fflush(stderr);
            return -1;
        };
        try {
            if (setup_cancelled->load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= limit)
                return refused("setup-cancelled");
            std::unique_ptr<MsgChannel> channel(Service::createChannelRetryUntil(
                arm.selected_f_host,
                static_cast<unsigned short>(arm.selected_f_cache_port), limit,
                kSourceConnectAttemptBudget,
                Service::ChannelRetryPolicy::HedgeAfterFirst));
            if (!channel)
                return refused("f-connect");
            if (setup_cancelled->load(std::memory_order_acquire))
                return refused("setup-cancelled");
            if (!protocol_supports_p50_r1_bridge(channel->protocol))
                return refused("f-protocol", channel->protocol);
            if (std::chrono::steady_clock::now() >= limit)
                return refused("f-connect-deadline");
            stage_start = std::chrono::steady_clock::now();
            const P50SourceArmMsg request_message(arm);
            if (setup_cancelled->load(std::memory_order_acquire))
                return refused("setup-cancelled");
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
            if (setup_cancelled->load(std::memory_order_acquire))
                return refused("setup-cancelled");
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

    struct IdentityResolution {
        bool accepted = false;
        std::optional<RouteStoreIdentity> previous;
    };
    auto resolution = std::make_shared<IdentityResolution>();
    const bool resolved = owner_round_trip(
        [this, endpoint_key, relationship, resolution] {
            if (!route_owner_ || route_replacement_required_.load(
                                     std::memory_order_acquire))
                return;
            const auto endpoint_position =
                route_endpoint_identities_.find(endpoint_key);
            if (endpoint_position != route_endpoint_identities_.end()) {
                resolution->previous = endpoint_position->second;
            } else if (!pending_route_endpoint_identities_.contains(endpoint_key)) {
                return;
            }
            const SourceIncarnationKey observed_incarnation{
                relationship.f_store_guid, relationship.f_store_generation};
            if (retired_source_incarnations_.contains(observed_incarnation))
                return;
            resolution->accepted = true;
        },
        transfer_deadline);
    if (!resolved || !resolution->accepted) {
        first.reset();
        return source_transfer_error(static_cast<uint16_t>(
            local::SourceTransferErrorCode::RouteReplacementRequired));
    }

    const SourceIncarnationKey observed_incarnation{
        remote_f_guid, remote_f_generation};
    const bool known_same = known_endpoint_identity.has_value() &&
        known_endpoint_identity->guid == remote_f_guid &&
        known_endpoint_identity->generation == remote_f_generation;
    if (!known_same) {
        // Do not let an endpoint alias parked behind a busy incarnation pin a
        // global execution or raw-byte credit.  No source vector exists yet.
        release_source_credit(reserved_raw_bytes);
        credit_admitted = false;
        std::vector<SourceIncarnationKey> gates{observed_incarnation};
        if (resolution->previous.has_value() &&
            (resolution->previous->guid != remote_f_guid ||
             resolution->previous->generation != remote_f_generation)) {
            predecessor_incarnation = SourceIncarnationKey{
                resolution->previous->guid,
                resolution->previous->generation};
            gates.push_back(*predecessor_incarnation);
        }
        wait_started = std::chrono::steady_clock::now();
        const bool transitioned = incarnation_admitted && admitted_incarnation &&
            predecessor_incarnation &&
            *admitted_incarnation == *predecessor_incarnation
                ? transition_source_incarnation(*admitted_incarnation,
                                                observed_incarnation,
                                                transfer_deadline)
                : acquire_source_incarnations(std::move(gates),
                                              transfer_deadline);
        if (!transitioned) {
            first.reset();
            return source_transfer_error(7);
        }
        admitted_incarnation = observed_incarnation;
        incarnation_admitted = true;
        wait_ns_since();

        if (relationship_reservation_owned && admitted_relationship &&
            *admitted_relationship != relationship) {
            const P50RouteRelationship prior = *admitted_relationship;
            if (!owner_round_trip(
                    [this, prior] {
                        pending_route_relationships_.erase(prior);
                    },
                    transfer_deadline)) {
                if (std::chrono::steady_clock::now() >= transfer_deadline ||
                    stop_requested_.load(std::memory_order_acquire)) {
                    first.reset();
                    return source_transfer_error(7);
                }
                latch_route_replacement();
                first.reset();
                return source_transfer_error(static_cast<uint16_t>(
                    local::SourceTransferErrorCode::RouteReplacementRequired));
            }
            relationship_reservation_owned = false;
        }
        admitted_relationship = relationship;
        auto inserted = std::make_shared<bool>(false);
        auto accepted = std::make_shared<bool>(false);
        const bool relationship_reserved = owner_round_trip(
            [this, relationship, inserted, accepted] {
                if (!route_owner_ || route_replacement_required_.load(
                                         std::memory_order_acquire))
                    return;
                if (route_owner_->owns(relationship)) {
                    *accepted = true;
                    return;
                }
                if (route_owner_->owner_count() +
                        pending_route_relationships_.size() >=
                    config_.max_route_relationships)
                    return;
                try {
                    *inserted =
                        pending_route_relationships_.insert(relationship).second;
                    *accepted = *inserted;
                } catch (...) {
                }
            },
            transfer_deadline);
        relationship_reservation_owned = *inserted;
        if (!relationship_reserved || !*accepted) {
            if (!relationship_reserved &&
                (std::chrono::steady_clock::now() >= transfer_deadline ||
                 stop_requested_.load(std::memory_order_acquire))) {
                first.reset();
                return source_transfer_error(7);
            }
            latch_route_replacement();
            first.reset();
            return source_transfer_error(static_cast<uint16_t>(
                local::SourceTransferErrorCode::RouteReplacementRequired));
        }

        wait_started = std::chrono::steady_clock::now();
        if (!acquire_source_credit(reserved_raw_bytes, transfer_deadline)) {
            first.reset();
            return source_transfer_error(7);
        }
        credit_admitted = true;
        wait_ns_since();
    }

    const auto source_read_start = std::chrono::steady_clock::now();
    const auto source_bytes = read_source_fd(
        source.get(), config_.endpoint_caps.zstd.max_raw_bytes,
        reserved_raw_bytes, transfer_deadline, stop_requested_);
    const auto source_read_elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - source_read_start)
            .count();
    const uint64_t elapsed_read_ns = source_read_elapsed > 0
        ? static_cast<uint64_t>(source_read_elapsed) : 0;
    source_read_ns->store(elapsed_read_ns, std::memory_order_relaxed);
    if (!source_bytes.has_value()) {
        std::fprintf(stderr,
                     "P50_SOURCE_TRANSFER_REFUSED stage=source-read request=%llu "
                     "f_host=%s f_cache_port=%u source_read_ns=%llu\n",
                     static_cast<unsigned long long>(arm.source_request_id),
                     arm.selected_f_host.c_str(), arm.selected_f_cache_port,
                     static_cast<unsigned long long>(elapsed_read_ns));
        std::fflush(stderr);
        return source_transfer_error(3);
    }
    const AsyncConnectedFdFactory connection =
        [this, first, open_armed, setup_cancelled, setup_inflight, setup_limit,
         expected_guid = remote_f_guid,
         expected_generation = remote_f_generation](
            std::chrono::steady_clock::time_point limit,
            std::function<void(int)> completion) mutable {
            if (setup_cancelled->load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= limit) {
                if (first->fd >= 0) {
                    (void)::close(first->fd);
                    first->fd = -1;
                }
                completion(-1);
                return;
            }
            if (first->fd >= 0) {
                const int fd = first->fd;
                first->fd = -1;
                completion(fd);
                return;
            }
            size_t outstanding = setup_inflight->load(
                std::memory_order_relaxed);
            while (outstanding < setup_limit &&
                   !setup_inflight->compare_exchange_weak(
                       outstanding, outstanding + 1,
                       std::memory_order_acq_rel,
                       std::memory_order_relaxed)) {}
            if (outstanding >= setup_limit) {
                completion(-1);
                return;
            }
            std::shared_ptr<SourceSetupTaskSlot> setup_slot;
            try {
                setup_slot = std::make_shared<SourceSetupTaskSlot>(
                    setup_inflight);
            } catch (...) {
                setup_inflight->fetch_sub(1, std::memory_order_relaxed);
                completion(-1);
                return;
            }
            std::shared_ptr<std::function<void(int)>> shared_completion;
            try {
                shared_completion =
                    std::make_shared<std::function<void(int)>>(completion);
                asio::post(source_setup_pool_,
                    [open_armed, setup_cancelled, expected_guid,
                     expected_generation, limit,
                     shared_completion, setup_slot] {
                        if (setup_cancelled->load(std::memory_order_acquire) ||
                            std::chrono::steady_clock::now() >= limit) {
                            (*shared_completion)(-1);
                            return;
                        }
                        FStoreGuid observed_guid;
                        uint64_t observed_generation = 0;
                        int fd = open_armed(limit, observed_guid,
                                            observed_generation);
                        if (fd >= 0 &&
                            (observed_guid != expected_guid ||
                             observed_generation != expected_generation ||
                             std::chrono::steady_clock::now() >= limit)) {
                            (void)::close(fd);
                            fd = -1;
                        }
                        (*shared_completion)(fd);
                    });
            } catch (...) {
                if (shared_completion && *shared_completion)
                    (*shared_completion)(-1);
                else
                    completion(-1);
            }
        };

    auto completion = std::make_shared<std::promise<local::P50SourceTransferResult>>();
    std::future<local::P50SourceTransferResult> result = completion->get_future();
    try {
        asio::co_spawn(
            context_,
            [this, relationship, request, connection, transfer_deadline,
             source_bytes = *source_bytes, completion, route_request,
             expected_c_guid = config_.c_store_guid, admission_wait_ns,
             operation_started]() mutable
                -> asio::awaitable<void> {
                local::P50SourceTransferResult value = source_transfer_error(4);
                ZstdSourceTransferResult observed;
                try {
                    if (stop_requested_.load(std::memory_order_acquire) ||
                        route_replacement_required_.load(
                            std::memory_order_acquire)) {
                        observed.status = ZstdSourceTransferStatus::Unavailable;
                        observed.profile = relationship.profile;
                        observed.replacement_required =
                            route_replacement_required_.load(
                                std::memory_order_acquire);
                    } else {
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
                        pending_route_relationships_.erase(relationship);
                        if (stop_requested_.load(std::memory_order_acquire) ||
                            route_replacement_required_.load(
                                std::memory_order_acquire)) {
                            observed.status =
                                ZstdSourceTransferStatus::Unavailable;
                            observed.profile = relationship.profile;
                            observed.replacement_required =
                                route_replacement_required_.load(
                                    std::memory_order_acquire);
                        } else {
                        observed = co_await route_owner_->transfer(
                            relationship, route_request, connection,
                            transfer_deadline,
                            std::span<const uint8_t>(*source_bytes));
                        }
                    }
                    }
                    if (observed.replacement_required && !observed.route_local_failure)
                        latch_route_replacement();
                    value = source_transfer_result(observed, expected_c_guid);
                } catch (const P29V1CapabilityUnavailable&) {
                    latch_route_replacement();
                    value = source_transfer_error(static_cast<uint16_t>(
                        local::SourceTransferErrorCode::
                            PermanentLocalProfileUnavailable));
                } catch (...) {
                    observed.status = ZstdSourceTransferStatus::TerminalError;
                    observed.profile = relationship.profile;
                    observed.replacement_required = true;
                    latch_route_replacement();
                    value = source_transfer_result(observed, expected_c_guid);
                }
                const auto source_service_elapsed =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - operation_started)
                        .count();
                const uint64_t source_elapsed_ns = source_service_elapsed > 0
                    ? static_cast<uint64_t>(source_service_elapsed) : 0;
                const uint64_t source_service_ns =
                    source_elapsed_ns > admission_wait_ns
                        ? source_elapsed_ns - admission_wait_ns : 0;
                append_source_result_trace(
                    request, expected_c_guid, relationship.profile, observed,
                    admission_wait_ns, source_service_ns);
                completion->set_value(value);
                co_return;
            },
            asio::detached);
    } catch (...) {
        latch_route_replacement();
        return source_transfer_error(static_cast<uint16_t>(
            local::SourceTransferErrorCode::RouteReplacementRequired));
    }
    // The route owner uses the same absolute deadline for connect, arm, and
    // CacheWire.  A bounded grace lets the owner coroutine publish its typed
    // terminal result without allowing a control worker to wait forever.
    auto wait_limit = transfer_deadline + config_.cancellation_grace;
    while (result.wait_for(std::chrono::milliseconds(10)) !=
           std::future_status::ready) {
        const int64_t stop_ns =
            stop_requested_at_ns_.load(std::memory_order_acquire);
        if (stop_ns != 0) {
            const auto stopped_at = std::chrono::steady_clock::time_point(
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::nanoseconds(stop_ns)));
            wait_limit = std::min(wait_limit,
                                  stopped_at + config_.cancellation_grace);
        }
        if (std::chrono::steady_clock::now() < wait_limit)
            continue;
        // The coroutine still owns retained route state and admission leases.
        // Retire the supervised sidecar instead of publishing a successor
        // while its prior owner-affine operation remains live.
        if (config_.fail_stop)
            config_.fail_stop();
        std::_Exit(125);
    }
    return result.get();
}

local::P50SourceTransferResult SidecarRuntime::transfer_p51_source_on_owner(
    local::P51SourceTransferRequest request,
    local::HandoffFd source) noexcept {
    constexpr uint16_t kInvalid = 1;
    constexpr uint16_t kExpired = 7;
    constexpr uint16_t kSourceRead = 3;
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = request.absolute_deadline.as_steady_time_point();
    if (!request.armed.valid() || !request.absolute_deadline.valid() ||
        !request.absolute_deadline.matches_clock(clock) || !source.valid() ||
        !config_.sidecar_launch || !config_.sidecar_launch->valid() ||
        request.armed.arm.source.c_store_guid != config_.c_store_guid.bytes ||
        request.armed.arm.source.c_store_generation !=
            config_.sidecar_launch->store_generation ||
        request.armed.arm.source.c_control_generation !=
            config_.sidecar_launch->identity.generation ||
        request.armed.arm.source.c_control_attempt !=
            config_.sidecar_launch->identity.attempt ||
        request.armed.f_store_guid == config_.f_store_guid.bytes ||
        request.armed.f_store_guid == std::array<uint8_t, 16>{} ||
        request.armed.f_store_generation == 0 ||
        request.armed.selected_revision != 2)
        return source_transfer_error(kInvalid);
    if (stop_requested_.load(std::memory_order_acquire) ||
        route_replacement_required_.load(std::memory_order_acquire) ||
        std::chrono::steady_clock::now() >= deadline)
        return source_transfer_error(kExpired);

    const auto& arm = request.armed.arm.source;
    ProfileId profile;
    switch (arm.cache_profile) {
    case CACHE_PROFILE_P29V1: profile = ProfileId::P29V1; break;
    case CACHE_PROFILE_ZSTD_TU: profile = ProfileId::ZSTD_TU; break;
    case CACHE_PROFILE_ZSTD_ROUTE: profile = ProfileId::ZSTD_ROUTE; break;
    default: return source_transfer_error(kInvalid);
    }
    if (request.armed.selected_window == 0 ||
        request.armed.selected_window > 30 ||
        arm.selected_f_cache_port == 0 || arm.selected_f_cache_port > UINT16_MAX ||
        arm.selected_f_host.empty())
        return source_transfer_error(kInvalid);
    const auto source_size = source_fd_size(
        source.get(), config_.endpoint_caps.zstd.max_raw_bytes);
    if (!source_size.has_value())
        return source_transfer_error(kSourceRead);
    if (!acquire_p51_source_credit(*source_size, deadline))
        return source_transfer_error(kExpired);
    bool credit_owned = true;
    auto credit_guard = std::unique_ptr<int, std::function<void(int*)>>(
        reinterpret_cast<int*>(1), [this, source_size, &credit_owned](int*) {
            if (credit_owned)
                release_p51_source_credit(*source_size);
        });
    const auto raw = read_source_fd(
        source.get(), config_.endpoint_caps.zstd.max_raw_bytes, *source_size,
        deadline, stop_requested_);
    if (!raw.has_value())
        return source_transfer_error(kSourceRead);

    const P50RouteRelationship relationship{
        config_.c_store_guid, FStoreGuid{request.armed.f_store_guid},
        request.armed.f_store_generation, profile};
    const PrepareRequestKey route_request{
        arm.assignment_epoch, arm.assignment_nonce};
    auto setup_cancelled = source_setup_cancelled_;
    auto setup_inflight = source_setup_inflight_;
    const size_t setup_limit = config_.max_active_source_transfers;
    const AsyncConnectedFdFactory connector =
        [this, host = arm.selected_f_host,
         port = static_cast<unsigned short>(arm.selected_f_cache_port),
         setup_cancelled, setup_inflight, setup_limit](
            std::chrono::steady_clock::time_point limit,
            std::function<void(int)> completion) mutable {
            if (setup_cancelled->load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= limit) {
                completion(-1);
                return;
            }
            size_t outstanding = setup_inflight->load(std::memory_order_relaxed);
            while (outstanding < setup_limit &&
                   !setup_inflight->compare_exchange_weak(
                       outstanding, outstanding + 1, std::memory_order_acq_rel,
                       std::memory_order_relaxed)) {}
            if (outstanding >= setup_limit) {
                completion(-1);
                return;
            }
            std::shared_ptr<SourceSetupTaskSlot> slot;
            std::shared_ptr<std::function<void(int)>> done;
            try {
                slot = std::make_shared<SourceSetupTaskSlot>(setup_inflight);
                done = std::make_shared<std::function<void(int)>>(completion);
                asio::post(source_setup_pool_,
                    [host = std::move(host), port, limit, setup_cancelled,
                     done, slot] {
                        int fd = -1;
                        try {
                            if (!setup_cancelled->load(std::memory_order_acquire) &&
                                std::chrono::steady_clock::now() < limit) {
                                std::unique_ptr<MsgChannel> channel(
                                    Service::createChannelRetryUntil(
                                        host, port, limit,
                                        kSourceConnectAttemptBudget,
                                        Service::ChannelRetryPolicy::HedgeAfterFirst));
                                if (channel &&
                                    protocol_supports_cache_r2(channel->protocol) &&
                                    std::chrono::steady_clock::now() < limit &&
                                    channel->send_msg(P51CacheLinkSessionMsg())) {
                                    std::unique_ptr<Msg> reply(
                                        channel->get_msg_until(limit));
                                    if (dynamic_cast<P51CacheLinkSessionMsg*>(
                                            reply.get()) != nullptr)
                                        fd = channel->release_fd_after_p51_link_session_ready(
                                            limit);
                                }
                            }
                        } catch (...) {
                            fd = -1;
                        }
                        (*done)(fd);
                    });
            } catch (...) {
                if (done && *done)
                    (*done)(-1);
                else {
                    if (!slot)
                        setup_inflight->fetch_sub(1, std::memory_order_relaxed);
                    completion(-1);
                }
            }
        };

    auto completion = std::make_shared<
        std::promise<local::P50SourceTransferResult>>();
    auto result = completion->get_future();
    try {
        asio::co_spawn(context_,
            [this, request = std::move(request), relationship, route_request,
             connector, deadline, source_bytes = *raw, completion]() mutable
                -> asio::awaitable<void> {
                local::P50SourceTransferResult value =
                    source_transfer_error(kInvalid);
                ZstdSourceTransferResult observed;
                try {
                    const RouteEndpointKey endpoint_key{
                        request.armed.arm.source.selected_f_host,
                        static_cast<unsigned short>(
                            request.armed.arm.source.selected_f_cache_port)};
                    const RouteStoreIdentity store_identity{
                        relationship.f_store_guid,
                        relationship.f_store_generation};
                    if (!stop_requested_.load(std::memory_order_acquire) &&
                        !route_replacement_required_.load(
                            std::memory_order_acquire) &&
                        bind_route_endpoint_identity(endpoint_key, store_identity)) {
                        observed = co_await route_owner_->transfer_p51(
                            relationship, request.armed, connector, route_request,
                            deadline,
                            std::span<const uint8_t>(*source_bytes));
                    } else {
                        observed.status = ZstdSourceTransferStatus::Unavailable;
                        observed.profile = relationship.profile;
                        observed.replacement_required =
                            route_replacement_required_.load(
                                std::memory_order_acquire);
                    }
                    if (observed.replacement_required &&
                        !observed.route_local_failure)
                        latch_route_replacement();
                    value = source_transfer_result(observed, config_.c_store_guid,
                                                   true);
                } catch (...) {
                    observed.status = ZstdSourceTransferStatus::TerminalError;
                    observed.profile = relationship.profile;
                    observed.replacement_required = true;
                    latch_route_replacement();
                    value = source_transfer_result(observed, config_.c_store_guid,
                                                   true);
                }
                completion->set_value(value);
                co_return;
            }, asio::detached);
    } catch (...) {
        return source_transfer_error(static_cast<uint16_t>(
            local::SourceTransferErrorCode::RouteReplacementRequired));
    }
    const auto wait_limit = deadline + config_.cancellation_grace;
    while (result.wait_for(std::chrono::milliseconds(10)) !=
           std::future_status::ready) {
        if (std::chrono::steady_clock::now() < wait_limit)
            continue;
        if (config_.fail_stop)
            config_.fail_stop();
        std::_Exit(125);
    }
    credit_owned = false;
    release_p51_source_credit(*source_size);
    return result.get();
}

bool SidecarRuntime::enqueue_p51_source_transfer(
    local::Connection&& connection, local::Identity identity,
    local::ControlOperation operation, local::HandoffFd source) noexcept {
    const auto& configured = operation.p51_source_transfer;
    if (!connection.valid() || !source.valid() || !configured.has_value() ||
        operation.kind != local::ControlOperationKind::P51SourceTransfer ||
        operation.identity != identity || operation.request_id == 0 ||
        stop_requested_.load(std::memory_order_acquire))
        return false;
    size_t pending = p51_source_operation_count_.load(std::memory_order_relaxed);
    for (;;) {
        if (pending >= config_.max_pending_p51_source_operations)
            return false;
        if (p51_source_operation_count_.compare_exchange_weak(
                pending, pending + 1, std::memory_order_acq_rel,
                std::memory_order_relaxed))
            break;
    }

    std::shared_ptr<PendingP51Transfer> pending_transfer;
    try {
        const local::P51SourceTransferRequest request_copy = *configured;
        pending_transfer = std::make_shared<PendingP51Transfer>(
            std::move(connection), identity, std::move(operation), request_copy,
            std::move(source), &p51_source_operation_count_);
        asio::post(p51_source_prepare_pool_, [this, pending_transfer] {
            const auto& request = pending_transfer->request;
            const auto deadline = request.absolute_deadline.as_steady_time_point();
            auto release_operation_slot = [this, pending_transfer] {
                pending_transfer->release_slot();
            };
            auto reply = [this, pending_transfer, release_operation_slot](
                             local::P50SourceTransferResult result) mutable {
                try {
                    asio::post(context_,
                        [this, pending_transfer, release_operation_slot,
                         result = std::move(result)]() mutable {
                            start_p51_transfer_reply(
                                std::move(pending_transfer->connection),
                                pending_transfer->identity,
                                std::move(pending_transfer->operation),
                                std::move(result), release_operation_slot);
                        });
                } catch (...) {
                    release_operation_slot();
                }
            };

            constexpr uint16_t kInvalid = 1;
            constexpr uint16_t kExpired = 7;
            constexpr uint16_t kSourceRead = 3;
            try {
            const auto clock = sidecar::process_monotonic_clock_identity();
            const auto& armed = request.armed;
            if (!armed.valid() || !request.absolute_deadline.valid() ||
                !request.absolute_deadline.matches_clock(clock) ||
                !pending_transfer->source.valid() || !config_.sidecar_launch ||
                !config_.sidecar_launch->valid() ||
                armed.arm.source.c_store_guid != config_.c_store_guid.bytes ||
                armed.arm.source.c_store_generation !=
                    config_.sidecar_launch->store_generation ||
                armed.arm.source.c_control_generation !=
                    config_.sidecar_launch->identity.generation ||
                armed.arm.source.c_control_attempt !=
                    config_.sidecar_launch->identity.attempt ||
                armed.f_store_guid == config_.f_store_guid.bytes ||
                armed.f_store_guid == std::array<uint8_t, 16>{} ||
                armed.f_store_generation == 0 || armed.selected_revision != 2) {
                reply(source_transfer_error(kInvalid));
                return;
            }
            if (stop_requested_.load(std::memory_order_acquire) ||
                route_replacement_required_.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
                reply(source_transfer_error(kExpired));
                return;
            }

            ProfileId profile;
            switch (armed.arm.source.cache_profile) {
            case CACHE_PROFILE_P29V1: profile = ProfileId::P29V1; break;
            case CACHE_PROFILE_ZSTD_TU: profile = ProfileId::ZSTD_TU; break;
            case CACHE_PROFILE_ZSTD_ROUTE: profile = ProfileId::ZSTD_ROUTE; break;
            default:
                reply(source_transfer_error(kInvalid));
                return;
            }
            if (armed.selected_window == 0 || armed.selected_window > 30 ||
                armed.arm.source.selected_f_cache_port == 0 ||
                armed.arm.source.selected_f_cache_port > UINT16_MAX ||
                armed.arm.source.selected_f_host.empty()) {
                reply(source_transfer_error(kInvalid));
                return;
            }

            const auto source_size = source_fd_size(
                pending_transfer->source.get(), config_.endpoint_caps.zstd.max_raw_bytes);
            if (!source_size.has_value()) {
                reply(source_transfer_error(kSourceRead));
                return;
            }
            if (!acquire_p51_source_credit(*source_size, deadline)) {
                reply(source_transfer_error(kExpired));
                return;
            }
            const auto raw = read_source_fd(
                pending_transfer->source.get(), config_.endpoint_caps.zstd.max_raw_bytes,
                *source_size, deadline, stop_requested_);
            if (!raw.has_value()) {
                release_p51_source_credit(*source_size);
                reply(source_transfer_error(kSourceRead));
                return;
            }
            std::shared_ptr<P51RawCredit> raw_credit;
            try {
                raw_credit = std::make_shared<P51RawCredit>(this, *source_size);
            } catch (...) {
                release_p51_source_credit(*source_size);
                reply(source_transfer_error(kSourceRead));
                return;
            }

            const P50RouteRelationship relationship{
                config_.c_store_guid, FStoreGuid{armed.f_store_guid},
                armed.f_store_generation, profile};
            const PrepareRequestKey route_request{
                armed.arm.source.assignment_epoch,
                armed.arm.source.assignment_nonce};
            auto setup_cancelled = source_setup_cancelled_;
            auto setup_inflight = source_setup_inflight_;
            const size_t setup_limit = config_.max_active_source_transfers;
            const AsyncConnectedFdFactory connector =
                [this, host = armed.arm.source.selected_f_host,
                 port = static_cast<unsigned short>(
                     armed.arm.source.selected_f_cache_port),
                 setup_cancelled, setup_inflight, setup_limit](
                    std::chrono::steady_clock::time_point limit,
                    std::function<void(int)> completion) mutable {
                    if (setup_cancelled->load(std::memory_order_acquire) ||
                        std::chrono::steady_clock::now() >= limit) {
                        completion(-1);
                        return;
                    }
                    size_t outstanding = setup_inflight->load(
                        std::memory_order_relaxed);
                    while (outstanding < setup_limit &&
                           !setup_inflight->compare_exchange_weak(
                               outstanding, outstanding + 1,
                               std::memory_order_acq_rel,
                               std::memory_order_relaxed)) {}
                    if (outstanding >= setup_limit) {
                        completion(-1);
                        return;
                    }
                    std::shared_ptr<SourceSetupTaskSlot> slot;
                    std::shared_ptr<std::function<void(int)>> done;
                    try {
                        slot = std::make_shared<SourceSetupTaskSlot>(setup_inflight);
                        done = std::make_shared<std::function<void(int)>>(completion);
                        asio::post(source_setup_pool_,
                            [host = std::move(host), port, limit, setup_cancelled,
                             done, slot] {
                                int fd = -1;
                                try {
                                    if (!setup_cancelled->load(
                                            std::memory_order_acquire) &&
                                        std::chrono::steady_clock::now() < limit) {
                                        std::unique_ptr<MsgChannel> channel(
                                            Service::createChannelRetryUntil(
                                                host, port, limit,
                                                kSourceConnectAttemptBudget,
                                                Service::ChannelRetryPolicy::
                                                    HedgeAfterFirst,
                                                setup_cancelled.get()));
                                        if (channel && protocol_supports_cache_r2(
                                                channel->protocol) &&
                                            !setup_cancelled->load(
                                                std::memory_order_acquire) &&
                                            std::chrono::steady_clock::now() < limit &&
                                            channel->send_msg(
                                                P51CacheLinkSessionMsg())) {
                                            std::unique_ptr<Msg> response(
                                                channel->get_msg_until(
                                                    limit, false,
                                                    setup_cancelled.get()));
                                            if (!setup_cancelled->load(
                                                    std::memory_order_acquire) &&
                                                dynamic_cast<
                                                    P51CacheLinkSessionMsg*>(
                                                        response.get()) != nullptr)
                                                fd = channel->
                                                    release_fd_after_p51_link_session_ready(
                                                        limit);
                                        }
                                    }
                                } catch (...) {
                                    fd = -1;
                                }
                                (*done)(fd);
                            });
                    } catch (...) {
                        if (done && *done)
                            (*done)(-1);
                        else {
                            if (!slot)
                                setup_inflight->fetch_sub(
                                    1, std::memory_order_relaxed);
                            completion(-1);
                        }
                    }
                };

            try {
                asio::co_spawn(context_,
                    [this, pending_transfer, relationship, route_request,
                     connector, deadline, raw = *raw, raw_credit,
                     reply]() mutable
                        -> asio::awaitable<void> {
                        (void)raw_credit; // lifetime owns aggregate raw-byte credit
                        local::P50SourceTransferResult result =
                            source_transfer_error(kInvalid);
                        ZstdSourceTransferResult observed;
                        try {
                            const RouteEndpointKey endpoint_key{
                                pending_transfer->request.armed.arm.source.
                                    selected_f_host,
                                static_cast<unsigned short>(
                                    pending_transfer->request.armed.arm.source.
                                        selected_f_cache_port)};
                            const RouteStoreIdentity store_identity{
                                relationship.f_store_guid,
                                relationship.f_store_generation};
                            if (!stop_requested_.load(
                                    std::memory_order_acquire) &&
                                !route_replacement_required_.load(
                                    std::memory_order_acquire) &&
                                bind_route_endpoint_identity(
                                    endpoint_key, store_identity)) {
                                observed = co_await route_owner_->transfer_p51(
                                    relationship,
                                    pending_transfer->request.armed,
                                    connector, route_request, deadline,
                                    std::span<const uint8_t>(*raw));
                            } else {
                                observed.status =
                                    ZstdSourceTransferStatus::Unavailable;
                                observed.profile = relationship.profile;
                                observed.replacement_required =
                                    route_replacement_required_.load(
                                        std::memory_order_acquire);
                            }
                            if (observed.replacement_required &&
                                !observed.route_local_failure)
                                latch_route_replacement();
                            result = source_transfer_result(
                                observed, config_.c_store_guid, true);
                        } catch (...) {
                            observed.status =
                                ZstdSourceTransferStatus::TerminalError;
                            observed.profile = relationship.profile;
                            observed.replacement_required = true;
                            latch_route_replacement();
                            result = source_transfer_result(
                                observed, config_.c_store_guid, true);
                        }
                        reply(std::move(result));
                        co_return;
                    }, asio::detached);
            } catch (...) {
                reply(source_transfer_error(static_cast<uint16_t>(
                    local::SourceTransferErrorCode::RouteReplacementRequired)));
            }
            } catch (...) {
                reply(source_transfer_error(kSourceRead));
            }
        });
        return true;
    } catch (...) {
        if (pending_transfer)
            pending_transfer->release_slot();
        else
            p51_source_operation_count_.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
}

bool SidecarRuntime::bind_route_endpoint_identity(
    const RouteEndpointKey& endpoint,
    RouteStoreIdentity observed) noexcept {
    if (!route_owner_ || endpoint.host.empty() || endpoint.cache_port == 0 ||
        observed.guid == FStoreGuid{} || observed.generation == 0)
        return false;
    const auto position = route_endpoint_identities_.find(endpoint);
    if (position == route_endpoint_identities_.end()) {
        if (retired_source_incarnations_.contains(
                SourceIncarnationKey{observed.guid, observed.generation}))
            return false;
        const bool reserved = pending_route_endpoint_identities_.contains(endpoint);
        if (!reserved && route_endpoint_identities_.size() +
                                 pending_route_endpoint_identities_.size() >=
                             config_.max_route_endpoint_identities)
            return false;
        try {
            const bool inserted =
                route_endpoint_identities_.emplace(endpoint, observed).second;
            if (inserted && reserved)
                pending_route_endpoint_identities_.erase(endpoint);
            return inserted;
        } catch (...) {
            return false;
        }
    }
    if (position->second == observed)
        return true;

    // The map still names the predecessor until every one of its profile
    // owners is retired.  Passing the newly observed GUID here would leak old
    // history into the successor incarnation.
    const SourceIncarnationKey retired{
        position->second.guid, position->second.generation};
    if (retired_source_incarnations_.size() >=
            config_.max_route_endpoint_identities &&
        !retired_source_incarnations_.contains(retired)) {
        latch_route_replacement();
        return false;
    }
    const SourceIncarnationKey successor{observed.guid, observed.generation};
    if (retired_source_incarnations_.contains(successor))
        return false;
    if (!route_owner_->reset_f_store_exact(position->second.guid,
                                           position->second.generation))
        return false;
    try {
        retired_source_incarnations_.insert(retired);
    } catch (...) {
        latch_route_replacement();
        return false;
    }
    position->second = observed;
    return true;
}

bool SidecarRuntime::owner_round_trip(
    std::function<void()> operation,
    std::chrono::steady_clock::time_point deadline,
    bool allow_during_stop) noexcept {
    if (!operation ||
        (!allow_during_stop && stop_requested_.load(std::memory_order_acquire)) ||
        std::chrono::steady_clock::now() >= deadline)
        return false;
    auto completion = std::make_shared<std::promise<bool>>();
    std::future<bool> result = completion->get_future();
    auto queued_state = std::make_shared<std::atomic<uint8_t>>(0);
    try {
        asio::post(context_, [this, operation = std::move(operation), completion,
                              queued_state, allow_during_stop, deadline]() mutable {
            uint8_t expected = 0;
            if (!queued_state->compare_exchange_strong(
                    expected, 2, std::memory_order_acq_rel)) {
                try { completion->set_value(false); } catch (...) {}
                return;
            }
            bool executed = false;
            if ((allow_during_stop ||
                 !stop_requested_.load(std::memory_order_acquire)) &&
                std::chrono::steady_clock::now() < deadline) {
                try {
                    operation();
                    executed = true;
                } catch (...) {
                }
            }
            try { completion->set_value(executed); } catch (...) {}
        });
    } catch (...) {
        return false;
    }
    if (result.wait_until(deadline) == std::future_status::ready)
        return result.get() &&
               (allow_during_stop ||
                !stop_requested_.load(std::memory_order_acquire));
    uint8_t expected = 0;
    if (queued_state->compare_exchange_strong(expected, 1,
                                               std::memory_order_acq_rel))
        return false;
    const auto settle_until = std::chrono::steady_clock::now() +
                              config_.cancellation_grace;
    if (result.wait_until(settle_until) != std::future_status::ready) {
        if (config_.fail_stop)
            config_.fail_stop();
        std::_Exit(125);
    }
    return result.get();
}

bool SidecarRuntime::owner_preflight_source_endpoint(
    const RouteEndpointKey& endpoint,
    std::optional<RouteStoreIdentity>& known_identity,
    bool& route_fatal,
    std::chrono::steady_clock::time_point deadline) noexcept {
    auto result = std::make_shared<std::optional<RouteStoreIdentity>>();
    auto accepted = std::make_shared<bool>(false);
    auto fatal = std::make_shared<bool>(false);
    const bool completed = owner_round_trip(
        [this, endpoint, result, accepted, fatal] {
            if (!route_owner_ || route_replacement_required_.load(
                                     std::memory_order_acquire)) {
                *fatal = !route_owner_ || route_replacement_required_.load(
                                              std::memory_order_acquire);
                return;
            }
            const auto position = route_endpoint_identities_.find(endpoint);
            if (position == route_endpoint_identities_.end()) {
                if (route_endpoint_identities_.size() +
                        pending_route_endpoint_identities_.size() >=
                    config_.max_route_endpoint_identities) {
                    *fatal = true;
                    return;
                }
                try {
                    if (!pending_route_endpoint_identities_.insert(endpoint).second)
                        return;
                } catch (...) {
                    *fatal = true;
                    return;
                }
                *accepted = true;
                return;
            }
            *result = position->second;
            const SourceIncarnationKey incarnation{
                position->second.guid, position->second.generation};
            if (retired_source_incarnations_.contains(incarnation)) {
                *fatal = true;
                return;
            }
            *accepted = true;
        },
        deadline);
    if (!completed || !*accepted)
    {
        route_fatal = *fatal;
        return false;
    }
    known_identity = *result;
    route_fatal = false;
    return true;
}

bool SidecarRuntime::acquire_source_address(
    const RouteEndpointKey& endpoint,
    std::chrono::steady_clock::time_point deadline) noexcept {
    try {
        std::unique_lock lock(source_admission_mutex_);
        const bool ready = source_admission_changed_.wait_until(
            lock, deadline, [this, &endpoint] {
                return stop_requested_.load(std::memory_order_acquire) ||
                       route_replacement_required_.load(
                           std::memory_order_acquire) ||
                       !active_source_addresses_.contains(endpoint);
            });
        if (!ready || std::chrono::steady_clock::now() >= deadline ||
            stop_requested_.load(std::memory_order_acquire) ||
            route_replacement_required_.load(std::memory_order_acquire))
            return false;
        return active_source_addresses_.insert(endpoint).second;
    } catch (...) {
        return false;
    }
}

bool SidecarRuntime::acquire_source_incarnation(
    const SourceIncarnationKey& incarnation,
    std::chrono::steady_clock::time_point deadline) noexcept {
    return acquire_source_incarnations({incarnation}, deadline);
}

bool SidecarRuntime::acquire_source_incarnations(
    std::vector<SourceIncarnationKey> incarnations,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (incarnations.empty())
        return false;
    try {
        std::sort(incarnations.begin(), incarnations.end());
        incarnations.erase(std::unique(incarnations.begin(), incarnations.end()),
                           incarnations.end());
        std::unique_lock lock(source_admission_mutex_);
        const bool ready = source_admission_changed_.wait_until(
            lock, deadline, [this, &incarnations] {
                if (stop_requested_.load(std::memory_order_acquire))
                    return true;
                if (route_replacement_required_.load(
                        std::memory_order_acquire))
                    return true;
                return std::none_of(incarnations.begin(), incarnations.end(),
                                    [this](const auto& key) {
                                        return active_source_incarnations_.contains(key) ||
                                               retiring_source_incarnations_.contains(key);
                                    });
            });
        if (!ready || std::chrono::steady_clock::now() >= deadline ||
            stop_requested_.load(std::memory_order_acquire) ||
            route_replacement_required_.load(std::memory_order_acquire))
            return false;
        std::vector<SourceIncarnationKey> inserted;
        try {
            inserted.reserve(incarnations.size());
            for (const auto& key : incarnations) {
                active_source_incarnations_.insert(key);
                inserted.push_back(key);
            }
        } catch (...) {
            for (const auto& key : inserted)
                active_source_incarnations_.erase(key);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool SidecarRuntime::transition_source_incarnation(
    const SourceIncarnationKey& predecessor,
    const SourceIncarnationKey& successor,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (predecessor == successor)
        return true;
    try {
        std::unique_lock lock(source_admission_mutex_);
        auto predecessor_node =
            active_source_incarnations_.extract(predecessor);
        if (predecessor_node.empty())
            return false;
        try {
            retiring_source_incarnations_.insert(std::move(predecessor_node));
        } catch (...) {
            active_source_incarnations_.insert(std::move(predecessor_node));
            return false;
        }
        source_admission_changed_.notify_all();
        const bool ready = source_admission_changed_.wait_until(
            lock, deadline, [this, &predecessor, &successor] {
                return stop_requested_.load(std::memory_order_acquire) ||
                       route_replacement_required_.load(
                           std::memory_order_acquire) ||
                       (!active_source_incarnations_.contains(predecessor) &&
                        !active_source_incarnations_.contains(successor) &&
                        !retiring_source_incarnations_.contains(successor));
            });
        if (!ready || std::chrono::steady_clock::now() >= deadline ||
            stop_requested_.load(std::memory_order_acquire) ||
            route_replacement_required_.load(std::memory_order_acquire)) {
            active_source_incarnations_.insert(
                retiring_source_incarnations_.extract(predecessor));
            lock.unlock();
            source_admission_changed_.notify_all();
            return false;
        }
        try {
            active_source_incarnations_.insert(
                retiring_source_incarnations_.extract(predecessor));
            active_source_incarnations_.insert(successor);
        } catch (...) {
            active_source_incarnations_.erase(successor);
            retiring_source_incarnations_.erase(predecessor);
            lock.unlock();
            source_admission_changed_.notify_all();
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool SidecarRuntime::acquire_source_credit(
    uint64_t raw_bytes,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (raw_bytes > config_.max_aggregate_source_raw_bytes)
        return false;
    try {
        std::unique_lock lock(source_admission_mutex_);
        const bool ready = source_admission_changed_.wait_until(
            lock, deadline, [this, raw_bytes] {
                return stop_requested_.load(std::memory_order_acquire) ||
                       route_replacement_required_.load(
                           std::memory_order_acquire) ||
                       (active_source_count_ <
                            config_.max_active_source_transfers &&
                        raw_bytes <= config_.max_aggregate_source_raw_bytes -
                                         active_source_raw_bytes_);
            });
        if (!ready || std::chrono::steady_clock::now() >= deadline ||
            stop_requested_.load(std::memory_order_acquire) ||
            route_replacement_required_.load(std::memory_order_acquire))
            return false;
        ++active_source_count_;
        active_source_raw_bytes_ += raw_bytes;
        return true;
    } catch (...) {
        return false;
    }
}

bool SidecarRuntime::acquire_p51_source_credit(
    uint64_t raw_bytes,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (raw_bytes > config_.max_aggregate_source_raw_bytes)
        return false;
    try {
        std::unique_lock lock(source_admission_mutex_);
        const bool ready = source_admission_changed_.wait_until(
            lock, deadline, [this, raw_bytes] {
                return stop_requested_.load(std::memory_order_acquire) ||
                       route_replacement_required_.load(
                           std::memory_order_acquire) ||
                       (active_p51_source_count_ <
                            config_.max_active_p51_source_transfers &&
                        raw_bytes <= config_.max_aggregate_source_raw_bytes -
                                         active_source_raw_bytes_);
            });
        if (!ready || std::chrono::steady_clock::now() >= deadline ||
            stop_requested_.load(std::memory_order_acquire) ||
            route_replacement_required_.load(std::memory_order_acquire))
            return false;
        ++active_p51_source_count_;
        active_source_raw_bytes_ += raw_bytes;
        return true;
    } catch (...) {
        return false;
    }
}

void SidecarRuntime::release_p51_source_credit(uint64_t raw_bytes) noexcept {
    {
        std::lock_guard lock(source_admission_mutex_);
        if (active_p51_source_count_ == 0 ||
            raw_bytes > active_source_raw_bytes_) {
            if (config_.fail_stop)
                config_.fail_stop();
            std::_Exit(125);
        }
        --active_p51_source_count_;
        active_source_raw_bytes_ -= raw_bytes;
    }
    source_admission_changed_.notify_all();
}

void SidecarRuntime::release_source_credit(uint64_t raw_bytes) noexcept {
    {
        std::lock_guard lock(source_admission_mutex_);
        if (active_source_count_ == 0 || raw_bytes > active_source_raw_bytes_) {
            if (config_.fail_stop)
                config_.fail_stop();
            std::_Exit(125);
        }
        --active_source_count_;
        active_source_raw_bytes_ -= raw_bytes;
    }
    source_admission_changed_.notify_all();
}

void SidecarRuntime::release_source_admission(
    const RouteEndpointKey& endpoint,
    const std::optional<SourceIncarnationKey>& incarnation,
    const std::optional<SourceIncarnationKey>& predecessor,
    const std::optional<P50RouteRelationship>& relationship,
    bool address_admitted, bool relationship_reserved,
    uint64_t raw_bytes, bool has_credit) noexcept {
    if (!address_admitted && !incarnation && !predecessor &&
        !relationship_reserved && !has_credit)
        return;
    try {
        asio::post(context_, [this, endpoint, incarnation, predecessor,
                              relationship, address_admitted,
                              relationship_reserved, raw_bytes, has_credit] {
            // The address/incarnation gates remain held until these owner map
            // reservations are cleared, so a successor cannot erase or reuse
            // the predecessor's capacity token.
            if (address_admitted)
                pending_route_endpoint_identities_.erase(endpoint);
            if (relationship_reserved && relationship)
                pending_route_relationships_.erase(*relationship);
            {
                std::lock_guard lock(source_admission_mutex_);
                if (address_admitted)
                    active_source_addresses_.erase(endpoint);
                if (incarnation)
                    active_source_incarnations_.erase(*incarnation);
                if (predecessor)
                    active_source_incarnations_.erase(*predecessor);
                if (predecessor)
                    retiring_source_incarnations_.erase(*predecessor);
                if (has_credit) {
                    if (active_source_count_ == 0 ||
                        raw_bytes > active_source_raw_bytes_) {
                        if (config_.fail_stop)
                            config_.fail_stop();
                        std::_Exit(125);
                    }
                    --active_source_count_;
                    active_source_raw_bytes_ -= raw_bytes;
                }
            }
            source_admission_changed_.notify_all();
        });
    } catch (...) {
        // The owner cannot process a deferred release. Fail closed before
        // waking waiters; no later operation may consume retained route state.
        latch_route_replacement();
        {
            std::lock_guard lock(source_admission_mutex_);
            if (address_admitted)
                active_source_addresses_.erase(endpoint);
            if (incarnation)
                active_source_incarnations_.erase(*incarnation);
            if (predecessor)
                active_source_incarnations_.erase(*predecessor);
            if (has_credit && active_source_count_ != 0 &&
                raw_bytes <= active_source_raw_bytes_) {
                --active_source_count_;
                active_source_raw_bytes_ -= raw_bytes;
            }
        }
        source_admission_changed_.notify_all();
    }
}

void SidecarRuntime::latch_route_replacement() noexcept {
    route_replacement_required_.store(true, std::memory_order_release);
    source_setup_cancelled_->store(true, std::memory_order_release);
    source_admission_changed_.notify_all();
}

SidecarRuntime::~SidecarRuntime() {
    stop();
    // A source preparation task may be awaiting admission or holding a raw
    // reservation. stop() wakes those waits; join preparation before resolver
    // setup so no new setup task can be posted after that pool is drained.
    p51_source_prepare_pool_.join();
    const auto setup_settle_until =
        std::chrono::steady_clock::now() + config_.cancellation_grace;
    while (source_setup_inflight_->load(std::memory_order_acquire) != 0 &&
           std::chrono::steady_clock::now() < setup_settle_until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (source_setup_inflight_->load(std::memory_order_acquire) != 0) {
        // A blocking system resolver cannot be cancelled portably. Do not
        // block destruction indefinitely or let a successor reuse this
        // sidecar's retained state while the task remains live.
        if (config_.fail_stop)
            config_.fail_stop();
        std::_Exit(125);
    }
    source_setup_pool_.join();
    const auto transfer_settle_until =
        std::chrono::steady_clock::now() + config_.cancellation_grace;
    while (p51_source_operation_count_.load(std::memory_order_acquire) != 0 &&
           std::chrono::steady_clock::now() < transfer_settle_until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (p51_source_operation_count_.load(std::memory_order_acquire) != 0) {
        if (config_.fail_stop)
            config_.fail_stop();
        std::_Exit(125);
    }
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
    std::promise<EndpointOwnerResult> completion, int completion_wake_fd,
    bool r2_link) {
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
        ServerRunResult endpoint_result;
        if (r2_link) {
            endpoint_result = co_await endpoint_->run_adopted_r2(
                std::move(*socket), std::move(endpoint_control));
        } else {
            endpoint_result = co_await endpoint_->run_adopted(
                std::move(*socket), std::move(endpoint_control));
        }
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

local::P51SourceReservationResult SidecarRuntime::reserve_p51_source_on_owner(
    local::P51SourceReservationRequest request) noexcept {
    local::P51SourceReservationResult result;
    constexpr uint16_t kInvalid = 0x5101;
    constexpr uint16_t kExpired = 0x5102;
    constexpr uint16_t kCapacity = 0x5103;
    constexpr uint16_t kEntropy = 0x5104;
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = request.absolute_deadline.as_steady_time_point();
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    if (!request.arm.valid() || !request.absolute_deadline.valid() ||
        !request.absolute_deadline.matches_clock(clock) ||
        request.arm.source.c_store_guid == config_.f_store_guid.bytes ||
        !config_.sidecar_launch.has_value() ||
        !config_.sidecar_launch->valid() || config_.f_store_generation == 0 ||
        config_.sidecar_launch->f_store_guid != config_.f_store_guid ||
        config_.sidecar_launch->store_generation != config_.f_store_generation) {
        result.error_code = kInvalid;
        return result;
    }
    if (request.absolute_deadline.expired(
            now_ns, clock.clock_domain_id, clock.time_namespace_id) ||
        stop_requested_.load(std::memory_order_acquire)) {
        result.error_code = kExpired;
        return result;
    }

    const bool completed = owner_round_trip(
        [this, &request, &result, kCapacity, kEntropy, deadline] {
            if (stop_requested_.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline)
                return;
            sweep_p51_reservations_on_owner();
            const CStoreGuid c_guid{request.arm.source.c_store_guid};
            const auto selected_profile = profile_from_cache_profile_mask(
                request.arm.source.cache_profile);
            if (!selected_profile) {
                result.error_code = kInvalid;
                return;
            }
            const ProfileId profile = *selected_profile;
            for (const auto& [id, row] : p51_source_reservations_) {
                (void)id;
                if (row.armed.arm.source.c_store_guid == c_guid.bytes &&
                    row.armed.arm.source.source_request_id ==
                        request.arm.source.source_request_id) {
                    if (row.armed.arm != request.arm) {
                        result.error_code = kInvalid;
                        return;
                    }
                    result.armed = row.armed;
                    return;
                }
            }
            if (p51_source_reservations_.size() >=
                config_.max_pending_p51_source_reservations) {
                result.error_code = kCapacity;
                return;
            }
            auto relationship = p51_source_relationships_.find(c_guid);
            bool created_relationship = false;
            P51SourceRelationship staged_relationship;
            if (relationship != p51_source_relationships_.end()) {
                const auto& prior = relationship->second;
                if (prior.c_store_generation !=
                        request.arm.source.c_store_generation ||
                    prior.c_control_generation !=
                        request.arm.source.c_control_generation ||
                    prior.c_control_attempt !=
                        request.arm.source.c_control_attempt) {
                    result.error_code = kInvalid;
                    return;
                }
                // Profile is fixed for the relationship's physical link. A
                // drained-link retirement/reset API must explicitly replace
                // this row before a different profile can be armed.
                if (relationship->second.profile != profile ||
                    request.arm.requested_window <
                        relationship->second.selected_window) {
                    result.error_code = kCapacity;
                    return;
                }
            } else {
                if (p51_source_relationships_.size() >=
                    config_.max_route_relationships) {
                    for (auto it = p51_source_relationships_.begin();
                         it != p51_source_relationships_.end();) {
                        if (it->second.outstanding == 0 &&
                            !it->second.link_active && !it->second.has_receipts)
                            it = p51_source_relationships_.erase(it);
                        else
                            ++it;
                        if (p51_source_relationships_.size() <
                            config_.max_route_relationships)
                            break;
                    }
                }
                if (p51_source_relationships_.size() >=
                        config_.max_route_relationships ||
                    next_p51_relationship_epoch_ == 0 ||
                    next_p51_relationship_epoch_ == UINT64_MAX) {
                    result.error_code = kCapacity;
                    return;
                }
                ClaimAttemptCapability128 id_one;
                ClaimAttemptCapability128 id_two;
                if (!fresh_claim_attempt_capabilities(id_one, id_two)) {
                    result.error_code = kEntropy;
                    return;
                }
                staged_relationship.profile = profile;
                staged_relationship.logical_id = id_one.bytes;
                staged_relationship.c_store_generation =
                    request.arm.source.c_store_generation;
                staged_relationship.c_control_generation =
                    request.arm.source.c_control_generation;
                staged_relationship.c_control_attempt =
                    request.arm.source.c_control_attempt;
                staged_relationship.epoch = next_p51_relationship_epoch_;
                staged_relationship.selected_window =
                    std::min<uint32_t>(request.arm.requested_window, 30u);
                created_relationship = true;
            }

            ClaimAttemptCapability128 reservation_id;
            ClaimAttemptCapability128 unused_id;
            ClaimAttemptCapability128 capability_1;
            ClaimAttemptCapability128 capability_2;
            if (!fresh_claim_attempt_capabilities(reservation_id, unused_id) ||
                !fresh_claim_attempt_capabilities(capability_1, capability_2) ||
                reservation_id.bytes == unused_id.bytes ||
                capability_1 == capability_2) {
                result.error_code = kEntropy;
                return;
            }

            P51SourceRelationship& selected_relationship =
                created_relationship ? staged_relationship : relationship->second;
            P51SourceArmedFields armed;
            armed.arm = request.arm;
            armed.f_control_generation = config_.sidecar_launch->identity.generation;
            armed.f_control_attempt = config_.sidecar_launch->identity.attempt;
            armed.f_store_generation = config_.f_store_generation;
            armed.f_store_guid = config_.f_store_guid.bytes;
            armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
            if (next_p51_arm_observation_ == 0 ||
                next_p51_arm_observation_ == UINT64_MAX) {
                result.error_code = kCapacity;
                return;
            }
            armed.arm_observation_id = next_p51_arm_observation_++;
            const auto now = std::chrono::steady_clock::now();
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
            if (remaining <= 0) {
                result.error_code = kExpired;
                return;
            }
            armed.source_budget_msec = static_cast<uint32_t>(std::min<int64_t>(
                P50SourceArmedFields::MaxSourceBudgetMsec, remaining));
            armed.attempt_capability_1 = capability_1;
            armed.attempt_capability_2 = capability_2;
            armed.reservation_id = reservation_id.bytes;
            armed.logical_relationship_id = selected_relationship.logical_id;
            armed.relationship_epoch = selected_relationship.epoch;
            armed.selected_revision = CACHE_WIRE_REVISION_R2;
            armed.selected_window = selected_relationship.selected_window;
            if (!armed.valid()) {
                result.error_code = kInvalid;
                return;
            }
            if (created_relationship) {
                const auto inserted = p51_source_relationships_.emplace(
                    c_guid, staged_relationship);
                if (!inserted.second) {
                    result.error_code = kCapacity;
                    return;
                }
                ++next_p51_relationship_epoch_;
            }
            try {
                const auto inserted = p51_source_reservations_.emplace(
                    armed.reservation_id,
                    P51SourceReservationRow{
                        armed, request.absolute_deadline, false, 0, 0,
                        false, false, std::nullopt});
                if (!inserted.second) {
                    if (created_relationship)
                        p51_source_relationships_.erase(c_guid);
                    result.error_code = kEntropy;
                    return;
                }
            } catch (...) {
                if (created_relationship)
                    p51_source_relationships_.erase(c_guid);
                throw;
            }
            schedule_p51_reservation_sweep_on_owner();
            P51SourceRelationship& committed_relationship =
                p51_source_relationships_.find(c_guid)->second;
            if (committed_relationship.anchor_reservation_id == Id128{}) {
                committed_relationship.anchor_armed = armed;
                committed_relationship.anchor_reservation_id =
                    Id128{armed.reservation_id};
            }
            ++committed_relationship.outstanding;
            result.armed = std::move(armed);
        }, deadline);
    if (!completed && !result.armed.has_value() && result.error_code == 0)
        result.error_code = stop_requested_.load(std::memory_order_acquire)
                                ? kExpired
                                : kCapacity;
    if (!result.valid()) {
        result = {};
        result.error_code = kInvalid;
    }
    return result;
}

bool SidecarRuntime::p51_source_reservation_terminal_on_owner(
    const JobBind& binding) noexcept {
    if (binding.reservation_id == Id128{})
        return false;
    const auto position =
        p51_source_reservations_.find(binding.reservation_id.bytes);
    // This callback is only invoked by the endpoint for a previously retained
    // exact JOB_BIND proof. In that restricted context, removal of this
    // reservation ID is authoritative terminal state, not an inconclusive
    // lookup failure.
    if (position == p51_source_reservations_.end())
        return true;
    const P51SourceReservationRow& row = position->second;
    if (!row.consumed_binding || *row.consumed_binding != binding)
        return false;
    const auto clock = sidecar::process_monotonic_clock_identity();
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    const bool expired = row.absolute_deadline.expired(
        now_ns, clock.clock_domain_id, clock.time_namespace_id);
    return !row.publishing && (row.cancel_requested || expired);
}

void SidecarRuntime::schedule_p51_reservation_sweep_on_owner() noexcept {
    if (stop_requested_.load(std::memory_order_acquire)) {
        boost::system::error_code ignored;
        p51_reservation_sweep_timer_.cancel(ignored);
        return;
    }
    std::optional<std::chrono::steady_clock::time_point> next_expiry;
    for (const auto& [reservation_id, row] : p51_source_reservations_) {
        (void)reservation_id;
        // A publisher owns the final publication/cancellation edge. It will
        // remove or settle this row; don't spin a timer on an expired row
        // while that edge is in flight.
        if (row.publishing)
            continue;
        const auto expiry = row.absolute_deadline.as_steady_time_point();
        if (!next_expiry || expiry < *next_expiry)
            next_expiry = expiry;
    }
    boost::system::error_code ignored;
    if (!next_expiry) {
        p51_reservation_sweep_timer_.cancel(ignored);
        return;
    }
    p51_reservation_sweep_timer_.expires_at(*next_expiry, ignored);
    if (ignored)
        return;
    p51_reservation_sweep_timer_.async_wait([this](boost::system::error_code error) {
        if (error || stop_requested_.load(std::memory_order_acquire))
            return;
        sweep_p51_reservations_on_owner();
        schedule_p51_reservation_sweep_on_owner();
    });
}

void SidecarRuntime::sweep_p51_reservations_on_owner() noexcept {
    if (stop_requested_.load(std::memory_order_acquire))
        return;
    const auto clock = sidecar::process_monotonic_clock_identity();
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    for (auto reservation = p51_source_reservations_.begin();
         reservation != p51_source_reservations_.end();) {
        P51SourceReservationRow& row = reservation->second;
        if (row.publishing || !row.absolute_deadline.expired(
                                  now_ns, clock.clock_domain_id,
                                  clock.time_namespace_id)) {
            ++reservation;
            continue;
        }
        const CStoreGuid c_guid{row.armed.arm.source.c_store_guid};
        auto relationship = p51_source_relationships_.find(c_guid);
        if (!row.consumed && relationship != p51_source_relationships_.end() &&
            relationship->second.outstanding != 0)
            --relationship->second.outstanding;
        if (row.consumed && relationship != p51_source_relationships_.end() &&
            relationship->second.pending_ordinal == row.consumed_ordinal &&
            relationship->second.pending_physical_link_generation ==
                row.consumed_physical_link_generation) {
            relationship->second.pending_ordinal = 0;
            relationship->second.pending_physical_link_generation = 0;
            relationship->second.pending_binding_digest = {};
        }
        const Id128 reservation_id{row.armed.reservation_id};
        // The endpoint hook retires only a tombstone carrying this reservation
        // identity. It leaves committed residents and unrelated rows intact.
        [[maybe_unused]] bool endpoint_marker_retired = false;
        if (endpoint_)
            endpoint_marker_retired =
                endpoint_->retire_p51_recovery_install(reservation_id);
        reservation = p51_source_reservations_.erase(reservation);
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
        notify_p51_reservation_retired_for_test(
            reservation_id, endpoint_marker_retired);
#endif
    }
}

#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
uint64_t SidecarRuntime::active_source_raw_bytes_for_test() noexcept {
    std::lock_guard lock(source_admission_mutex_);
    return active_source_raw_bytes_;
}

void SidecarRuntime::run_owner_callback_for_test(
    std::function<void()> callback) {
    if (!callback)
        throw std::invalid_argument("owner test callback is empty");
    auto completion = std::make_shared<std::promise<void>>();
    auto completed = completion->get_future();
    asio::post(context_, [callback = std::move(callback), completion] {
        try {
            callback();
            completion->set_value();
        } catch (...) {
            completion->set_exception(std::current_exception());
        }
    });
    completed.get();
}

void SidecarRuntime::notify_p51_reservation_retired_for_test(
    const Id128& id, bool endpoint_marker_retired) noexcept {
    if (!config_.p51_reservation_retired_for_test)
        return;
    try {
        config_.p51_reservation_retired_for_test(id,
                                                endpoint_marker_retired);
    } catch (...) {
    }
}
#endif

bool SidecarRuntime::cancel_p51_source_on_owner(
    const P51SourceArmFields& arm,
    const std::array<uint8_t, 16>& reservation_id,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (!arm.valid() ||
        std::all_of(reservation_id.begin(), reservation_id.end(),
                    [](uint8_t byte) { return byte == 0; }))
        return false;
    bool removed = false;
    const bool completed = owner_round_trip(
        [this, &arm, &reservation_id, &removed] {
            const auto position = p51_source_reservations_.find(reservation_id);
            if (position == p51_source_reservations_.end() ||
                position->second.armed.arm != arm)
                return;
            const CStoreGuid c_guid{arm.source.c_store_guid};
            const auto relationship = p51_source_relationships_.find(c_guid);
            if (relationship == p51_source_relationships_.end() ||
                (!position->second.consumed &&
                 relationship->second.outstanding == 0))
                return;
            if (!position->second.consumed) {
                --relationship->second.outstanding;
            } else if (relationship->second.pending_ordinal ==
                           position->second.consumed_ordinal &&
                       relationship->second.pending_physical_link_generation ==
                           position->second.consumed_physical_link_generation) {
                if (position->second.publishing)
                    return;
                position->second.cancel_requested = true;
                removed = true;
                return;
            } else {
                return;
            }
            const Id128 id{position->second.armed.reservation_id};
            [[maybe_unused]] const bool marker_retired = endpoint_ &&
                endpoint_->retire_p51_recovery_install(id);
            p51_source_reservations_.erase(position);
            schedule_p51_reservation_sweep_on_owner();
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
            notify_p51_reservation_retired_for_test(id, marker_retired);
#endif
            removed = true;
        }, deadline, true);
    return completed && removed;
}

std::optional<P51SourceLinkLease>
SidecarRuntime::lookup_p51_link_reservation_on_owner(
    const LinkHello& hello) noexcept {
    if (stop_requested_.load(std::memory_order_acquire) ||
        hello.revision != 2 ||
        hello.reservation_id == Id128{} || hello.relationship_id == Id128{} ||
        hello.relationship_epoch == 0 || hello.physical_link_generation == 0)
        return std::nullopt;
    const auto relationship = p51_source_relationships_.find(hello.c_store_guid);
    if (hello.start_mode == LinkStartMode::Reconnect) {
        if (relationship == p51_source_relationships_.end())
            return std::nullopt;
        P51SourceRelationship& row = relationship->second;
        const bool current_epoch = hello.relationship_epoch == row.epoch &&
                                  hello.history_nonce == row.history_nonce;
        const bool replay_old_reset = row.last_reset_ack &&
            row.last_reset_ack->request.old_relationship_epoch ==
                hello.relationship_epoch &&
            row.last_reset_ack->request.old_history_nonce == hello.history_nonce;
        if (row.logical_id != hello.relationship_id.bytes ||
            row.profile != hello.profile || row.selected_window != hello.window ||
            row.c_store_generation != hello.c_store_generation ||
            row.c_control_generation != hello.c_control_generation ||
            row.c_control_attempt != hello.c_control_attempt ||
            row.anchor_reservation_id != hello.reservation_id ||
            (!current_epoch && !replay_old_reset) ||
            hello.f_store_guid != config_.f_store_guid ||
            hello.f_store_generation != config_.f_store_generation ||
            hello.physical_link_generation <=
                row.highest_physical_link_generation ||
            row.anchor_armed.selected_revision != CACHE_WIRE_REVISION_R2)
            return std::nullopt;
        row.link_active = true;
        row.physical_link_generation = hello.physical_link_generation;
        row.highest_physical_link_generation = hello.physical_link_generation;
        row.recovery_context.reset();
        const auto clock = sidecar::process_monotonic_clock_identity();
        const auto link_deadline =
            sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
                std::chrono::steady_clock::now() + std::chrono::hours(24),
                clock.clock_domain_id, clock.time_namespace_id);
        P51SourceArmedFields anchor = row.anchor_armed;
        anchor.relationship_epoch = row.epoch;
        return P51SourceLinkLease{anchor, link_deadline, true, row.epoch,
                                  row.history_nonce,
                                  row.committed_prefix_k,
                                  row.acknowledged_prefix_q};
    }
    if (hello.start_mode != LinkStartMode::Initial)
        return std::nullopt;
    const auto position = p51_source_reservations_.find(hello.reservation_id.bytes);
    if (position == p51_source_reservations_.end())
        return std::nullopt;
    const P51SourceArmedFields& armed = position->second.armed;
    const auto selected_profile = profile_from_cache_profile_mask(
        armed.arm.source.cache_profile);
    if (!armed.valid() ||
        !selected_profile ||
        position->second.absolute_deadline.expired(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            sidecar::process_monotonic_clock_identity().clock_domain_id,
            sidecar::process_monotonic_clock_identity().time_namespace_id) ||
        hello.relationship_id.bytes != armed.logical_relationship_id ||
        hello.relationship_epoch != armed.relationship_epoch ||
        hello.c_store_guid.bytes != armed.arm.source.c_store_guid ||
        hello.c_store_generation != armed.arm.source.c_store_generation ||
        hello.c_control_generation != armed.arm.source.c_control_generation ||
        hello.c_control_attempt != armed.arm.source.c_control_attempt ||
        hello.profile != *selected_profile ||
        hello.window != armed.selected_window ||
        hello.f_store_guid.bytes != armed.f_store_guid ||
        hello.f_store_generation != armed.f_store_generation)
        return std::nullopt;
    if (relationship == p51_source_relationships_.end() ||
        relationship->second.logical_id != armed.logical_relationship_id ||
        relationship->second.epoch != armed.relationship_epoch ||
        relationship->second.profile != hello.profile ||
        relationship->second.c_store_generation != hello.c_store_generation ||
        relationship->second.c_control_generation != hello.c_control_generation ||
        relationship->second.c_control_attempt != hello.c_control_attempt ||
        relationship->second.next_relationship_ordinal != 1 ||
        relationship->second.has_receipts ||
        relationship->second.link_active ||
        hello.physical_link_generation <=
            relationship->second.highest_physical_link_generation ||
        (relationship->second.history_nonce.value != 0 &&
         relationship->second.history_nonce != hello.history_nonce))
        return std::nullopt;
    relationship->second.link_active = true;
    relationship->second.physical_link_generation =
        hello.physical_link_generation;
    relationship->second.highest_physical_link_generation =
        hello.physical_link_generation;
    relationship->second.history_nonce = hello.history_nonce;
    relationship->second.anchor_armed = armed;
    relationship->second.anchor_reservation_id = hello.reservation_id;
    return P51SourceLinkLease{armed, position->second.absolute_deadline,
                              false, relationship->second.epoch,
                              relationship->second.history_nonce, 0, 0};
}

std::optional<P51SourceJobLease>
SidecarRuntime::consume_p51_job_reservation_on_owner(
    const LinkHello& link, const JobBind& binding) noexcept {
    if (stop_requested_.load(std::memory_order_acquire) ||
        binding.reservation_id == Id128{} || binding.relationship_ordinal == 0 ||
        binding.physical_link_generation == 0 ||
        binding.physical_link_generation != link.physical_link_generation)
        return std::nullopt;
    const auto position = p51_source_reservations_.find(binding.reservation_id.bytes);
    if (position == p51_source_reservations_.end())
        return std::nullopt;
    const P51SourceArmedFields& armed = position->second.armed;
    const P50SourceArmFields& source = armed.arm.source;
    const auto selected_profile =
        profile_from_cache_profile_mask(source.cache_profile);
    const auto clock = sidecar::process_monotonic_clock_identity();
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    if (!armed.valid() || !selected_profile || position->second.consumed ||
        position->second.absolute_deadline.expired(
                              now_ns, clock.clock_domain_id,
                              clock.time_namespace_id) ||
        binding.wire_job_id != source.wire_job_id ||
        binding.assignment_epoch != source.assignment_epoch ||
        binding.assignment_nonce != source.assignment_nonce ||
        binding.logical_job != source.logical_job ||
        binding.compiler_attempt != source.compiler_attempt ||
        binding.source_request_id != source.source_request_id ||
        binding.profile != *selected_profile ||
        link.relationship_id.bytes != armed.logical_relationship_id ||
        link.relationship_epoch != armed.relationship_epoch ||
        link.c_store_guid.bytes != source.c_store_guid ||
        link.c_store_generation != source.c_store_generation ||
        link.c_control_generation != source.c_control_generation ||
        link.c_control_attempt != source.c_control_attempt ||
        link.f_store_guid.bytes != armed.f_store_guid ||
        link.f_store_generation != armed.f_store_generation ||
        link.profile != binding.profile ||
        link.physical_link_generation != binding.physical_link_generation)
        return std::nullopt;
    P51SourceJobLease lease;
    lease.armed = armed;
    lease.absolute_deadline = position->second.absolute_deadline;
    lease.binding = binding;
    lease.input_key = InputRecordKey{CStoreGuid{source.c_store_guid}, binding.tu_seq};
    const CStoreGuid c_guid{source.c_store_guid};
    const auto relationship = p51_source_relationships_.find(c_guid);
    if (relationship == p51_source_relationships_.end() ||
        relationship->second.outstanding == 0 ||
        relationship->second.logical_id != armed.logical_relationship_id ||
        relationship->second.epoch != armed.relationship_epoch ||
        !relationship->second.link_active ||
        relationship->second.physical_link_generation !=
            link.physical_link_generation ||
        relationship->second.next_relationship_ordinal != binding.relationship_ordinal ||
        relationship->second.next_relationship_ordinal == UINT64_MAX ||
        relationship->second.pending_ordinal != 0 ||
        relationship->second.committed_prefix_k -
                relationship->second.acknowledged_prefix_q >=
            relationship->second.selected_window)
        return std::nullopt;
    const size_t receipt_index = static_cast<size_t>(
        (binding.relationship_ordinal - 1) %
        relationship->second.receipt_rows.size());
    if (relationship->second.receipt_rows[receipt_index].has_value())
        return std::nullopt;
    Digest128 binding_digest;
    try {
        binding_digest = compute_r2_binding_digest(binding);
    } catch (...) {
        return std::nullopt;
    }
    --relationship->second.outstanding;
    relationship->second.pending_ordinal = binding.relationship_ordinal;
    relationship->second.pending_physical_link_generation =
        link.physical_link_generation;
    relationship->second.pending_binding_digest = binding_digest;
    lease.binding_digest = binding_digest;
    position->second.consumed = true;
    position->second.consumed_ordinal = binding.relationship_ordinal;
    position->second.consumed_physical_link_generation =
        link.physical_link_generation;
    position->second.cancel_requested = false;
    position->second.publishing = false;
    position->second.consumed_binding = binding;
    return lease;
}

bool SidecarRuntime::authorize_p51_job_publication_on_owner(
    const LinkHello& link, const JobBind& binding) noexcept {
    const auto reservation =
        p51_source_reservations_.find(binding.reservation_id.bytes);
    const auto relationship = p51_source_relationships_.find(link.c_store_guid);
    if (reservation == p51_source_reservations_.end() ||
        relationship == p51_source_relationships_.end() ||
        reservation->second.cancel_requested ||
        reservation->second.publishing || !reservation->second.consumed ||
        reservation->second.consumed_ordinal != binding.relationship_ordinal ||
        reservation->second.consumed_physical_link_generation !=
            link.physical_link_generation ||
        relationship->second.pending_ordinal != binding.relationship_ordinal ||
        relationship->second.pending_physical_link_generation !=
            link.physical_link_generation ||
        relationship->second.physical_link_generation !=
            link.physical_link_generation ||
        relationship->second.logical_id != link.relationship_id.bytes)
        return false;
    const auto clock = sidecar::process_monotonic_clock_identity();
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    if (reservation->second.absolute_deadline.expired(
            now_ns, clock.clock_domain_id, clock.time_namespace_id))
        return false;
    reservation->second.publishing = true;
    return true;
}

void SidecarRuntime::settle_p51_cancelled_job_on_owner(
    const LinkHello& link, const JobBind& binding) noexcept {
    const auto relationship = p51_source_relationships_.find(link.c_store_guid);
    const auto reservation =
        p51_source_reservations_.find(binding.reservation_id.bytes);
    if (relationship == p51_source_relationships_.end() ||
        reservation == p51_source_reservations_.end() ||
        !reservation->second.cancel_requested ||
        reservation->second.publishing ||
        reservation->second.consumed_ordinal != binding.relationship_ordinal ||
        reservation->second.consumed_physical_link_generation !=
            link.physical_link_generation ||
        relationship->second.pending_ordinal != binding.relationship_ordinal ||
        relationship->second.pending_physical_link_generation !=
            link.physical_link_generation)
        return;
    relationship->second.pending_ordinal = 0;
    relationship->second.pending_physical_link_generation = 0;
    relationship->second.pending_binding_digest = {};
    const Id128 id{reservation->second.armed.reservation_id};
    [[maybe_unused]] const bool marker_retired = endpoint_ &&
        endpoint_->retire_p51_recovery_install(id);
    p51_source_reservations_.erase(reservation);
    schedule_p51_reservation_sweep_on_owner();
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    notify_p51_reservation_retired_for_test(id, marker_retired);
#endif
}

bool SidecarRuntime::record_p51_job_commit_on_owner(
    const LinkHello& link, const JobBind& binding,
    const R2TxCommit& commit) noexcept {
    const auto relationship = p51_source_relationships_.find(link.c_store_guid);
    if (relationship == p51_source_relationships_.end() ||
        !relationship->second.link_active ||
        relationship->second.physical_link_generation !=
            link.physical_link_generation ||
        relationship->second.logical_id != link.relationship_id.bytes ||
        relationship->second.epoch != link.relationship_epoch ||
        relationship->second.pending_ordinal != binding.relationship_ordinal ||
        relationship->second.committed_prefix_k + 1 !=
            binding.relationship_ordinal ||
        commit.relationship_ordinal != binding.relationship_ordinal ||
        commit.binding_digest != relationship->second.pending_binding_digest ||
        commit.inner.tu_seq != binding.tu_seq ||
        commit.inner.raw_digest != binding.raw_digest)
        return false;
    const size_t row_index = static_cast<size_t>(
        (binding.relationship_ordinal - 1) %
        relationship->second.receipt_rows.size());
    auto& row = relationship->second.receipt_rows[row_index];
    if (row.has_value())
        return false;
    const auto reservation =
        p51_source_reservations_.find(binding.reservation_id.bytes);
    if (reservation == p51_source_reservations_.end() ||
        !reservation->second.consumed ||
        !reservation->second.publishing ||
        reservation->second.cancel_requested ||
        reservation->second.consumed_ordinal != binding.relationship_ordinal ||
        reservation->second.consumed_physical_link_generation !=
            link.physical_link_generation)
        return false;
    row = commit;
    p51_source_reservations_.erase(reservation);
    schedule_p51_reservation_sweep_on_owner();
    relationship->second.committed_prefix_k = binding.relationship_ordinal;
    relationship->second.next_relationship_ordinal =
        binding.relationship_ordinal + 1;
    relationship->second.pending_ordinal = 0;
    relationship->second.pending_physical_link_generation = 0;
    relationship->second.pending_binding_digest = {};
    relationship->second.has_receipts = true;
    return true;
}

bool SidecarRuntime::acknowledge_p51_receipt_on_owner(
    const LinkHello& link, const CommitAck& ack) noexcept {
    const auto relationship = p51_source_relationships_.find(link.c_store_guid);
    if (relationship == p51_source_relationships_.end() ||
        !relationship->second.link_active ||
        relationship->second.physical_link_generation !=
            link.physical_link_generation ||
        relationship->second.logical_id != ack.relationship_id.bytes ||
        relationship->second.epoch != ack.relationship_epoch ||
        ack.physical_link_generation != link.physical_link_generation ||
        ack.contiguous_verified_ordinal <=
            relationship->second.acknowledged_prefix_q ||
        ack.contiguous_verified_ordinal >
            relationship->second.committed_prefix_k)
        return false;
    for (uint64_t ordinal = relationship->second.acknowledged_prefix_q + 1;
         ordinal <= ack.contiguous_verified_ordinal; ++ordinal) {
        const size_t row_index = static_cast<size_t>(
            (ordinal - 1) % relationship->second.receipt_rows.size());
        const auto& row = relationship->second.receipt_rows[row_index];
        if (!row.has_value() || row->relationship_ordinal != ordinal)
            return false;
    }
    relationship->second.acknowledged_prefix_q =
        ack.contiguous_verified_ordinal;
    for (auto& receipt : relationship->second.receipt_rows) {
        if (receipt && receipt->relationship_ordinal <=
                           relationship->second.acknowledged_prefix_q)
            receipt.reset();
    }
    relationship->second.has_receipts =
        relationship->second.committed_prefix_k >
        relationship->second.acknowledged_prefix_q;
    return true;
}

bool SidecarRuntime::settle_p51_interrupted_job_on_owner(
    const LinkHello& link) noexcept {
    const auto position = p51_source_relationships_.find(link.c_store_guid);
    if (position == p51_source_relationships_.end())
        return false;
    P51SourceRelationship& relationship = position->second;
    if (!relationship.link_active ||
        relationship.physical_link_generation !=
            link.physical_link_generation)
        return false;
    if (relationship.pending_ordinal == 0)
        return relationship.pending_physical_link_generation == 0;
    if (relationship.pending_physical_link_generation == 0 ||
        relationship.pending_physical_link_generation >=
            link.physical_link_generation)
        return false;
    const uint64_t interrupted_ordinal = relationship.pending_ordinal;
    const uint64_t interrupted_generation =
        relationship.pending_physical_link_generation;
    // The R2 endpoint calls this only after owner-affine activate() has
    // replaced the old session and discarded its staged decode. The source
    // bytes remain on C for suffix rebuild; no F input was committed here.
    relationship.pending_ordinal = 0;
    relationship.pending_physical_link_generation = 0;
    relationship.pending_binding_digest = {};
    for (auto reservation = p51_source_reservations_.begin();
         reservation != p51_source_reservations_.end(); ++reservation) {
        auto& row = reservation->second;
        if (!row.consumed || row.consumed_ordinal != interrupted_ordinal ||
            row.consumed_physical_link_generation != interrupted_generation)
            continue;
        if (row.cancel_requested && !row.publishing) {
            if (endpoint_)
                (void)endpoint_->retire_p51_recovery_install(
                    Id128{row.armed.reservation_id});
            p51_source_reservations_.erase(reservation);
        } else {
            row.consumed_binding.reset();
            row.consumed_ordinal = 0;
            row.consumed_physical_link_generation = 0;
        }
        break;
    }
    schedule_p51_reservation_sweep_on_owner();
    return true;
}

std::optional<P51RecoveryReceiptInterval>
SidecarRuntime::recover_p51_receipts_on_owner(
    const LinkHello& link, const RecoverBegin& begin,
    std::span<const RecoverWitness> witnesses,
    const RecoverEnd& end) noexcept {
    try {
        if (stop_requested_.load(std::memory_order_acquire) ||
            begin.relationship_id != link.relationship_id ||
            begin.relationship_epoch != link.relationship_epoch ||
            begin.physical_link_generation != link.physical_link_generation ||
            begin.verified_floor_a != link.verified_receipt_floor ||
            end.relationship_id != begin.relationship_id ||
            end.relationship_epoch != begin.relationship_epoch ||
            end.physical_link_generation != begin.physical_link_generation ||
            end.operation_id != begin.operation_id ||
            end.witness_count != begin.witness_count ||
            witnesses.size() != begin.witness_count ||
            end.transcript_digest !=
                compute_r2_recovery_transcript_digest(begin, witnesses))
            return std::nullopt;
        const auto position = p51_source_relationships_.find(link.c_store_guid);
        if (position == p51_source_relationships_.end())
            return std::nullopt;
        P51SourceRelationship& relationship = position->second;
        if (!relationship.link_active ||
            relationship.physical_link_generation !=
                link.physical_link_generation ||
            relationship.logical_id != begin.relationship_id.bytes ||
            relationship.epoch != begin.relationship_epoch ||
            begin.prepared_prefix_p < relationship.committed_prefix_k ||
            begin.prepared_prefix_p - begin.verified_floor_a >
                relationship.selected_window ||
            begin.verified_floor_a < relationship.acknowledged_prefix_q ||
            begin.verified_floor_a > relationship.committed_prefix_k ||
            relationship.pending_ordinal != 0 ||
            relationship.pending_physical_link_generation != 0)
            return std::nullopt;

        uint64_t ordinal = begin.verified_floor_a;
        for (const RecoverWitness& witness : witnesses) {
            if (ordinal == UINT64_MAX)
                return std::nullopt;
            ++ordinal;
            if (witness.relationship_id != begin.relationship_id ||
                witness.relationship_epoch != begin.relationship_epoch ||
                witness.physical_link_generation !=
                    begin.physical_link_generation ||
                witness.operation_id != begin.operation_id ||
                witness.relationship_ordinal != ordinal)
                return std::nullopt;
            if (ordinal > relationship.committed_prefix_k)
                continue;
            const size_t row_index = static_cast<size_t>(
                (ordinal - 1) % relationship.receipt_rows.size());
            const auto& retained = relationship.receipt_rows[row_index];
            if (!retained || retained->relationship_ordinal != ordinal ||
                retained->binding_digest != witness.binding_digest ||
                retained->transaction_digest != witness.transaction_digest ||
                retained->inner.history_nonce != witness.inner.history_nonce ||
                retained->inner.rel_seq != witness.inner.rel_seq ||
                retained->inner.tu_seq != witness.inner.tu_seq ||
                retained->inner.transaction_digest !=
                    witness.inner.transaction_digest ||
                retained->inner.raw_digest != witness.inner.raw_digest)
                return std::nullopt;
        }
        if (ordinal != begin.prepared_prefix_p)
            return std::nullopt;

        P51RecoveryReceiptInterval result;
        result.end.relationship_id = begin.relationship_id;
        result.end.relationship_epoch = begin.relationship_epoch;
        result.end.physical_link_generation =
            begin.physical_link_generation;
        result.end.operation_id = begin.operation_id;
        result.end.verified_floor_a = begin.verified_floor_a;
        result.end.committed_prefix_k = relationship.committed_prefix_k;
        result.end.acknowledged_prefix_q =
            relationship.acknowledged_prefix_q;
        result.end.receipt_count = static_cast<uint32_t>(
            relationship.committed_prefix_k - begin.verified_floor_a);
        result.rows.reserve(result.end.receipt_count);
        for (uint64_t receipt_ordinal = begin.verified_floor_a + 1;
             receipt_ordinal <= relationship.committed_prefix_k;
             ++receipt_ordinal) {
            const size_t row_index = static_cast<size_t>(
                (receipt_ordinal - 1) % relationship.receipt_rows.size());
            const auto& retained = relationship.receipt_rows[row_index];
            if (!retained ||
                retained->relationship_ordinal != receipt_ordinal)
                return std::nullopt;
            result.rows.push_back(ReceiptRow{
                begin.relationship_id, begin.relationship_epoch,
                begin.physical_link_generation, begin.operation_id,
                *retained});
        }
        relationship.recovery_context = P51SourceRelationship::RecoveryContext{
            begin.operation_id, begin.physical_link_generation,
            begin.relationship_epoch, begin.verified_floor_a,
            begin.prepared_prefix_p, relationship.committed_prefix_k,
            relationship.acknowledged_prefix_q, end.transcript_digest};
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<ResetAck> SidecarRuntime::validate_p51_reset_on_owner(
    const LinkHello& link, const ResetRequest& request) noexcept {
    const auto position = p51_source_relationships_.find(link.c_store_guid);
    if (position == p51_source_relationships_.end())
        return std::nullopt;
    P51SourceRelationship& relationship = position->second;
    if (request.relationship_id.bytes != relationship.logical_id ||
        request.physical_link_generation != link.physical_link_generation ||
        !relationship.link_active ||
        relationship.physical_link_generation !=
            link.physical_link_generation)
        return std::nullopt;
    if (relationship.last_reset_ack &&
        request.operation_id ==
            relationship.last_reset_ack->request.operation_id) {
        const ResetRequest& prior = relationship.last_reset_ack->request;
        if (request.relationship_id != prior.relationship_id ||
            request.old_relationship_epoch != prior.old_relationship_epoch ||
            request.new_relationship_epoch != prior.new_relationship_epoch ||
            request.operation_id != prior.operation_id ||
            request.settled_prefix_k != prior.settled_prefix_k ||
            request.old_history_nonce != prior.old_history_nonce ||
            request.new_history_nonce != prior.new_history_nonce)
            return std::nullopt;
        ResetAck replay = *relationship.last_reset_ack;
        replay.request.physical_link_generation =
            request.physical_link_generation;
        return replay;
    }
    if (relationship.last_reset_ack &&
        !relationship.last_reset_confirmed)
        return std::nullopt;
    if (relationship.epoch != request.old_relationship_epoch ||
        link.relationship_epoch != request.old_relationship_epoch ||
        relationship.history_nonce != request.old_history_nonce ||
        request.new_history_nonce.value <=
            relationship.history_nonce.value ||
        relationship.committed_prefix_k != request.settled_prefix_k ||
        relationship.pending_ordinal != 0)
        return std::nullopt;
    if (!relationship.recovery_context ||
        relationship.recovery_context->operation_id != request.operation_id ||
        relationship.recovery_context->physical_link_generation !=
            link.physical_link_generation ||
        relationship.recovery_context->relationship_epoch !=
            request.old_relationship_epoch ||
        relationship.recovery_context->committed_prefix_k !=
            request.settled_prefix_k ||
        relationship.recovery_context->committed_prefix_k !=
            relationship.committed_prefix_k ||
        relationship.recovery_context->verified_floor_a <
            relationship.acknowledged_prefix_q)
        return std::nullopt;
    for (uint64_t ordinal = relationship.acknowledged_prefix_q + 1;
         ordinal <= request.settled_prefix_k; ++ordinal) {
        const size_t row_index = static_cast<size_t>(
            (ordinal - 1) % relationship.receipt_rows.size());
        const auto& receipt = relationship.receipt_rows[row_index];
        if (!receipt || receipt->relationship_ordinal != ordinal)
            return std::nullopt;
    }
    ResetRequest current = request;
    current.physical_link_generation = link.physical_link_generation;
    ResetAck ack;
    ack.request = current;
    ack.initial_state_digest =
        initial_route_digest(link.c_store_guid, request.new_history_nonce);
    ack.next_rel_seq = RelSeq{0};
    return ack;
}

bool SidecarRuntime::commit_p51_reset_on_owner(
    const LinkHello& link, const ResetRequest& request,
    const ResetAck& ack) noexcept {
    const auto position = p51_source_relationships_.find(link.c_store_guid);
    if (position == p51_source_relationships_.end())
        return false;
    P51SourceRelationship& relationship = position->second;
    if (relationship.epoch != request.old_relationship_epoch ||
        relationship.history_nonce != request.old_history_nonce ||
        relationship.physical_link_generation !=
            link.physical_link_generation ||
        relationship.committed_prefix_k != request.settled_prefix_k ||
        !relationship.link_active ||
        ack.request.relationship_id != request.relationship_id ||
        ack.request.old_relationship_epoch != request.old_relationship_epoch ||
        ack.request.new_relationship_epoch != request.new_relationship_epoch ||
        ack.request.operation_id != request.operation_id ||
        ack.request.settled_prefix_k != request.settled_prefix_k ||
        ack.request.old_history_nonce != request.old_history_nonce ||
        ack.request.new_history_nonce != request.new_history_nonce ||
        ack.request.physical_link_generation !=
            link.physical_link_generation ||
        ack.request.new_relationship_epoch != request.new_relationship_epoch ||
        ack.initial_state_digest !=
            initial_route_digest(link.c_store_guid, request.new_history_nonce))
        return false;
    relationship.epoch = request.new_relationship_epoch;
    relationship.history_nonce = request.new_history_nonce;
    relationship.acknowledged_prefix_q = request.settled_prefix_k;
    relationship.has_receipts = false;
    relationship.pending_ordinal = 0;
    relationship.pending_binding_digest = {};
    for (auto& receipt : relationship.receipt_rows)
        receipt.reset();
    relationship.anchor_armed.relationship_epoch = request.new_relationship_epoch;
    relationship.last_reset_ack = ack;
    relationship.last_reset_confirmed = false;
    relationship.recovery_context.reset();
    const auto clock = sidecar::process_monotonic_clock_identity();
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    for (auto reservation = p51_source_reservations_.begin();
         reservation != p51_source_reservations_.end();) {
        if (reservation->second.armed.logical_relationship_id ==
                relationship.logical_id &&
            reservation->second.armed.relationship_epoch ==
                request.old_relationship_epoch) {
            auto& row = reservation->second;
            const bool live = !row.cancel_requested &&
                !row.absolute_deadline.expired(
                    now_ns, clock.clock_domain_id, clock.time_namespace_id);
            if (!live) {
                if (!row.consumed && relationship.outstanding != 0)
                    --relationship.outstanding;
                const Id128 id{row.armed.reservation_id};
                [[maybe_unused]] const bool marker_retired = endpoint_ &&
                    endpoint_->retire_p51_recovery_install(id);
                reservation = p51_source_reservations_.erase(reservation);
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
                notify_p51_reservation_retired_for_test(id, marker_retired);
#endif
                continue;
            }
            // An exact recovered suffix may be rebound under the new codec
            // history, but only while its original compiler-owned deadline
            // remains live. Preserve the reservation ID and ARM identity.
            row.armed.relationship_epoch = request.new_relationship_epoch;
            if (row.consumed) {
                row.consumed = false;
                row.consumed_ordinal = 0;
                row.consumed_physical_link_generation = 0;
                row.consumed_binding.reset();
                ++relationship.outstanding;
            }
            ++reservation;
        } else {
            ++reservation;
        }
    }
    schedule_p51_reservation_sweep_on_owner();
    return true;
}

bool SidecarRuntime::confirm_p51_reset_on_owner(
    const LinkHello& link, const ResetConfirm& confirm) noexcept {
    const auto position = p51_source_relationships_.find(link.c_store_guid);
    if (position == p51_source_relationships_.end())
        return false;
    P51SourceRelationship& relationship = position->second;
    if (!relationship.last_reset_ack ||
        relationship.physical_link_generation !=
            link.physical_link_generation ||
        !relationship.link_active ||
        confirm.relationship_id.bytes != relationship.logical_id ||
        confirm.new_relationship_epoch != relationship.epoch ||
        confirm.physical_link_generation !=
            link.physical_link_generation ||
        confirm.operation_id !=
            relationship.last_reset_ack->request.operation_id ||
        confirm.new_history_nonce != relationship.history_nonce ||
        confirm.settled_prefix_k !=
            relationship.last_reset_ack->request.settled_prefix_k)
        return false;
    relationship.last_reset_confirmed = true;
    return true;
}

void SidecarRuntime::release_p51_link_on_owner(const LinkHello& hello) noexcept {
    const CStoreGuid c_guid = hello.c_store_guid;
    const auto relationship = p51_source_relationships_.find(c_guid);
    if (relationship == p51_source_relationships_.end() ||
        relationship->second.logical_id != hello.relationship_id.bytes ||
        relationship->second.physical_link_generation !=
            hello.physical_link_generation)
        return;
    relationship->second.link_active = false;
    relationship->second.physical_link_generation = 0;
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
struct P51TransferSettlement {
    explicit P51TransferSettlement(std::function<void()> completion)
        : callback(std::move(completion)) {}
    ~P51TransferSettlement() { run(); }
    void run() noexcept {
        if (done.exchange(true, std::memory_order_acq_rel))
            return;
        if (callback) {
            try { callback(); } catch (...) {}
            callback = {};
        }
    }
    std::atomic<bool> done{false};
    std::function<void()> callback;
};
} // namespace

struct SidecarRuntime::P51TransferReplyPump {
    P51TransferReplyPump(boost::asio::io_context& context,
                         local::Connection socket,
                         local::Identity expected_identity,
                         std::chrono::steady_clock::time_point operation_deadline,
                         local::Frame response,
                         std::shared_ptr<P51TransferSettlement> completion)
        : connection(std::move(socket)), readiness(context),
          timer(context), identity(expected_identity), deadline(operation_deadline),
          operation(std::make_unique<local::FrameOperation>(
              connection, response, operation_deadline)),
          settled(std::move(completion)) {}

    local::Connection connection;
    boost::asio::posix::stream_descriptor readiness;
    boost::asio::steady_timer timer;
    local::Identity identity{};
    std::chrono::steady_clock::time_point deadline;
    std::unique_ptr<local::FrameOperation> operation;
    std::shared_ptr<P51TransferSettlement> settled;
    bool writing = true;
    bool closed = false;
};

void SidecarRuntime::start_p51_transfer_reply(
    local::Connection connection, local::Identity identity,
    local::ControlOperation operation,
    local::P50SourceTransferResult result,
    std::function<void()> settled) noexcept {
    if (!connection.valid()) {
        if (settled) {
            try { settled(); } catch (...) {}
        }
        return;
    }
    if (stop_requested_.load(std::memory_order_acquire)) {
        if (settled) {
            try { settled(); } catch (...) {}
        }
        return;
    }
    const auto deadline = operation.p51_source_transfer.has_value()
        ? operation.p51_source_transfer->absolute_deadline.as_steady_time_point()
        : std::chrono::steady_clock::now();
    std::shared_ptr<P51TransferSettlement> settlement;
    int readiness_fd = -1;
    std::shared_ptr<P51TransferReplyPump> pump;
    try {
        settlement = std::make_shared<P51TransferSettlement>(settled);
        const std::vector<uint8_t> payload =
            local::encode_control_operation(
                local::make_p51_source_transfer_reply_operation(operation, result));
        if (payload.empty() || std::chrono::steady_clock::now() >= deadline) {
            settlement->run();
            return;
        }
        const local::Frame response{local::kProtocolVersion,
                                    local::MessageType::Data,
                                    identity, payload};
        readiness_fd = ::dup(connection.native_handle());
        if (readiness_fd < 0) {
            settlement->run();
            return;
        }
        const int readiness_flags = ::fcntl(readiness_fd, F_GETFD);
        if (readiness_flags < 0 ||
            ::fcntl(readiness_fd, F_SETFD, readiness_flags | FD_CLOEXEC) < 0) {
            (void)::close(readiness_fd);
            readiness_fd = -1;
            settlement->run();
            return;
        }
        pump = std::make_shared<P51TransferReplyPump>(
            context_, std::move(connection), identity, deadline,
            response, settlement);
        boost::system::error_code assign_error;
        pump->readiness.assign(readiness_fd, assign_error);
        if (assign_error) {
            (void)::close(readiness_fd);
            readiness_fd = -1;
            close_p51_transfer_reply(pump);
            return;
        }
        readiness_fd = -1;
        p51_transfer_reply_pumps_.push_back(pump);
        pump->timer.expires_at(deadline);
        pump->timer.async_wait([this, pump](const boost::system::error_code& error) {
            if (!error)
                close_p51_transfer_reply(pump);
        });
        advance_p51_transfer_reply(std::move(pump));
    } catch (...) {
        if (pump)
            close_p51_transfer_reply(pump);
        else {
            if (readiness_fd >= 0)
                (void)::close(readiness_fd);
            if (settlement)
                settlement->run();
            else if (settled)
                settled();
        }
    }
}

void SidecarRuntime::advance_p51_transfer_reply(
    std::shared_ptr<P51TransferReplyPump> pump) noexcept {
    if (pump->closed)
        return;
    pump->operation->advance();
    if (!pump->operation->done()) {
        const short events = pump->operation->poll_events();
        const auto wait = (events & POLLOUT) != 0
            ? boost::asio::posix::stream_descriptor::wait_write
            : boost::asio::posix::stream_descriptor::wait_read;
        try {
            pump->readiness.async_wait(
                wait, [this, pump](const boost::system::error_code& error) mutable {
                    if (pump->closed)
                        return;
                    if (error) {
                        close_p51_transfer_reply(std::move(pump));
                        return;
                    }
                    advance_p51_transfer_reply(std::move(pump));
                });
        } catch (...) {
            close_p51_transfer_reply(std::move(pump));
        }
        return;
    }
    if (pump->operation->status() != local::Status::Ok) {
        close_p51_transfer_reply(std::move(pump));
        return;
    }
    if (pump->writing) {
        pump->writing = false;
        pump->operation.reset();
        try {
            pump->operation = std::make_unique<local::FrameOperation>(
                pump->connection, pump->deadline);
        } catch (...) {
            close_p51_transfer_reply(std::move(pump));
            return;
        }
        advance_p51_transfer_reply(std::move(pump));
        return;
    }
    const local::Frame& acknowledgement = pump->operation->frame();
    const bool valid = acknowledgement.type == local::MessageType::Goodbye &&
        acknowledgement.payload.empty() &&
        local::validate_identity(acknowledgement, pump->identity) ==
            local::Status::Ok;
    (void)valid; // both success and a rejected ACK terminate this exact operation
    close_p51_transfer_reply(std::move(pump));
}

void SidecarRuntime::close_p51_transfer_reply(
    std::shared_ptr<P51TransferReplyPump> pump) noexcept {
    if (!pump || pump->closed)
        return;
    pump->closed = true;
    pump->operation.reset();
    boost::system::error_code ignored;
    pump->timer.cancel(ignored);
    pump->readiness.cancel(ignored);
    pump->readiness.close(ignored);
    for (auto it = p51_transfer_reply_pumps_.begin();
         it != p51_transfer_reply_pumps_.end(); ++it) {
        if (it->get() == pump.get()) {
            p51_transfer_reply_pumps_.erase(it);
            break;
        }
    }
    if (pump->settled)
        pump->settled->run();
}

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

void SidecarRuntime::start_adopted_r2_endpoint(
    int adopted_fd, EndpointIoControl endpoint_control) noexcept {
    if (adopted_fd < 0)
        return;
    std::promise<EndpointOwnerResult> completion;
    try {
        asio::co_spawn(context_,
                       run_endpoint_on_owner(adopted_fd,
                                              std::move(endpoint_control),
                                              std::move(completion), -1,
                                              true),
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
    auto result = std::make_shared<bool>(false);
    return owner_round_trip(
        [this, host = std::move(host), cache_port, guid, generation, result] {
            *result = bind_route_endpoint_identity(
                RouteEndpointKey{host, cache_port},
                RouteStoreIdentity{guid, generation});
        },
        std::chrono::steady_clock::now() + config_.cancellation_grace) &&
        *result;
}

bool SidecarRuntime::seed_route_relationship_for_test(
    std::string host, uint32_t cache_port, FStoreGuid guid,
    uint64_t generation, ProfileId profile) noexcept {
    auto result = std::make_shared<bool>(false);
    return owner_round_trip(
        [this, host = std::move(host), cache_port, guid, generation,
         profile, result] {
            if (!route_owner_)
                return;
            const RouteEndpointKey endpoint{host, cache_port};
            const RouteStoreIdentity identity{guid, generation};
            if (!bind_route_endpoint_identity(endpoint, identity))
                return;
            *result = route_owner_->seed_relationship_for_test(
                P50RouteRelationship{config_.c_store_guid, guid, generation,
                                     profile});
        },
        std::chrono::steady_clock::now() + config_.cancellation_grace) &&
        *result;
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
    bool expected = false;
    if (stop_requested_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
        const int64_t now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        stop_requested_at_ns_.store(now_ns == 0 ? 1 : now_ns,
                                    std::memory_order_release);
    }
    source_setup_cancelled_->store(true, std::memory_order_release);
    source_admission_changed_.notify_all();
    cancel_active_control();
    cancel_endpoint_incarnation();
    // Whole-incarnation teardown of dedicated F-session connections: posted to
    // the owner executor (never a cross-thread socket mutation for an ordinary
    // per-operation cancel; this is the exact incarnation-failure authority).
    try {
        asio::post(context_, [this] {
            boost::system::error_code ignored;
            p51_reservation_sweep_timer_.cancel(ignored);
            if (route_owner_)
                route_owner_->cancel_active_p51_transfers();
            std::vector<std::shared_ptr<FSessionPump>> pumps = fsession_pumps_;
            for (auto& pump : pumps)
                fsession_close(pump);
            std::vector<std::shared_ptr<P51TransferReplyPump>> replies =
                p51_transfer_reply_pumps_;
            for (auto& reply : replies)
                close_p51_transfer_reply(reply);
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
        append_fingerprint_test_trace("fingerprint completed\n");
        break;
    case P29FingerprintOutcome::Unavailable:
        append_fingerprint_test_trace("fingerprint unavailable\n");
        break;
    case P29FingerprintOutcome::TimedOut:
        append_fingerprint_test_trace("fingerprint timed-out\n");
        break;
    case P29FingerprintOutcome::Cancelled:
        append_fingerprint_test_trace("fingerprint cancelled\n");
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
