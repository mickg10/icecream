/* Real iceccd ordinary-link fail-closed integration.
 * A Protocol-50 CACHE_SESSION sent to the public daemon listener must be
 * classified by the production event loop and closed while no sidecar
 * relationship is available.  Login advertisement is not involved. */
#include "comm.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <cstdint>
#include <poll.h>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <pwd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;

static int failures = 0;
#define REQUIRE(condition, text) do { \
    if (condition) std::fprintf(stderr, "ok - %s\n", text); \
    else { std::fprintf(stderr, "FAILED - %s\n", text); ++failures; } \
} while (0)

static MsgChannel *connect_unix_bounded(const std::string &path, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        if (::access(path.c_str(), F_OK) == 0) {
            MsgChannel *channel = Service::createChannel(path);
            if (channel) return channel;
        }
        ::usleep(20000);
    }
    return nullptr;
}

static bool wait_eof(MsgChannel *channel, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        Msg *msg = channel->get_msg(1, true);
        delete msg;
        if (channel->at_eof()) return true;
    }
    return channel && channel->at_eof();
}

static bool raw_io_exact_until(int fd, unsigned char *bytes, size_t length,
                               bool writing, Clock::time_point deadline)
{
    size_t offset = 0;
    while (offset != length) {
        const auto now = Clock::now();
        if (now >= deadline)
            return false;
        const int timeout = static_cast<int>(std::clamp<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count(), 0, INT_MAX));
        pollfd descriptor{fd, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        const int ready = ::poll(&descriptor, 1, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || (descriptor.revents & POLLNVAL) != 0)
            return false;
        if (writing &&
            (descriptor.revents & (POLLERR | POLLHUP)) != 0)
            return false;
        if (!writing &&
            (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0)
            continue;
        const ssize_t count = writing
            ? ::send(fd, bytes + offset, length - offset,
                     MSG_DONTWAIT | MSG_NOSIGNAL)
            : ::recv(fd, bytes + offset, length - offset, MSG_DONTWAIT);
        if (count > 0) {
            offset += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN ||
                          errno == EWOULDBLOCK))
            continue;
        return false;
    }
    return true;
}

static uint32_t protocol_word(const std::array<unsigned char, 4> &bytes)
{
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

struct Protocol50DispatchProbe {
    uint32_t server_offer = 0;
    uint32_t selected_protocol = 0;
    bool handshake_complete = false;
    bool cache_session_sent = false;
    bool peer_closed = false;
};

static Protocol50DispatchProbe send_protocol50_cache_session(
    const std::string &socket_path)
{
    Protocol50DispatchProbe result;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                            0);
    if (fd < 0)
        return result;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return result;
    }
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    const int connected = ::connect(
        fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (connected != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
            ::close(fd);
            return result;
        }
        for (;;) {
            const auto now = Clock::now();
            if (now >= deadline) {
                ::close(fd);
                return result;
            }
            pollfd descriptor{fd, POLLOUT, 0};
            const int timeout = static_cast<int>(std::clamp<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now).count(), 0, INT_MAX));
            const int ready = ::poll(&descriptor, 1, timeout);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready <= 0 ||
                (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                ::close(fd);
                return result;
            }
            if ((descriptor.revents & POLLOUT) == 0)
                continue;
            int connect_error = 0;
            socklen_t connect_error_size = sizeof(connect_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &connect_error,
                             &connect_error_size) != 0 || connect_error != 0) {
                ::close(fd);
                return result;
            }
            break;
        }
    }

    std::array<unsigned char, 4> offer{};
    std::array<unsigned char, 4> client_max{50, 0, 0, 0};
    if (!raw_io_exact_until(fd, offer.data(), offer.size(), false, deadline)) {
        ::close(fd);
        return result;
    }
    result.server_offer = protocol_word(offer);
    if (!raw_io_exact_until(fd, client_max.data(), client_max.size(), true,
                            deadline)) {
        ::close(fd);
        return result;
    }

    std::array<unsigned char, 4> selected{};
    if (!raw_io_exact_until(fd, selected.data(), selected.size(), false, deadline)) {
        ::close(fd);
        return result;
    }
    result.selected_protocol = protocol_word(selected);
    if (result.selected_protocol != 50 || result.server_offer < 50) {
        ::close(fd);
        return result;
    }
    if (!raw_io_exact_until(fd, selected.data(), selected.size(), true, deadline)) {
        ::close(fd);
        return result;
    }
    result.handshake_complete = true;

    uint32_t frame_length = htonl(sizeof(uint32_t));
    uint32_t message_type = htonl(static_cast<uint32_t>(Msg::CACHE_SESSION));
    std::array<unsigned char, 8> frame{};
    std::memcpy(frame.data(), &frame_length, sizeof(frame_length));
    std::memcpy(frame.data() + sizeof(frame_length), &message_type,
                sizeof(message_type));
    result.cache_session_sent = raw_io_exact_until(
        fd, frame.data(), frame.size(), true, deadline);

    if (result.cache_session_sent) {
        size_t response_bytes = 0;
        while (Clock::now() < deadline) {
            pollfd descriptor{fd, static_cast<short>(POLLIN | POLLHUP), 0};
            const int timeout = static_cast<int>(std::clamp<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - Clock::now()).count(), 0, INT_MAX));
            const int ready = ::poll(&descriptor, 1, timeout);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready <= 0 || (descriptor.revents & POLLNVAL) != 0)
                break;
            unsigned char response[256];
            const ssize_t count = ::recv(fd, response, sizeof(response),
                                         MSG_DONTWAIT);
            if (count == 0) {
                result.peer_closed = true;
                break;
            }
            if (count > 0) {
                response_bytes += static_cast<size_t>(count);
                if (response_bytes > 4096)
                    break;
                continue;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
    }
    ::close(fd);
    return result;
}

