#include "p50_cache_service.h"

#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <signal.h>
#include <grp.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <utility>

namespace icecc::p50::service {
namespace {

constexpr std::string_view kReadyEnvironment = "ICECC_CACHE_SERVICE_READY_FD";
constexpr std::string_view kReadyMessage = "READY\n";
constexpr int kPollMilliseconds = 100;
constexpr int kHandshakeMilliseconds = 500;
constexpr int kMaxBacklog = 16;

volatile sig_atomic_t g_stop_requested = 0;

void request_stop(int) noexcept { g_stop_requested = 1; }

struct SignalGuard {
    struct sigaction old_term{};
    struct sigaction old_int{};
    bool installed = false;

    SignalGuard() = default;

    bool install() noexcept {
        struct sigaction action{};
        action.sa_handler = request_stop;
        if (::sigemptyset(&action.sa_mask) != 0)
            return false;
        // Deliberately omit SA_RESTART so poll/read wake for graceful stop.
        if (::sigaction(SIGTERM, &action, &old_term) != 0)
            return false;
        if (::sigaction(SIGINT, &action, &old_int) != 0) {
            (void)::sigaction(SIGTERM, &old_term, nullptr);
            return false;
        }
        installed = true;
        return true;
    }

    ~SignalGuard() {
        if (installed) {
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

bool next_value(int argc, char* const argv[], int& index, std::string_view& value) noexcept {
    if (index + 1 >= argc || argv[index + 1] == nullptr)
        return false;
    value = argv[++index];
    return !value.empty();
}

bool parse_option_uint(std::string_view name, std::string_view value, uint64_t& target) noexcept {
    if (name == "--generation" || name == "--attempt" || name == "--peer-uid" ||
        name == "--peer-gid" || name == "--expected-uid" || name == "--expected-gid" ||
        name == "--drop-uid" || name == "--drop-gid")
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
    ready.fd = fd;
    return true;
}

bool write_exact(int fd, std::string_view bytes) noexcept {
    size_t written = 0;
    while (written != bytes.size()) {
        const ssize_t result = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (result > 0) {
            written += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
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

void cleanup_listener(int fd, const std::string& path, const ListenerIdentity& identity) noexcept {
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

bool handle_connection(local::Connection connection, const Options& options) noexcept {
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
    const local::Frame ack = local::make_hello_ack(local::PeerRole::Sidecar, options.identity);
    (void)connection.send(ack);
    return true;
}

} // namespace

bool parse_options(int argc, char* const argv[], Options& options, bool& show_help) noexcept {
    show_help = false;
    try {
        options = Options{};
        bool have_socket = false;
        bool have_generation = false;
        bool have_attempt = false;
        bool have_uid = false;
        bool have_gid = false;
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
            if (name == "--generation" || name == "--attempt" || name == "--peer-uid" ||
                name == "--peer-gid" || name == "--expected-uid" || name == "--expected-gid" ||
                name == "--drop-uid" || name == "--drop-gid" || name == "--backlog") {
                if (!next_value(argc, argv, index, value))
                    return false;
                uint64_t parsed = 0;
                if (name == "--backlog") {
                    if (!parse_int(value, options.backlog) || options.backlog < 1 ||
                        options.backlog > kMaxBacklog)
                        return false;
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
    OwnedFd ready;
    if (!parse_ready_fd(ready))
        return 2;
    SignalGuard signals;
    if (!signals.install())
        return 2;
    g_stop_requested = 0;

    if (!drop_and_prove(options))
        return 2;

    local::Status listen_status = local::Status::Ok;
    const int listener = local::listen_unix(options.socket_path, options.backlog, &listen_status);
    if (listener < 0)
        return 2;
    OwnedFd listener_owner(listener);
    ListenerIdentity identity{};
    if (!capture_listener_identity(listener, options.socket_path, identity)) {
        cleanup_listener(listener, options.socket_path, identity);
        return 2;
    }
    if (g_stop_requested != 0 || !write_exact(ready.fd, kReadyMessage)) {
        cleanup_listener(listener, options.socket_path, identity);
        return 2;
    }
    (void)::close(ready.fd);
    ready.fd = -1;

    while (g_stop_requested == 0) {
        struct pollfd descriptor{listener, POLLIN | POLLERR | POLLHUP, 0};
        const int result = ::poll(&descriptor, 1, kPollMilliseconds);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (result == 0 || (descriptor.revents & POLLIN) == 0)
            continue;
        local::Status accept_status = local::Status::Ok;
        local::Connection connection = local::accept_unix(listener, &accept_status);
        if (connection.valid())
            (void)handle_connection(std::move(connection), options);
    }

    cleanup_listener(listener, options.socket_path, identity);
    return 0;
}

} // namespace icecc::p50::service

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