static int reserve_port()
{
    int port = 0;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0) {
        ::close(listener);
        return 0;
    }
    socklen_t length = sizeof(address);
    const bool ok = ::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0;
    port = ok ? ntohs(address.sin_port) : 0;
    ::close(listener);
    return port;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <iceccd>\n", argv[0]);
        return 2;
    }
    const char *temporary_root = ::getenv("TMPDIR");
    const std::string prefix = temporary_root && *temporary_root
        ? temporary_root : "/tmp";
    std::string pattern = prefix + "/icecc-s2-daemon-cache.XXXXXX";
    std::vector<char> work_template(pattern.begin(), pattern.end());
    work_template.push_back('\0');
    char *work_raw = ::mkdtemp(work_template.data());
    REQUIRE(work_raw != nullptr, "real daemon temporary directory created");
    if (!work_raw) return 2;
    const std::string work(work_raw);
    const std::string socket_path = work + "/iceccd.sock";
    const std::string envdir = work + "/envs";
    const std::string log_path = work + "/iceccd.log";
    if (::mkdir(envdir.c_str(), 0700) != 0) return 2;
    std::string daemon_user;
    if (::getuid() == 0) {
        const passwd *account = ::getpwnam("nobody");
        if (!account || account->pw_uid == 0 || account->pw_gid == 0) return 2;
        if (::chown(work.c_str(), account->pw_uid, account->pw_gid) != 0 ||
            ::chown(envdir.c_str(), account->pw_uid, account->pw_gid) != 0) return 2;
        daemon_user = "nobody";
    }
    const int scheduler_port = reserve_port();
    REQUIRE(scheduler_port > 0, "real daemon scheduler retry port reserved");

    const pid_t child = ::fork();
    if (child == 0) {
        char scheduler[64];
        std::snprintf(scheduler, sizeof(scheduler), "127.0.0.1:%d", scheduler_port);
        ::setenv("ICECC_TESTS", "1", 1);
        ::setenv("ICECC_TEST_SOCKET", socket_path.c_str(), 1);
        std::vector<std::string> arguments {
            argv[1], "--no-remote", "-m", "0", "-p", "10245", "-s", scheduler,
            "-n", "s2-cache-dispatch", "-N", "s2-cache-daemon", "-b", envdir,
            "-l", log_path
        };
        if (!daemon_user.empty()) arguments.insert(arguments.end(), {"-u", daemon_user});
        std::vector<char *> pointers;
        for (auto &argument : arguments) pointers.push_back(argument.data());
        pointers.push_back(nullptr);
        ::execv(argv[1], pointers.data());
        ::_exit(127);
    }
    REQUIRE(child > 0, "real iceccd process started");
    if (child <= 0) return 2;

    MsgChannel *client = connect_unix_bounded(socket_path, 10000);
    REQUIRE(client != nullptr, "client reached the real public iceccd listener");
    if (client) {
        REQUIRE(client->protocol == 51,
                "ordinary link negotiated the current Protocol-51 maximum");
        // The production dispatcher is unavailable until a reviewed READY
        // adapter supplies an authenticated sidecar, so this must terminate
        // through the normal client teardown and never advertise cache.
        REQUIRE(client->send_msg(CacheSessionMsg()),
                "real client sent Protocol-50 CACHE_SESSION");
        REQUIRE(wait_eof(client, 5000),
                "real iceccd classified CACHE_SESSION and failed closed boundedly");
        delete client;
    }

    const Protocol50DispatchProbe legacy50 =
        send_protocol50_cache_session(socket_path);
    REQUIRE(legacy50.handshake_complete && legacy50.server_offer >= 50 &&
                legacy50.selected_protocol == 50,
            "raw legacy client negotiated exactly Protocol-50 with current iceccd");
    REQUIRE(legacy50.cache_session_sent,
            "raw Protocol-50 client sent the exact CACHE_SESSION frame");
    REQUIRE(legacy50.peer_closed,
            "real iceccd classified legacy Protocol-50 CACHE_SESSION and failed closed boundedly");

    ::kill(child, SIGTERM);
    int status = 0;
    bool reaped = false;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        const pid_t got = ::waitpid(child, &status, WNOHANG);
        if (got == child) { reaped = true; break; }
        if (got < 0) break;
        ::usleep(20000);
    }
    if (!reaped) {
        ::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
    }
    REQUIRE(reaped, "real iceccd exited under bounded shutdown");
    return failures ? 1 : 0;
}
