/*
 * Real positive iceccd/sidecar/READY integration gate.
 *
 * This is intentionally a process test, not another Controller unit test.  A
 * fake scheduler activates one real iceccd; the daemon must start the actual
 * icecc-cache-service, publish a positive Login only after READY, hand one
 * Protocol-50 CACHE_SESSION to it, publish the one-shot absent->present pair,
 * and withdraw before orderly scheduler teardown.
 */
#include "config.h"
#include "comm.h"
#include "../cache/p50_incarnation_identity.h"
#include "../cache/p50_daemon_control.h"
#include "../cache/p50_control_operation.h"
#include "../cache/p50_endpoint.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <future>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

#if defined(HAVE_LIBCAP_NG)
static int failures = 0;
#define REQUIRE(condition, text) do { \
    if (condition) std::fprintf(stderr, "ok - %s\n", text); \
    else { std::fprintf(stderr, "FAILED - %s\n", text); ++failures; } \
} while (0)

static int listen_ephemeral(int *port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
            || ::listen(fd, 8) != 0) {
        ::close(fd);
        return -1;
    }
    if (const char *bytes = ::getenv("ICECC_TEST_SCHEDULER_RCVBUF");
        bytes != nullptr && *bytes != '\0') {
        const int value = std::atoi(bytes);
        if (value > 0)
            (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
    }
    socklen_t size = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
        ::close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

static int reserve_port()
{
    int port = 0;
    const int fd = listen_ephemeral(&port);
    if (fd >= 0) ::close(fd);
    return port;
}

static MsgChannel *accept_channel(int listener, int timeout_msec)
{
    pollfd descriptor{listener, POLLIN, 0};
    if (::poll(&descriptor, 1, timeout_msec) <= 0) return nullptr;
    const int fd = ::accept(listener, nullptr, nullptr);
    if (fd < 0) return nullptr;
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    return Service::createChannel(fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
}

static Msg *wait_for_type(MsgChannel *channel, Msg::Value type, int timeout_msec)
{
    if (!channel) return nullptr;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        Msg *message = channel->get_msg(1, true);
        if (message != nullptr) {
            if (*message == type) return message;
            delete message;
        }
        if (channel->at_eof()) return nullptr;
    }
    return nullptr;
}

static MsgChannel *connect_tcp_bounded(int port, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return nullptr;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
            return Service::createChannel(fd, reinterpret_cast<sockaddr *>(&address),
                                          sizeof(address));
        ::close(fd);
        ::usleep(20000);
    }
    return nullptr;
}

static int connect_raw_tcp(int port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    const int connected = ::connect(
        fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (connected < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    if (connected < 0) {
        pollfd waiter{fd, POLLOUT, 0};
        const auto connect_deadline = Clock::now() + std::chrono::milliseconds(250);
        int ready = -1;
        while (true) {
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(connect_deadline - Clock::now()).count();
            if (remaining <= 0) break;
            ready = ::poll(&waiter, 1, static_cast<int>(remaining));
            if (ready >= 0 || errno != EINTR) break;
        }
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (ready <= 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                       &socket_error, &socket_error_size) < 0 ||
            socket_error != 0) {
            ::close(fd);
            return -1;
        }
    }
    return fd;
}

static bool write_all(int fd, const void *data, size_t size)
{
    const char *position = static_cast<const char *>(data);
    while (size != 0) {
        const ssize_t written = ::send(fd, position, size, MSG_NOSIGNAL);
        if (written > 0) {
            position += written;
            size -= static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool run_iptables_rule(const std::vector<std::string>& arguments)
{
    const pid_t child = ::fork();
    if (child < 0) return false;
    if (child == 0) {
        std::vector<char *> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char *>("iptables"));
        for (const auto &argument : arguments)
            argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

class P51CommitReceiptGate {
public:
    P51CommitReceiptGate(int endpoint_port, uid_t sidecar_uid, size_t expected)
        : endpoint_port_(endpoint_port), sidecar_uid_(sidecar_uid), expected_(expected)
    {
        listener_fd_ = listen_ephemeral(&proxy_port_);
        if (listener_fd_ < 0 || proxy_port_ <= 0) return;
        const auto rule = [&](const char *action) {
            return run_iptables_rule({"-t", "nat", action, "OUTPUT", "-p", "tcp",
                "-d", "127.0.0.1", "--dport", std::to_string(endpoint_port_),
                "-m", "owner", "--uid-owner", std::to_string(sidecar_uid_),
                "-j", "REDIRECT", "--to-ports", std::to_string(proxy_port_)});
        };
        rule_installed_ = rule("-A");
        if (rule_installed_) thread_ = std::thread([this] {
            try { run(); }
            catch (...) { fail(); }
        });
    }

    P51CommitReceiptGate(const P51CommitReceiptGate&) = delete;
    P51CommitReceiptGate& operator=(const P51CommitReceiptGate&) = delete;

    ~P51CommitReceiptGate()
    {
        stop_.store(true, std::memory_order_release);
        {
            std::lock_guard lock(mutex_);
            release_ = true;
        }
        changed_.notify_all();
        if (thread_.joinable()) thread_.join();
        if (client_fd_ >= 0) { ::close(client_fd_); client_fd_ = -1; }
        if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
        if (listener_fd_ >= 0) { ::close(listener_fd_); listener_fd_ = -1; }
        if (rule_installed_)
            (void)run_iptables_rule({"-t", "nat", "-D", "OUTPUT", "-p", "tcp",
                "-d", "127.0.0.1", "--dport", std::to_string(endpoint_port_),
                "-m", "owner", "--uid-owner", std::to_string(sidecar_uid_),
                "-j", "REDIRECT", "--to-ports", std::to_string(proxy_port_)});
    }

    bool ready() const noexcept { return listener_fd_ >= 0 && rule_installed_; }
    int proxy_port() const noexcept { return proxy_port_; }
    size_t observed_commits() const
    {
        std::lock_guard lock(mutex_);
        return peak_commits_;
    }

    bool wait_for_commits(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] {
            return failed_ || commits_.size() >= expected_;
        }) && !failed_ && commits_.size() == expected_ &&
            ordinals_.size() == expected_ && *ordinals_.begin() == 1 &&
            *ordinals_.rbegin() == expected_;
    }

    void release_commits()
    {
        {
            std::lock_guard lock(mutex_);
            release_ = true;
        }
        changed_.notify_all();
    }

private:
    bool write_relay_bytes(int fd, const void *buffer, size_t size)
    {
        const auto *position = static_cast<const unsigned char *>(buffer);
        while (size != 0 && !stop_.load(std::memory_order_acquire)) {
            pollfd descriptor{fd, POLLOUT, 0};
            const int ready = ::poll(&descriptor, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (ready > 0 &&
                (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))))
                return false;
            if (ready == 0) continue;
            const ssize_t count = ::send(fd, position, size,
                                         MSG_NOSIGNAL | MSG_DONTWAIT);
            if (count > 0) {
                position += count;
                size -= static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            return false;
        }
        return size == 0;
    }

    bool read_relay_bytes(int fd, void *buffer, size_t size)
    {
        auto *position = static_cast<unsigned char *>(buffer);
        while (size != 0 && !stop_.load(std::memory_order_acquire)) {
            pollfd descriptor{fd, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (ready > 0 &&
                (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))))
                return false;
            if (ready == 0) continue;
            const ssize_t count = ::recv(fd, position, size, 0);
            if (count > 0) {
                position += count;
                size -= static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            return false;
        }
        return size == 0;
    }

    void fail()
    {
        {
            std::lock_guard lock(mutex_);
            failed_ = true;
        }
        changed_.notify_all();
    }

    void run()
    {
        pollfd listener{listener_fd_, POLLIN, 0};
        int ready = 0;
        while (!stop_.load(std::memory_order_acquire)) {
            ready = ::poll(&listener, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready != 0) break;
        }
        if (stop_.load(std::memory_order_acquire) || ready <= 0) { fail(); return; }
        client_fd_ = ::accept(listener_fd_, nullptr, nullptr);
        if (client_fd_ < 0) { fail(); return; }
        server_fd_ = connect_raw_tcp(endpoint_port_);
        if (server_fd_ < 0) { fail(); return; }
        std::thread client_to_server([this] {
            char bytes[8192];
            for (;;) {
                if (stop_.load(std::memory_order_acquire)) break;
                pollfd descriptor{client_fd_, POLLIN, 0};
                const int ready = ::poll(&descriptor, 1, 100);
                if (ready < 0 && errno == EINTR) continue;
                if (ready < 0 || (ready > 0 &&
                    (descriptor.revents & (POLLERR | POLLNVAL)))) break;
                if (ready == 0) continue;
                const ssize_t count = ::recv(client_fd_, bytes, sizeof(bytes), 0);
                if (count > 0) {
                    if (!write_relay_bytes(server_fd_, bytes,
                                           static_cast<size_t>(count))) break;
                    continue;
                }
                if (count < 0 && errno == EINTR) continue;
                break;
            }
            ::shutdown(server_fd_, SHUT_WR);
        });

        // Forward the negotiated ordinary-protocol word, then detect R2 by
        // its first LINK_STATE frame.  Ordinary MsgChannel frame lengths stay
        // below 1 MiB and therefore cannot alias an R2 type byte.
        unsigned char header[4];
        if (!read_relay_bytes(server_fd_, header, sizeof(header)) ||
            !write_relay_bytes(client_fd_, header, sizeof(header))) {
            fail();
        } else {
            // MsgChannel exchanges both the peer's maximum version and the
            // selected version in each direction before ordinary frames.
            if (!read_relay_bytes(server_fd_, header, sizeof(header)) ||
                !(header[0] == 51 && header[1] == 0 &&
                  header[2] == 0 && header[3] == 0) ||
                !write_relay_bytes(client_fd_, header, sizeof(header))) {
                fail();
            }
            bool r2 = false;
            while (!stop_.load(std::memory_order_acquire) && !failed_) {
                if (!read_relay_bytes(server_fd_, header, sizeof(header))) break;
                const uint32_t word = (uint32_t(header[0]) << 24) |
                    (uint32_t(header[1]) << 16) |
                    (uint32_t(header[2]) << 8) | uint32_t(header[3]);
                const uint8_t type_byte = header[0];
                if (!r2 && type_byte == static_cast<uint8_t>(
                        icecc::p50::MessageType::LINK_STATE))
                    r2 = true;
                uint32_t payload_bytes = word;
                if (r2) {
                    try {
                        payload_bytes = icecc::p50::decode_frame_header(
                            std::span<const uint8_t>(header, sizeof(header))).payload_bytes;
                    } catch (...) { fail(); break; }
                } else if (payload_bytes > (1u << 20)) { fail(); break; }
                std::vector<uint8_t> frame(header, header + sizeof(header));
                const size_t payload_offset = frame.size();
                frame.resize(payload_offset + payload_bytes);
                if (payload_bytes != 0 && !read_relay_bytes(
                        server_fd_, frame.data() + payload_offset, payload_bytes))
                    break;
                if (r2 && type_byte == static_cast<uint8_t>(
                        icecc::p50::MessageType::R2_TX_COMMIT)) {
                    try {
                        const auto decoded = icecc::p50::decode_payload(
                            icecc::p50::MessageType::R2_TX_COMMIT,
                            std::span<const uint8_t>(frame.data() + 4,
                                                     payload_bytes));
                        const auto& commit = std::get<icecc::p50::R2TxCommit>(decoded);
                        std::unique_lock lock(mutex_);
                        if (!ordinals_.insert(commit.relationship_ordinal).second) {
                            failed_ = true;
                            changed_.notify_all();
                            break;
                        }
                        commits_.push_back(std::move(frame));
                        peak_commits_ = std::max(peak_commits_, commits_.size());
                        changed_.notify_all();
                        if (commits_.size() == expected_) {
                            if (!changed_.wait_for(lock, std::chrono::seconds(30),
                                                   [&] { return release_ || failed_; }) ||
                                failed_) break;
                            for (const auto& held : commits_) {
                                if (!write_relay_bytes(
                                        client_fd_, held.data(), held.size())) {
                                    failed_ = true;
                                    break;
                                }
                            }
                            commits_.clear();
                            changed_.notify_all();
                            if (failed_) break;
                        }
                    } catch (...) { fail(); break; }
                } else if (!write_relay_bytes(
                               client_fd_, frame.data(), frame.size())) {
                    break;
                }
            }
        }
        stop_.store(true, std::memory_order_release);
        ::shutdown(client_fd_, SHUT_WR);
        ::shutdown(server_fd_, SHUT_RDWR);
        if (client_to_server.joinable()) client_to_server.join();
        fail();
    }

    int endpoint_port_ = 0;
    uid_t sidecar_uid_ = 0;
    size_t expected_ = 0;
    int proxy_port_ = 0;
    int listener_fd_ = -1;
    int client_fd_ = -1;
    int server_fd_ = -1;
    bool rule_installed_ = false;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::vector<uint8_t>> commits_;
    std::set<uint64_t> ordinals_;
    size_t peak_commits_ = 0;
    bool release_ = false;
    bool failed_ = false;
    std::atomic<bool> stop_{false};
};

/* Queue a complete protocol-50 negotiation plus an ordinary CACHE_SESSION
   frame while the daemon process is stopped.  The listener can then resume
   with the entire burst already resident in its kernel accept queue, making
   admission-versus-message-handling order deterministic rather than a client
   scheduling race. */
static int connect_preframed_cache_session(int port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }

    unsigned char wire[16]{};
    wire[0] = static_cast<unsigned char>(PROTOCOL_VERSION);
    wire[4] = static_cast<unsigned char>(PROTOCOL_VERSION);
    const uint32_t frame_size = htonl(sizeof(uint32_t));
    const uint32_t message = htonl(static_cast<uint32_t>(Msg::CACHE_SESSION));
    std::memcpy(wire + 8, &frame_size, sizeof(frame_size));
    std::memcpy(wire + 12, &message, sizeof(message));
    if (!write_all(fd, wire, sizeof(wire))) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static bool wait_raw_eof(int fd, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    char buffer[64];
    while (Clock::now() < deadline) {
        pollfd descriptor{fd, POLLIN | POLLHUP | POLLERR, 0};
        const int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count());
        int ready = -1;
        do {
            ready = ::poll(&descriptor, 1, remaining > 0 ? remaining : 0);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0) return false;
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count == 0) return true;
        if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return false;
    }
    return false;
}

static std::string read_file_suffix(const std::string& path, uintmax_t offset)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    stream.seekg(static_cast<std::streamoff>(offset));
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

static size_t count_text(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

static bool wait_eof(MsgChannel *channel, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (channel != nullptr && Clock::now() < deadline) {
        Msg *message = channel->get_msg(1, true);
        delete message;
        if (channel->at_eof()) return true;
    }
    return channel != nullptr && channel->at_eof();
}

static bool no_terminal_socket_event(MsgChannel *channel, int timeout_msec)
{
    if (channel == nullptr || channel->fd < 0)
        return false;
    pollfd descriptor{channel->fd, 0, 0};
    int result = -1;
    do {
        result = ::poll(&descriptor, 1, timeout_msec);
    } while (result < 0 && errno == EINTR);
    return result == 0 ||
           (result > 0 &&
            (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) == 0);
}

static int process_fd_count(pid_t pid)
{
    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::path("/proc") / std::to_string(pid) / "fd";
    std::filesystem::directory_iterator position(directory, error);
    const std::filesystem::directory_iterator end;
    if (error) return -1;
    int count = 0;
    while (position != end) {
        ++count;
        position.increment(error);
        if (error) return -1;
    }
    return count;
}

static int wait_for_fd_count_at_most(pid_t pid, int maximum, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    int observed = process_fd_count(pid);
    while (observed > maximum && Clock::now() < deadline) {
        ::usleep(20000);
        observed = process_fd_count(pid);
    }
    return observed;
}

static bool wait_child(pid_t pid, int timeout_msec, int *status)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        const pid_t result = ::waitpid(pid, status, WNOHANG);
        if (result == pid) return true;
        if (result < 0) return false;
        ::usleep(20000);
    }
    return false;
}

static bool absent(const LoginMsg *login)
{
    return login != nullptr && login->cache_endpoint_port == 0
        && login->cache_protocol == 0 && login->cache_profile_mask == 0;
}

static bool present_revision(const LoginMsg *login, uint32_t port,
                             uint32_t revision)
{
    return login != nullptr && login->cache_endpoint_port == port
        && login->cache_protocol == revision
        && login->cache_profile_mask == CACHE_ADVERTISABLE_PROFILE_MASK;
}

static P50SourceArmFields source_arm(uint32_t wire_id, uint64_t epoch,
                                     uint64_t nonce, uint32_t daemon_port,
                                     uint32_t cache_port)
{
    P50SourceArmFields arm;
    arm.wire_job_id = wire_id;
    arm.assignment_epoch = epoch;
    arm.assignment_nonce = nonce;
    arm.selected_f_host = "127.0.0.1";
    arm.selected_f_ordinary_port = daemon_port;
    arm.selected_f_cache_port = cache_port;
    arm.cache_protocol = CACHE_WIRE_REVISION;
    arm.cache_profile = CACHE_PROFILE_ZSTD_TU;
    arm.logical_job = wire_id;
    arm.compiler_attempt = nonce;
    arm.c_store_generation = 1;
    arm.c_store_derivation_version =
        icecc::p50::kStoreIdentityDerivationVersion;
    arm.c_store_guid[0] = 0x11;
    arm.c_store_guid[1] = 0x22;
    arm.source_request_id = nonce;
    arm.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    arm.c_control_generation = 1;
    arm.c_control_attempt = nonce;
    return arm;
}

static pid_t find_attachment_sidecar(pid_t daemon_pid, const char *service_path)
{
    const std::string wanted = service_path ? service_path : "";
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/proc", error)) {
        if (error) break;
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::ifstream stat(entry.path() / "stat");
        pid_t pid = -1, ppid = -1;
        char comm[256]{}, state = 0;
        if (!(stat >> pid >> comm >> state >> ppid) || ppid != daemon_pid) continue;
        std::ifstream cmd(entry.path() / "cmdline", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(cmd)),
                                std::istreambuf_iterator<char>());
        if (!wanted.empty() && bytes.find(wanted) != std::string::npos) return pid;
    }
    return -1;
}

static bool wait_attachment_log(const std::string &path, uintmax_t offset,
                                const std::string &marker, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        std::ifstream stream(path, std::ios::binary);
        stream.seekg(static_cast<std::streamoff>(offset));
        const std::string suffix((std::istreambuf_iterator<char>(stream)),
                                 std::istreambuf_iterator<char>());
        if (suffix.find(marker) != std::string::npos) return true;
        ::usleep(10000);
    }
    return false;
}

static CompileJob attachment_compile_job(uint32_t wire_id, uint64_t epoch,
                                         uint64_t nonce,
                                         const P50SourceArmFields &arm,
                                         const icecc::p50::local::P50SourceTransferResult *published)
{
    CompileJob job;
    job.setLanguage(CompileJob::Lang_CXX);
    job.setJobID(wire_id);
    job.setAssignmentIdentity(epoch, nonce);
    job.setCompileIdentity(31, 42);
    job.setEnvironmentVersion("env");
    job.setTargetPlatform("x86_64");
    job.setCompilerName("g++");
    job.setInputFile("in.ii");
    job.setWorkingDirectory("/tmp");
    job.setOutputFile("out.o");
    job.appendFlag("-O2", Arg_Remote);
    CompileInputIdentity input;
    input.profile = CompileInputIdentity::ZstdTuProfile;
    input.c_store_guid = published ? published->c_store_guid.bytes : arm.c_store_guid;
    input.tu_seq = published ? published->tu_seq : 0;
    input.raw_bytes = published ? published->raw_bytes : 1;
    if (published)
        input.raw_digest = published->raw_digest.bytes;
    else
        input.raw_digest.fill(0xa5);
    input.attempt_id = arm.compiler_attempt;
    input.request_id = arm.source_request_id;
    job.setCompileInputIdentity(input);
    return job;
}

static pid_t launch_vertical_daemon(const char *daemon_binary,
                                    const char *cache_service,
                                    const std::string& socket_path,
                                    const std::string& envdir,
                                    const std::string& runtime,
                                    const std::string& log,
                                    int scheduler_port, int public_port,
                                    const char *hostname, unsigned max_jobs)
{
    const pid_t child = ::fork();
    if (child != 0) return child;
    char scheduler[64];
    char public_port_text[16];
    char max_jobs_text[16];
    std::snprintf(scheduler, sizeof(scheduler), "127.0.0.1:%d", scheduler_port);
    std::snprintf(public_port_text, sizeof(public_port_text), "%d", public_port);
    std::snprintf(max_jobs_text, sizeof(max_jobs_text), "%u", max_jobs);
    ::setenv("ICECC_TESTS", "1", 1);
    ::setenv("ICECC_P51_MODE", "on", 1);
    ::setenv("ICECC_TEST_SOCKET", socket_path.c_str(), 1);
    ::execl(daemon_binary, daemon_binary, "-p", public_port_text, "-m", max_jobs_text,
            "-s", scheduler, "-n", hostname, "-N", hostname,
            "-b", envdir.c_str(), "-l", log.c_str(),
            "--cache-service", cache_service,
            "--cache-runtime-dir", runtime.c_str(),
            "-v", "-v", "-v", static_cast<char *>(nullptr));
    ::_exit(127);
}

static int make_vertical_source_fd(const std::string& work,
                                   const std::string& bytes)
{
    const std::string path = work + "/vertical-source.XXXXXX";
    std::vector<char> mutable_path(path.begin(), path.end());
    mutable_path.push_back('\0');
    const int fd = ::mkstemp(mutable_path.data());
    if (fd < 0) return -1;
    (void)::unlink(mutable_path.data());
    const char *position = bytes.data();
    size_t remaining = bytes.size();
    bool written_all = true;
    while (remaining != 0) {
        const ssize_t written = ::write(fd, position, remaining);
        if (written > 0) {
            position += written;
            remaining -= static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        written_all = false;
        break;
    }
    if (!written_all || ::lseek(fd, 0, SEEK_SET) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static icecc::p50::local::P50SourceTransferResult execute_p51_kind8(
    MsgChannel& local, MsgChannel& compiler_channel,
    uint32_t wire_id, uint64_t epoch, uint64_t nonce,
    uint32_t f_port, int source_fd, uint32_t profile_mask)
{
    using namespace icecc::p50;
    using namespace icecc::p50::local;
    P50SourceTransferResult failed{};
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    if (!protocol_supports_cache_r2(local.protocol)) {
        ::close(source_fd);
        return failed;
    }

    P51SourceLeaseRequestFields lease_request;
    lease_request.wire_job_id = wire_id;
    lease_request.assignment_epoch = epoch;
    lease_request.assignment_nonce = nonce;
    lease_request.profile = profile_mask;
    lease_request.requested_cache_revision = CACHE_WIRE_REVISION_R2;
    lease_request.requested_window = 30;
    if (!lease_request.valid() ||
        !local.send_msg(P51SourceLeaseRequestMsg(lease_request))) {
        ::close(source_fd);
        return failed;
    }
    P51CacheControlIdentity control_identity;
    const int control_fd = local.receive_p51_cache_fd_reply(
        lease_request, control_identity, deadline);
    if (control_fd < 0 || !control_identity.valid()) {
        if (control_fd >= 0) ::close(control_fd);
        ::close(source_fd);
        return failed;
    }

    P50SourceArmFields source = source_arm(
        wire_id, epoch, nonce, f_port, f_port);
    source.cache_protocol = CACHE_WIRE_REVISION_R2;
    source.cache_profile = profile_mask;
    source.source_mode = profile_mask == CACHE_PROFILE_P29V1
        ? P50_SOURCE_MODE_P29V1
        : profile_mask == CACHE_PROFILE_ZSTD_ROUTE
            ? P50_SOURCE_MODE_ZSTD_ROUTE : P50_SOURCE_MODE_ZSTD_TU;
    source.c_store_generation = control_identity.c_store_generation;
    source.c_store_derivation_version = control_identity.derivation_version;
    source.c_store_guid = control_identity.c_store_guid;
    source.c_control_generation = control_identity.control_generation;
    source.c_control_attempt = control_identity.control_attempt;
    const P51SourceArmFields arm{source, 30};
    const P51SourceArmMsg arm_message{arm};
    if (!arm.valid() || !arm_message.valid_for_protocol(compiler_channel.protocol) ||
        !compiler_channel.send_msg(arm_message)) {
        ::close(control_fd);
        ::close(source_fd);
        return failed;
    }
    std::unique_ptr<Msg> response(compiler_channel.get_msg_until(deadline));
    const auto *armed_message =
        dynamic_cast<const P51SourceArmedMsg *>(response.get());
    if (armed_message == nullptr || !armed_message->valid_payload() ||
        !armed_message->acknowledges(arm_message) ||
        armed_message->selected_revision != CACHE_WIRE_REVISION_R2 ||
        armed_message->selected_window == 0 || armed_message->selected_window > 30) {
        std::fprintf(stderr,
            "P51 fixture: ARMED invalid offer_window=%u selected_revision=%u selected_window=%u\n",
            arm.requested_window,
            armed_message ? armed_message->selected_revision : 0,
            armed_message ? armed_message->selected_window : 0);
        ::close(control_fd);
        ::close(source_fd);
        return failed;
    }
    std::fprintf(stderr,
        "P51 fixture: ARMED offer_window=%u selected_revision=%u selected_window=%u\n",
        arm.requested_window, armed_message->selected_revision,
        armed_message->selected_window);

    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto absolute_deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        deadline, clock.clock_domain_id, clock.time_namespace_id);
    const P51SourceTransferRequest request{
        static_cast<const P51SourceArmedFields&>(*armed_message), absolute_deadline};
    const Identity identity{control_identity.control_generation,
                             control_identity.control_attempt};
    const ControlOperation operation = make_p51_source_transfer_operation(
        identity, request, source.source_request_id);
    CredentialExpectation credentials;
    credentials.uid = control_identity.peer_uid;
    credentials.gid = control_identity.peer_gid;
    DaemonControlOperation control;
    const DaemonControlStatus started = control.begin_authenticated(
        control_fd, operation, source_fd, credentials, identity, deadline,
        DaemonControlLimits{}, DaemonControlFdOwnership::Owned);
    if (started != DaemonControlStatus::InProgress)
        return failed;

    while (!control.done()) {
        const auto now = Clock::now();
        if (now >= deadline) {
            (void)control.advance(now, 0);
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        pollfd descriptor{control.native_handle(), control.desired_events(), 0};
        const int ready = ::poll(&descriptor, 1,
            static_cast<int>(std::max<int64_t>(1, remaining.count())));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) {
            (void)control.advance(Clock::now(), POLLERR);
            break;
        }
        (void)control.advance(Clock::now(),
                              ready == 0 ? short{0} : descriptor.revents);
    }
    if (control.status() != DaemonControlStatus::Complete ||
        !control.source_transfer_result().has_value())
        return failed;
    return *control.source_transfer_result();
}

// Publish the exact input into the already-ARMED F store through the native
// P50 client endpoint. This fixture is intentionally not a C-daemon lease:
// the daemon under test is the F owner and correctly refuses to source itself.
static icecc::p50::local::P50SourceTransferResult publish_input_to_armed_f(
    MsgChannel& compiler_channel, const P50SourceArmFields& arm,
    std::string_view exact_source)
{
    using namespace icecc::p50;
    using namespace icecc::p50::local;
    P50SourceTransferResult failed{};
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    const P50SourceArmMsg arm_message(arm);
    if (!arm.valid() ||
        !arm_message.valid_for_protocol(compiler_channel.protocol) ||
        !compiler_channel.send_msg(arm_message))
        return failed;
    std::unique_ptr<Msg> response(compiler_channel.get_msg_until(deadline));
    const auto* armed = dynamic_cast<const P50SourceArmedMsg*>(response.get());
    if (!armed || !armed->valid_payload() || !armed->acknowledges(arm_message))
        return failed;
    if (!compiler_channel.send_msg(CacheSessionMsg(), MsgChannel::SendNonBlocking))
        return failed;
    const int endpoint_fd = compiler_channel.release_fd_after_cache_session_ready(deadline);
    if (endpoint_fd < 0)
        return failed;

    CStoreGuid c_guid;
    c_guid.bytes = arm.c_store_guid;
    auto authority = std::make_shared<P50PreparationAuthority>(
        c_guid, ZstdTuLimits{uint64_t{64} << 20, uint64_t{2} << 30},
        PreparationAuthorityLimits{}, 1, ProfileId::ZSTD_TU, TuSeq{0});
    const PreparationRouteKey route{
        armed->f_store_guid, armed->f_store_generation, ProfileId::ZSTD_TU};
    const PreparedTuHandle prepared = authority->prepare_for_route(
        route, PrepareRequestKey{arm.logical_job, arm.source_request_id},
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(exact_source.data()), exact_source.size()));
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_TU;
    caps.supported_profiles = kOperationalProfileMask;
    P50ClientEndpoint endpoint(authority, caps, HistoryNonce{1}, nullptr, nullptr,
                               std::nullopt, {}, {}, route);
    boost::asio::io_context context;
    boost::system::error_code socket_error;
    auto socket = P50ServerEndpoint::adopt_connected_fd(
        context.get_executor(), endpoint_fd, socket_error);
    if (!socket)
        return failed;
    auto result = boost::asio::co_spawn(
        context,
        endpoint.run(std::move(*socket), prepared, {}, deadline),
        boost::asio::use_future);
    context.run();
    const ClientRunResult client_result = result.get();
    if (client_result.status != ClientRunStatus::Committed ||
        !client_result.committed_commit || !client_result.committed_input)
        return failed;
    failed.code = SourceTransferResultCode::Committed;
    failed.attempts = 1;
    failed.tu_seq = client_result.committed_input->tu_seq.value;
    failed.raw_bytes = exact_source.size();
    failed.raw_digest = icecc::digest128(exact_source);
    failed.c_store_guid = client_result.committed_input->c_store_guid;
    return failed;
}

static int run_p51_vertical(const char *daemon_binary, const char *cache_service,
                            passwd *icecc, unsigned job_count,
                            uint32_t profile_mask)
{
    ::signal(SIGPIPE, SIG_IGN);
    const char *temporary_root = ::getenv("TMPDIR");
    const std::string prefix = temporary_root && *temporary_root
        ? temporary_root : "/tmp";
    std::string pattern = prefix + "/p51vertical.XXXXXX";
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    char *created = ::mkdtemp(mutable_pattern.data());
    REQUIRE(created != nullptr, "vertical test temporary root created");
    if (created == nullptr) return 2;
    const std::string work(created);
    const std::string cdir = work + "/c";
    const std::string fdir = work + "/f";
    const auto prepare_role = [&](const std::string& directory) {
        const bool made = ::mkdir(directory.c_str(), 0700) == 0;
        const bool owned = made && ::chown(directory.c_str(), icecc->pw_uid,
                                            icecc->pw_gid) == 0;
        const bool env = owned && ::mkdir((directory + "/envs").c_str(), 0700) == 0 &&
            ::chown((directory + "/envs").c_str(), icecc->pw_uid, icecc->pw_gid) == 0;
        const bool runtime = env &&
            ::mkdir((directory + "/runtime").c_str(), 0700) == 0 &&
            ::chown((directory + "/runtime").c_str(), icecc->pw_uid, icecc->pw_gid) == 0;
        return runtime;
    };
    REQUIRE(::chown(work.c_str(), icecc->pw_uid, icecc->pw_gid) == 0 &&
                ::chmod(work.c_str(), 0700) == 0 && prepare_role(cdir) && prepare_role(fdir),
            "distinct C and F daemons have exact private runtime ownership");
    if (failures) return 2;

    int c_scheduler_port = 0, f_scheduler_port = 0;
    const int c_scheduler_listener = listen_ephemeral(&c_scheduler_port);
    const int f_scheduler_listener = listen_ephemeral(&f_scheduler_port);
    const int c_port = reserve_port(), f_port = reserve_port();
    REQUIRE(c_scheduler_listener >= 0 && f_scheduler_listener >= 0 &&
                c_port > 0 && f_port > 0 && c_port != f_port,
            "C and F use independent scheduler/client endpoints");
    if (c_scheduler_listener < 0 || f_scheduler_listener < 0 ||
        c_port <= 0 || f_port <= 0 || c_port == f_port) return 2;

    const pid_t c_pid = launch_vertical_daemon(
        daemon_binary, cache_service, cdir + "/iceccd.sock", cdir + "/envs",
        cdir + "/runtime", cdir + "/iceccd.log", c_scheduler_port, c_port,
        "p51-c", job_count + 2);
    const pid_t f_pid = launch_vertical_daemon(
        daemon_binary, cache_service, fdir + "/iceccd.sock", fdir + "/envs",
        fdir + "/runtime", fdir + "/iceccd.log", f_scheduler_port, f_port,
        "p51-f", job_count + 2);
    REQUIRE(c_pid > 0 && f_pid > 0, "distinct real C and F daemons started");
    if (c_pid <= 0 || f_pid <= 0) return 2;
    struct DaemonPairCleanup {
        pid_t c_pid;
        pid_t f_pid;
        ~DaemonPairCleanup() {
            for (pid_t pid : {c_pid, f_pid}) {
                if (pid <= 0) continue;
                (void)::kill(pid, SIGTERM);
                int status = 0;
                if (!wait_child(pid, 3000, &status)) {
                    (void)::kill(pid, SIGKILL);
                    (void)::waitpid(pid, &status, 0);
                }
            }
        }
    } daemon_cleanup{c_pid, f_pid};
    MsgChannel *c_scheduler = accept_channel(c_scheduler_listener, 10000);
    MsgChannel *f_scheduler = accept_channel(f_scheduler_listener, 10000);
    Msg *c_initial_msg = wait_for_type(c_scheduler, Msg::LOGIN, 5000);
    Msg *f_initial_msg = wait_for_type(f_scheduler, Msg::LOGIN, 5000);
    const bool initial_absent = absent(dynamic_cast<LoginMsg *>(c_initial_msg)) &&
        absent(dynamic_cast<LoginMsg *>(f_initial_msg));
    REQUIRE(initial_absent, "both independent daemons begin cache-absent");
    delete c_initial_msg;
    delete f_initial_msg;
    const uint64_t epoch = UINT64_C(0x51c0000000000001);
    const ConfCSMsg activate(epoch, ConfCSMsg::StrictNonce);
    const bool activated = c_scheduler && f_scheduler &&
        c_scheduler->send_msg(activate) && f_scheduler->send_msg(activate);
    Msg *c_login_msg = activated ? wait_for_type(c_scheduler, Msg::LOGIN, 10000) : nullptr;
    Msg *f_login_msg = activated ? wait_for_type(f_scheduler, Msg::LOGIN, 10000) : nullptr;
    const bool both_ready = present_revision(dynamic_cast<LoginMsg *>(c_login_msg),
            static_cast<uint32_t>(c_port), CACHE_WIRE_REVISION_R2) &&
        present_revision(dynamic_cast<LoginMsg *>(f_login_msg),
            static_cast<uint32_t>(f_port), CACHE_WIRE_REVISION_R2);
    REQUIRE(both_ready, "distinct C and F sidecars publish opt-in R2 READY");
    delete c_login_msg;
    delete f_login_msg;
    if (!both_ready) return 1;

    struct VerticalJob {
        uint32_t wire_id = 0;
        uint64_t nonce = 0;
        MsgChannel *wrapper = nullptr;
        MsgChannel *compiler = nullptr;
        std::string bytes;
        int source_fd = -1;
        icecc::p50::local::P50SourceTransferResult result{};
        std::atomic<bool> finished{false};
        bool assigned = false;
        bool source_ready = false;
        bool compile_sent = false;
        bool attached = false;
        bool compile_bounded = false;
        uintmax_t attach_log_offset = 0;
    };
    std::vector<VerticalJob> jobs(job_count);
    bool all_assignments_ready = true;
    for (unsigned index = 0; index < job_count; ++index) {
        auto& job = jobs[index];
        job.wire_id = 0x51c001 + index;
        job.nonce = UINT64_C(0x51c00100000001) + index;
        const bool c_prepared = c_scheduler->send_msg(
            AssignPrepareMsg(epoch, job.wire_id, job.nonce, 1));
        const bool f_prepared = f_scheduler->send_msg(
            AssignPrepareMsg(epoch, job.wire_id, job.nonce, 1));
        Msg *c_ready_msg = c_prepared
            ? wait_for_type(c_scheduler, Msg::ASSIGN_READY, 5000) : nullptr;
        Msg *f_ready_msg = f_prepared
            ? wait_for_type(f_scheduler, Msg::ASSIGN_READY, 5000) : nullptr;
        const auto *c_ready = dynamic_cast<const AssignReadyMsg *>(c_ready_msg);
        const auto *f_ready = dynamic_cast<const AssignReadyMsg *>(f_ready_msg);
        job.assigned = c_ready && f_ready &&
            c_ready->wire_id == job.wire_id && f_ready->wire_id == job.wire_id &&
            c_ready->epoch() == epoch && f_ready->epoch() == epoch &&
            c_ready->nonce() == job.nonce && f_ready->nonce() == job.nonce;
        delete c_ready_msg;
        delete f_ready_msg;
        job.compiler = job.assigned ? connect_tcp_bounded(f_port, 5000) : nullptr;

        job.wrapper = Service::createChannel(cdir + "/iceccd.sock");
        Environments source_envs;
        source_envs.emplace_back("x86_64", "vertical-env");
        GetCSMsg source_get(source_envs, "vertical.cpp", CompileJob::Lang_CXX,
                            1, "x86_64", 0, "", PROTOCOL_VERSION, 0, 0);
        source_get.cache_protocol = CACHE_WIRE_REVISION_R2;
        source_get.cache_profile_mask = profile_mask;
        const bool source_get_sent = job.wrapper && job.wrapper->send_msg(source_get);
        Msg *source_get_msg = source_get_sent
            ? wait_for_type(c_scheduler, Msg::GET_CS, 5000) : nullptr;
        const auto *source_get_forwarded =
            dynamic_cast<const GetCSMsg *>(source_get_msg);
        const uint32_t source_client_id = source_get_forwarded
            ? source_get_forwarded->client_id : 0;
        const bool source_use_sent = source_get_forwarded &&
            c_scheduler->send_msg(UseCSMsg(
                "x86_64", "127.0.0.1", f_port, job.wire_id, true,
                source_client_id, 0, epoch, job.nonce, f_port,
                CACHE_WIRE_REVISION_R2, profile_mask));
        delete source_get_msg;
        std::unique_ptr<Msg> source_use_msg(source_use_sent
            ? job.wrapper->get_msg_until(Clock::now() + std::chrono::seconds(5))
            : nullptr);
        const auto *source_use = dynamic_cast<const UseCSMsg*>(source_use_msg.get());
        job.source_ready = source_use &&
            source_use->job_id == job.wire_id &&
            source_use->assignmentEpoch() == epoch &&
            source_use->assignmentNonce() == job.nonce &&
            source_use->cache_endpoint_port == static_cast<uint32_t>(f_port) &&
            source_use->cache_protocol == CACHE_WIRE_REVISION_R2 &&
            source_use->cache_profile_mask == profile_mask;
        job.bytes = "vertical P51 source bytes job=" + std::to_string(index) + "\\n";
        job.source_fd = job.compiler
            ? make_vertical_source_fd(work, job.bytes) : -1;
        all_assignments_ready &= job.assigned && job.compiler != nullptr &&
            job.source_ready && job.source_fd >= 0;
    }
    REQUIRE(all_assignments_ready,
            "all C and F assignments retain distinct authenticated wrapper channels");
    if (!all_assignments_ready) return 1;
    std::unique_ptr<P51CommitReceiptGate> receipt_gate;
    if (job_count > 1) {
        receipt_gate = std::make_unique<P51CommitReceiptGate>(
            f_port, icecc->pw_uid, job_count);
        REQUIRE(receipt_gate->ready(),
                "W30 receipt gate has NET_ADMIN, iptables and its scoped UID redirect");
        if (!receipt_gate->ready()) {
            std::fprintf(stderr,
                "required W30 setup unavailable: install iptables and grant NET_ADMIN in the disposable test container\n");
            return 2;
        }
    }
    bool all_source_fds_ready = true;
    for (auto& job : jobs) {
        job.source_fd = make_vertical_source_fd(work, job.bytes);
        all_source_fds_ready &= job.source_fd >= 0;
    }
    REQUIRE(all_source_fds_ready, "all exact source files are available before concurrent transfer");
    if (!all_source_fds_ready) {
        if (receipt_gate) receipt_gate->release_commits();
        for (auto& job : jobs) {
            if (job.source_fd >= 0) ::close(job.source_fd);
            delete job.wrapper;
            delete job.compiler;
        }
        return 1;
    }

    const auto transfer_started = Clock::now();
    std::vector<std::thread> transfers;
    transfers.reserve(job_count);
    for (unsigned index = 0; index < job_count; ++index) {
        transfers.emplace_back([&, index] {
            auto& current = jobs[index];
            current.result = execute_p51_kind8(
                *current.wrapper, *current.compiler, current.wire_id,
                epoch, current.nonce, static_cast<uint32_t>(f_port), current.source_fd,
                profile_mask);
            current.finished.store(true, std::memory_order_release);
        });
    }

    bool held_all_commits = false;
    if (receipt_gate) held_all_commits = receipt_gate->wait_for_commits(
        std::chrono::seconds(30));
    else {
        const auto settle_deadline = Clock::now() + std::chrono::seconds(30);
        while (Clock::now() < settle_deadline &&
               !jobs.front().finished.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        held_all_commits = jobs.front().finished.load(std::memory_order_acquire);
    }
    if (receipt_gate)
        std::fprintf(stderr,
            "P51 receipt gate held_commits=%zu expected=%u before_release=%u\n",
            receipt_gate->observed_commits(), job_count,
            held_all_commits ? 1u : 0u);
    bool no_transfer_completed_before_release = true;
    if (receipt_gate) {
        for (const auto& job : jobs)
            no_transfer_completed_before_release &=
                !job.finished.load(std::memory_order_acquire);
    }
    if (receipt_gate) receipt_gate->release_commits();
    for (auto& transfer : transfers) if (transfer.joinable()) transfer.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - transfer_started).count();

    std::set<uint64_t> observed_tu_sequences;
    bool all_transfers_committed = true;
    for (unsigned index = 0; index < job_count; ++index) {
        const auto& job = jobs[index];
        const auto& result = job.result;
        const bool exact = result.code ==
                icecc::p50::local::SourceTransferResultCode::Committed &&
            result.error_code == 0 && result.raw_bytes == job.bytes.size() &&
            result.raw_digest == icecc::digest128(job.bytes) &&
            result.c_store_guid != icecc::p50::CStoreGuid{} &&
            observed_tu_sequences.insert(result.tu_seq).second;
        all_transfers_committed &= exact;
        if (!exact)
            std::fprintf(stderr,
                "P51 fixture job=%u result code=%u error=%u attempts=%u tu=%llu raw=%llu expected=%zu digest_match=%u guid_valid=%u\n",
                index, static_cast<unsigned>(result.code), result.error_code,
                result.attempts, static_cast<unsigned long long>(result.tu_seq),
                static_cast<unsigned long long>(result.raw_bytes), job.bytes.size(),
                result.raw_digest == icecc::digest128(job.bytes) ? 1u : 0u,
                result.c_store_guid != icecc::p50::CStoreGuid{} ? 1u : 0u);
    }
    const bool contiguous_tu_sequences = observed_tu_sequences.size() == job_count &&
        *observed_tu_sequences.begin() == 0 &&
        *observed_tu_sequences.rbegin() == job_count - 1;
    REQUIRE(held_all_commits,
            "F emitted exactly W30 distinct complete COMMIT frames before any C receipt release");
    REQUIRE(no_transfer_completed_before_release,
            "all source-transfer callers remain pending while COMMIT receipts are withheld");
    REQUIRE(all_transfers_committed && contiguous_tu_sequences && elapsed < 30000,
            "real C kind-8 service completes all W30 exact distinct inputs and TU identities");
    if (receipt_gate)
        std::fprintf(stderr,
            "P51 vertical summary jobs=%u buffered_commits=%zu elapsed_ms=%lld\n",
            job_count, receipt_gate->observed_commits(), static_cast<long long>(elapsed));

    bool all_compilefile_sent = true;
    bool all_exactly_attached = true;
    bool all_compilefile_bounded = true;
    for (auto& item : jobs) {
        if (item.result.code != icecc::p50::local::SourceTransferResultCode::Committed ||
            item.compiler == nullptr) {
            all_compilefile_sent = all_exactly_attached = all_compilefile_bounded = false;
            continue;
        }
        CompileJob compile_job = attachment_compile_job(
            item.wire_id, epoch, item.nonce,
            source_arm(item.wire_id, epoch, item.nonce,
                       static_cast<uint32_t>(f_port), static_cast<uint32_t>(f_port)),
            nullptr);
        CompileInputIdentity identity;
        identity.profile = profile_mask == CACHE_PROFILE_P29V1
            ? CompileInputIdentity::P29V1Profile
            : profile_mask == CACHE_PROFILE_ZSTD_ROUTE
                ? CompileInputIdentity::ZstdRouteProfile
                : CompileInputIdentity::ZstdTuProfile;
        identity.c_store_guid = item.result.c_store_guid.bytes;
        identity.tu_seq = item.result.tu_seq;
        identity.raw_bytes = item.result.raw_bytes;
        identity.raw_digest = item.result.raw_digest.bytes;
        identity.attempt_id = item.nonce;
        identity.request_id = item.nonce;
        compile_job.setCompileInputIdentity(identity);
        std::error_code attach_log_error;
        item.attach_log_offset = std::filesystem::file_size(
            fdir + "/iceccd.log", attach_log_error);
        item.compile_sent = !attach_log_error &&
            item.compiler->send_msg(CompileFileMsg(&compile_job));
        all_compilefile_sent &= item.compile_sent;
    }
    for (auto& item : jobs) {
        if (!item.compile_sent) {
            all_exactly_attached = all_compilefile_bounded = false;
            continue;
        }
        const std::string accepted_marker =
            "P50_INPUT_ATTACH_END job=" + std::to_string(item.wire_id) +
            " epoch=" + std::to_string(epoch) +
            " nonce=" + std::to_string(item.nonce) +
            " request=" + std::to_string(item.nonce) + " elapsed_ms=";
        bool attached = wait_attachment_log(
            fdir + "/iceccd.log", item.attach_log_offset, accepted_marker, 15000);
        if (attached) {
            const std::string suffix = read_file_suffix(
                fdir + "/iceccd.log", item.attach_log_offset);
            const size_t marker_pos = suffix.find(accepted_marker);
            const size_t status_pos = marker_pos == std::string::npos
                ? std::string::npos : suffix.find(" status=0", marker_pos);
            attached = status_pos != std::string::npos;
        }
        item.attached = attached;
        item.compile_bounded = wait_eof(item.compiler, 15000);
        all_exactly_attached &= item.attached;
        all_compilefile_bounded &= item.compile_bounded;
    }
    REQUIRE(all_compilefile_sent,
            "all original F compiler TCP channels accept their CompileFile after R2 receipt");
    REQUIRE(all_exactly_attached,
            "F reports Accepted attachment for every exact committed input identity");
    REQUIRE(all_compilefile_bounded,
            "F daemon settles all W30 CompileFile attachments within bounds");
    for (auto& job : jobs) {
        delete job.wrapper;
        delete job.compiler;
    }
    const size_t peak_held_commits = receipt_gate
        ? receipt_gate->observed_commits() : 0;
    receipt_gate.reset();

    for (pid_t pid : {c_pid, f_pid}) ::kill(pid, SIGTERM);
    int c_status = 0, f_status = 0;
    const bool c_reaped = wait_child(c_pid, 10000, &c_status);
    const bool f_reaped = wait_child(f_pid, 10000, &f_status);
    if (c_reaped) daemon_cleanup.c_pid = -1;
    if (f_reaped) daemon_cleanup.f_pid = -1;
    REQUIRE(c_reaped && WIFEXITED(c_status) && WEXITSTATUS(c_status) == 0,
            "C daemon exits cleanly after the vertical transfer");
    REQUIRE(f_reaped && WIFEXITED(f_status) && WEXITSTATUS(f_status) == 0,
            "F daemon exits cleanly after the vertical transfer");
    delete c_scheduler;
    delete f_scheduler;
    ::close(c_scheduler_listener);
    ::close(f_scheduler_listener);
    if (failures == 0) std::filesystem::remove_all(work);
    else std::fprintf(stderr, "retained vertical work directory: %s\n", work.c_str());
    if (failures == 0 && job_count == 30)
        std::fprintf(stderr,
            "P51_VERTICAL_W30_PASS jobs=%u peak_held_commits=%zu elapsed_ms=%lld\n",
            job_count, peak_held_commits, static_cast<long long>(elapsed));
    return failures ? 1 : 0;
}
#endif

static uint32_t selected_vertical_profile()
{
    const char *value = ::getenv("ICECC_TEST_P51_PROFILE");
    if (value == nullptr || std::strcmp(value, "ZSTD_TU") == 0)
        return CACHE_PROFILE_ZSTD_TU;
    if (std::strcmp(value, "P29V1") == 0)
        return CACHE_PROFILE_P29V1;
    if (std::strcmp(value, "ZSTD_ROUTE") == 0)
        return CACHE_PROFILE_ZSTD_ROUTE;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <iceccd> <icecc-cache-service>\n", argv[0]);
        return 2;
    }
    if (::getenv("ICECC_TEST_POSITIVE_DAEMON") == nullptr) {
        std::fprintf(stderr, "SKIP: set ICECC_TEST_POSITIVE_DAEMON=1 in an isolated root container\n");
        return 77;
    }
#if !defined(HAVE_LIBCAP_NG)
    std::fprintf(stderr, "SKIP: positive remote daemon test requires libcap-ng\n");
    return 77;
#else
    if (::geteuid() != 0) {
        std::fprintf(stderr, "SKIP: positive remote daemon test requires container root\n");
        return 77;
    }
    passwd *icecc = ::getpwnam("icecc");
    if (icecc == nullptr || icecc->pw_uid == 0 || icecc->pw_gid == 0) {
        std::fprintf(stderr, "SKIP: isolated image has no unprivileged icecc identity\n");
        return 77;
    }

    if (::getenv("ICECC_TEST_P51_VERTICAL_W30") != nullptr ||
        ::getenv("ICECC_TEST_P51_VERTICAL") != nullptr) {
        const uint32_t profile_mask = selected_vertical_profile();
        if (profile_mask == 0) {
            std::fprintf(stderr, "FAIL: ICECC_TEST_P51_PROFILE must be P29V1, ZSTD_TU, or ZSTD_ROUTE\n");
            return 2;
        }
        const unsigned count = ::getenv("ICECC_TEST_P51_VERTICAL_W30") != nullptr
            ? 30u : 1u;
        return run_p51_vertical(argv[1], argv[2], icecc, count, profile_mask);
    }

    ::signal(SIGPIPE, SIG_IGN);
    const char *temporary_root = ::getenv("TMPDIR");
    const std::string prefix = temporary_root && *temporary_root
        ? temporary_root : "/tmp";
    std::string pattern = prefix + "/p50dp.XXXXXX";
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    char *created = ::mkdtemp(mutable_pattern.data());
    REQUIRE(created != nullptr, "positive daemon temporary root created");
    if (created == nullptr) return 2;
    const std::string work(created);
    const std::string envdir = work + "/envs";
    const std::string runtime = work + "/runtime";
    const std::string local_socket = work + "/iceccd.sock";
    const std::string log = work + "/iceccd.log";
    if (::getenv("ICECC_TEST_SCHEDULER_BACKPRESSURE") != nullptr) {
        const std::string marker = work + "/scheduler-send-eagain";
        ::setenv("ICECC_TEST_SEND_EAGAIN_MARKER", marker.c_str(), 1);
    }
    REQUIRE(::chown(work.c_str(), icecc->pw_uid, icecc->pw_gid) == 0
                && ::chmod(work.c_str(), 0700) == 0,
            "temporary root belongs only to the daemon identity");
    REQUIRE(::mkdir(envdir.c_str(), 0700) == 0
                && ::chown(envdir.c_str(), icecc->pw_uid, icecc->pw_gid) == 0,
            "environment directory has daemon ownership");
    REQUIRE(::mkdir(runtime.c_str(), 0700) == 0
                && ::chown(runtime.c_str(), icecc->pw_uid, icecc->pw_gid) == 0,
            "sidecar runtime directory has exact private ownership");

    int scheduler_port = 0;
    const int scheduler_listener = listen_ephemeral(&scheduler_port);
    const int daemon_port = reserve_port();
    REQUIRE(scheduler_listener >= 0 && scheduler_port > 0,
            "fake scheduler listens on an ephemeral port");
    REQUIRE(daemon_port > 0, "real daemon public port reserved");
    if (scheduler_listener < 0 || daemon_port <= 0) return 2;

    const pid_t daemon_pid = ::fork();
    if (daemon_pid == 0) {
        char scheduler[64];
        char public_port[16];
        std::snprintf(scheduler, sizeof(scheduler), "127.0.0.1:%d", scheduler_port);
        std::snprintf(public_port, sizeof(public_port), "%d", daemon_port);
        ::setenv("ICECC_TESTS", "1", 1);
        ::setenv("ICECC_TEST_SOCKET", local_socket.c_str(), 1);
        char scheduler_port_text[16];
        std::snprintf(scheduler_port_text, sizeof(scheduler_port_text), "%d", scheduler_port);
        ::setenv("ICECC_TEST_BACKPRESSURE_SCHED_PORT", scheduler_port_text, 1);
        if (const char *shim = ::getenv("ICECC_TEST_SNDBUF_SHIM");
            shim != nullptr && *shim != '\0')
            ::setenv("LD_PRELOAD", shim, 1);
        ::execl(argv[1], argv[1], "-p", public_port, "-m", "1",
                "-s", scheduler, "-n", "p50-daemon-positive", "-N", "p50-f",
                "-b", envdir.c_str(), "-l", log.c_str(),
                "--cache-service", argv[2], "--cache-runtime-dir", runtime.c_str(),
                "-v", "-v", "-v", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    REQUIRE(daemon_pid > 0, "real positive iceccd process started");
    if (daemon_pid <= 0) return 2;

    MsgChannel *scheduler = accept_channel(scheduler_listener, 10000);
    Msg *initial_message = wait_for_type(scheduler, Msg::LOGIN, 5000);
    LoginMsg *initial = dynamic_cast<LoginMsg *>(initial_message);
    REQUIRE(scheduler != nullptr && initial != nullptr,
            "fake scheduler received the initial real Login");
    REQUIRE(absent(initial), "initial Login is canonical cache absence before ConfCS/READY");
    delete initial_message;

    MsgChannel *premature = connect_tcp_bounded(daemon_port, 5000);
    REQUIRE(premature != nullptr,
            "pre-activation Protocol-50 client reached the real public listener");
    REQUIRE(premature && premature->send_msg(CacheSessionMsg()),
            "pre-activation client sent CACHE_SESSION");
    REQUIRE(wait_eof(premature, 5000),
            "LOGIN_ATTEMPT cannot dispatch cache while scheduler is inactive");
    delete premature;

    const uint64_t epoch = UINT64_C(0x5000000000000001);
    const ConfCSMsg activate(epoch, ConfCSMsg::StrictNonce);
    REQUIRE(scheduler && scheduler->send_msg(activate),
            "first ConfCS activates the scheduler session");
    Msg *positive_message = wait_for_type(scheduler, Msg::LOGIN, 10000);
    LoginMsg *positive = dynamic_cast<LoginMsg *>(positive_message);
    const bool p51_cancel_case =
        std::getenv("ICECC_TEST_P51_CANCEL_REPLACEMENT") != nullptr;
    REQUIRE(present_revision(
                positive, static_cast<uint32_t>(daemon_port),
                p51_cancel_case ? CACHE_WIRE_REVISION_R2
                                : CACHE_WIRE_REVISION_R1),
            "real READY/authenticated sidecar publishes exact positive advertisement");
    delete positive_message;

    if (p51_cancel_case) {
        const uint32_t wire_id = 0x7a710301;
        const uint64_t nonce = UINT64_C(0x7a71030100000001);
        P50SourceArmFields source = source_arm(
            wire_id, epoch, nonce, static_cast<uint32_t>(daemon_port),
            static_cast<uint32_t>(daemon_port));
        source.cache_protocol = CACHE_WIRE_REVISION_R2;
        const P51SourceArmFields arm{source, 30};
        const bool prepared = scheduler && scheduler->send_msg(
            AssignPrepareMsg(epoch, wire_id, nonce, 1));
        Msg *ready_message = prepared
            ? wait_for_type(scheduler, Msg::ASSIGN_READY, 5000) : nullptr;
        const auto *ready = dynamic_cast<const AssignReadyMsg *>(ready_message);
        const bool assignment_ready = ready && ready->wire_id == wire_id &&
            ready->epoch() == epoch && ready->nonce() == nonce;
        delete ready_message;

        MsgChannel *wrapper = assignment_ready
            ? connect_tcp_bounded(daemon_port, 5000) : nullptr;
        const P51SourceArmMsg request{arm};
        const bool request_sent = wrapper && wrapper->send_msg(request);
        Msg *armed_message = request_sent
            ? wait_for_type(wrapper, Msg::P51_SOURCE_ARMED, 5000) : nullptr;
        auto *armed = dynamic_cast<P51SourceArmedMsg *>(armed_message);
        const bool arm_ready = armed && armed->acknowledges(request) &&
            armed->f_store_generation != 0 && armed->selected_window == 30;
        delete armed_message;
        REQUIRE(assignment_ready && request_sent && arm_ready,
                "opt-in P51 ARM reserves the authenticated job at W30");
        if (!assignment_ready || !request_sent || !arm_ready) {
            delete wrapper;
            ::kill(daemon_pid, SIGTERM);
            int failed_status = 0;
            bool failed_reaped = wait_child(daemon_pid, 10000, &failed_status);
            if (!failed_reaped) {
                ::kill(daemon_pid, SIGKILL);
                (void)::waitpid(daemon_pid, &failed_status, 0);
            }
            delete scheduler;
            ::close(scheduler_listener);
            std::fprintf(stderr,
                         "retained failing work directory: %s\n", work.c_str());
            return 1;
        }

        std::error_code replace_log_error;
        const uintmax_t replace_log_offset =
            std::filesystem::file_size(log, replace_log_error);
        const pid_t old_sidecar = find_attachment_sidecar(daemon_pid, argv[2]);
        const bool killed_old = old_sidecar > 1 &&
            ::kill(old_sidecar, SIGKILL) == 0;
        const auto replacement_deadline =
            Clock::now() + std::chrono::seconds(15);
        pid_t new_sidecar = -1;
        bool replacement_advertised = false;
        while (killed_old && Clock::now() < replacement_deadline) {
            new_sidecar = find_attachment_sidecar(daemon_pid, argv[2]);
            if (new_sidecar > 1 && new_sidecar != old_sidecar) {
                Msg *login_message = wait_for_type(scheduler, Msg::LOGIN, 1000);
                auto *login = dynamic_cast<LoginMsg *>(login_message);
                replacement_advertised = present_revision(
                    login, static_cast<uint32_t>(daemon_port),
                    CACHE_WIRE_REVISION_R2);
                delete login_message;
                if (replacement_advertised) break;
            }
            ::usleep(10000);
        }
        REQUIRE(killed_old && new_sidecar > 1 && new_sidecar != old_sidecar &&
                    replacement_advertised,
                "READY replacement publishes a distinct P51 sidecar incarnation");

        // Disconnect only after the READY replacement. The queued cancellation
        // still names the old immutable lease; its failure must not withdraw
        // or shut down the newly published incarnation.
        delete wrapper;
        wrapper = nullptr;
        const bool old_cancel_failed = wait_attachment_log(
            log, replace_log_offset,
            "P51 source-reservation cancellation failed:", 5000);
        REQUIRE(old_cancel_failed,
                "late old-incarnation cancellation fails closed after replacement");

        const uint32_t successor_id = wire_id + 1;
        const uint64_t successor_nonce = nonce + 1;
        P50SourceArmFields successor_source = source_arm(
            successor_id, epoch, successor_nonce,
            static_cast<uint32_t>(daemon_port),
            static_cast<uint32_t>(daemon_port));
        successor_source.cache_protocol = CACHE_WIRE_REVISION_R2;
        const P51SourceArmFields successor_arm{successor_source, 30};
        const bool successor_prepared = scheduler && scheduler->send_msg(
            AssignPrepareMsg(epoch, successor_id, successor_nonce, 1));
        Msg *successor_ready_message = successor_prepared
            ? wait_for_type(scheduler, Msg::ASSIGN_READY, 5000) : nullptr;
        const auto *successor_ready =
            dynamic_cast<const AssignReadyMsg *>(successor_ready_message);
        const bool successor_assignment_ready = successor_ready &&
            successor_ready->wire_id == successor_id &&
            successor_ready->epoch() == epoch &&
            successor_ready->nonce() == successor_nonce;
        delete successor_ready_message;
        MsgChannel *successor = successor_assignment_ready
            ? connect_tcp_bounded(daemon_port, 5000) : nullptr;
        const P51SourceArmMsg successor_request{successor_arm};
        const bool successor_sent = successor &&
            successor->send_msg(successor_request);
        Msg *successor_reply_message = successor_sent
            ? wait_for_type(successor, Msg::P51_SOURCE_ARMED, 5000) : nullptr;
        auto *successor_reply =
            dynamic_cast<P51SourceArmedMsg *>(successor_reply_message);
        const bool successor_arm_ready = successor_reply &&
            successor_reply->acknowledges(successor_request) &&
            successor_reply->selected_window == 30;
        delete successor_reply_message;
        delete successor;
        REQUIRE(successor_assignment_ready && successor_sent &&
                    successor_arm_ready,
                "stale cancellation cannot withdraw the replacement P51 sidecar");

        ::kill(daemon_pid, SIGTERM);
        int status = 0;
        bool reaped = wait_child(daemon_pid, 10000, &status);
        if (!reaped) {
            ::kill(daemon_pid, SIGKILL);
            (void)::waitpid(daemon_pid, &status, 0);
        }
        REQUIRE(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "P51 cancellation replacement daemon exits cleanly");
        delete scheduler;
        ::close(scheduler_listener);
        if (failures == 0) std::filesystem::remove_all(work);
        else std::fprintf(stderr, "retained failing work directory: %s\n", work.c_str());
        return failures ? 1 : 0;
    }

    // Diagnostic regression: a synchronous compiler-input attachment must not
    // prevent a fresh ordinary peer from completing admission.  The sidecar
    // is test-owned and always resumed after a successful stop.
    MsgChannel *attach_healthy_peer = connect_tcp_bounded(daemon_port, 5000);
    const bool attach_healthy_greeting = attach_healthy_peer &&
        attach_healthy_peer->send_msg(CacheSessionMsg());
    delete attach_healthy_peer;
    REQUIRE(attach_healthy_greeting,
            "attachment diagnostic healthy-sidecar peer greeting succeeds");

    const bool disconnect_pending = std::getenv("ICECC_TEST_PENDING_DISCONNECT") != nullptr;
    const uint32_t attach_wire_id = 0x7a710101 + unsigned(disconnect_pending);
    // Assignment epochs are scheduler-session scoped; keep the active epoch
    // and mint a distinct wire/nonce pair for this diagnostic.
    const uint64_t attach_epoch = epoch;
    const uint64_t attach_nonce = UINT64_C(0x7a71010100000001) + unsigned(disconnect_pending);
    const P50SourceArmFields attach_arm = source_arm(
        attach_wire_id, attach_epoch, attach_nonce,
        static_cast<uint32_t>(daemon_port), static_cast<uint32_t>(daemon_port));
    REQUIRE(scheduler && scheduler->send_msg(
                AssignPrepareMsg(attach_epoch, attach_wire_id, attach_nonce, 1)),
            "attachment diagnostic PREPARE reaches the daemon");
    Msg *attach_ready_message = wait_for_type(scheduler, Msg::ASSIGN_READY, 5000);
    auto *attach_ready = dynamic_cast<AssignReadyMsg *>(attach_ready_message);
    REQUIRE(attach_ready != nullptr && attach_ready->wire_id == attach_wire_id &&
                attach_ready->epoch() == attach_epoch && attach_ready->nonce() == attach_nonce,
            "attachment diagnostic assignment is authenticated");
    delete attach_ready_message;

    MsgChannel *attach_wrapper = connect_tcp_bounded(daemon_port, 5000);
    icecc::p50::local::P50SourceTransferResult published_input{};
    bool attach_source_ready = false;
    if (disconnect_pending) {
        REQUIRE(attach_wrapper && attach_wrapper->send_msg(P50SourceArmMsg(attach_arm)),
                "pending-disconnect source arm is sent without a source transfer");
        Msg *attach_armed_message = wait_for_type(
            attach_wrapper, Msg::P50_SOURCE_ARMED, 5000);
        auto *attach_armed = dynamic_cast<P50SourceArmedMsg *>(attach_armed_message);
        attach_source_ready = attach_armed != nullptr && attach_armed->arm == attach_arm;
        delete attach_armed_message;
        REQUIRE(attach_source_ready,
                "pending-disconnect source arm is acknowledged before deliberate no-input attach");
        REQUIRE(attach_wrapper && attach_wrapper->send_msg(CacheSessionMsg()),
                "pending-disconnect wrapper enters CACHE_SESSION with no committed input");
    } else {
        const std::string exact_source = "published P50 attachment input\\n";
        published_input = attach_wrapper
            ? publish_input_to_armed_f(*attach_wrapper, attach_arm, exact_source)
            : icecc::p50::local::P50SourceTransferResult{};
        attach_source_ready = published_input.code ==
                icecc::p50::local::SourceTransferResultCode::Committed &&
            published_input.error_code == 0 && published_input.tu_seq == 0 &&
            published_input.raw_bytes == exact_source.size() &&
            published_input.raw_digest == icecc::digest128(exact_source) &&
            published_input.c_store_guid != icecc::p50::CStoreGuid{};
        if (!attach_source_ready) {
            std::fprintf(stderr,
                "R1 fixture result code=%u error=%u attempts=%u tu=%llu bytes=%llu/%zu digest_match=%d c_guid_nonzero=%d\n",
                static_cast<unsigned>(published_input.code), published_input.error_code,
                published_input.attempts,
                static_cast<unsigned long long>(published_input.tu_seq),
                static_cast<unsigned long long>(published_input.raw_bytes),
                exact_source.size(),
                int(published_input.raw_digest == icecc::digest128(exact_source)),
                int(published_input.c_store_guid != icecc::p50::CStoreGuid{}));
        }
        REQUIRE(attach_source_ready,
                "attachment input is committed before CompileFile uses its exact identity");
    }
    if (::getenv("ICECC_TEST_R1_PUBLISH_ONLY") != nullptr) {
        delete attach_wrapper;
        (void)::kill(daemon_pid, SIGTERM);
        int status = 0;
        bool reaped = wait_child(daemon_pid, 10000, &status);
        if (!reaped) {
            (void)::kill(daemon_pid, SIGKILL);
            (void)::waitpid(daemon_pid, &status, 0);
        }
        REQUIRE(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "R1 source publication daemon exits cleanly");
        delete scheduler;
        ::close(scheduler_listener);
        if (failures == 0) std::filesystem::remove_all(work);
        else std::fprintf(stderr, "retained failing work directory: %s\n", work.c_str());
        return failures ? 1 : 0;
    }
    ::usleep(100 * 1000);

    const pid_t attach_sidecar_pid = find_attachment_sidecar(daemon_pid, argv[2]);
    const bool attach_stop_sent = attach_sidecar_pid > 1 &&
        ::kill(attach_sidecar_pid, SIGSTOP) == 0;
    bool attach_stopped = false;
    if (attach_stop_sent) {
        const auto stop_deadline = Clock::now() + std::chrono::milliseconds(2000);
        while (Clock::now() < stop_deadline) {
            std::ifstream status(std::string("/proc/") +
                                 std::to_string(attach_sidecar_pid) + "/status");
            std::string line;
            while (std::getline(status, line))
                if (line.rfind("State:", 0) == 0 && line.find('T') != std::string::npos)
                    attach_stopped = true;
            if (attach_stopped) break;
            ::usleep(10000);
        }
    }
    REQUIRE(attach_stopped, "attachment diagnostic stops only its test-owned sidecar");

    std::error_code attach_log_error;
    const uintmax_t attach_log_offset = std::filesystem::file_size(log, attach_log_error);
    MsgChannel *attach_compile_client = connect_tcp_bounded(daemon_port, 5000);
    CompileJob attach_job = attachment_compile_job(
        attach_wire_id, attach_epoch, attach_nonce, attach_arm,
        disconnect_pending ? nullptr : &published_input);
    const bool attach_compile_sent = attach_compile_client &&
        attach_compile_client->send_msg(CompileFileMsg(&attach_job));
    REQUIRE(attach_compile_sent, "attachment diagnostic valid CompileFile is sent");
    const std::string attach_begin_marker =
        "P50_INPUT_ATTACH_BEGIN job=" + std::to_string(attach_wire_id) +
        " epoch=" + std::to_string(attach_epoch) +
        " nonce=" + std::to_string(attach_nonce) +
        " request=" + std::to_string(attach_arm.source_request_id);
    const std::string attach_end_marker =
        "P50_INPUT_ATTACH_END job=" + std::to_string(attach_wire_id) +
        " epoch=" + std::to_string(attach_epoch) +
        " nonce=" + std::to_string(attach_nonce) +
        " request=" + std::to_string(attach_arm.source_request_id);
    const bool attach_begin_seen = attach_compile_sent && !attach_log_error &&
        wait_attachment_log(log, attach_log_offset, attach_begin_marker, 3000);
    REQUIRE(attach_begin_seen, "attachment handler begins exact authenticated attach");

    // While the authenticated CompileFile remains active, keep a bounded
    // remote admission burst outstanding and prove that a fresh source-arm
    // still receives its ACK within the existing four-second budget.  This
    // uses the real attachment path above as ordinary daemon work; it does
    // not substitute Ping traffic or assert compile success.
    constexpr size_t kCompileAdmissionBurstCount = 64;
    std::vector<int> compile_admission_fds;
    compile_admission_fds.reserve(kCompileAdmissionBurstCount);
    for (size_t peer = 0; peer != kCompileAdmissionBurstCount; ++peer) {
        const int fd = connect_raw_tcp(daemon_port);
        if (fd < 0) break;
        compile_admission_fds.push_back(fd);
    }
    const uint32_t concurrent_wire_id = attach_wire_id + 1;
    const uint64_t concurrent_nonce = attach_nonce + 1;
    const P50SourceArmFields concurrent_arm = source_arm(
        concurrent_wire_id, attach_epoch, concurrent_nonce,
        static_cast<uint32_t>(daemon_port), static_cast<uint32_t>(daemon_port));
    const bool concurrent_prepare = scheduler && scheduler->send_msg(
        AssignPrepareMsg(attach_epoch, concurrent_wire_id, concurrent_nonce, 1));
    Msg *concurrent_ready_message = concurrent_prepare
        ? wait_for_type(scheduler, Msg::ASSIGN_READY, 5000) : nullptr;
    auto *concurrent_ready = dynamic_cast<AssignReadyMsg *>(concurrent_ready_message);
    const bool concurrent_ready_valid = concurrent_ready != nullptr &&
        concurrent_ready->wire_id == concurrent_wire_id &&
        concurrent_ready->epoch() == attach_epoch &&
        concurrent_ready->nonce() == concurrent_nonce;
    delete concurrent_ready_message;
    MsgChannel *concurrent_source = concurrent_ready_valid
        ? connect_tcp_bounded(daemon_port, 5000) : nullptr;
    const auto concurrent_arm_started = Clock::now();
    const bool concurrent_arm_sent = concurrent_source &&
        concurrent_source->send_msg(P50SourceArmMsg(concurrent_arm));
    Msg *concurrent_armed_message = concurrent_arm_sent
        ? wait_for_type(concurrent_source, Msg::P50_SOURCE_ARMED, 5000) : nullptr;
    auto *concurrent_armed = dynamic_cast<P50SourceArmedMsg *>(concurrent_armed_message);
    const auto concurrent_arm_elapsed = std::chrono::duration_cast<
        std::chrono::milliseconds>(Clock::now() - concurrent_arm_started).count();
    const bool concurrent_arm_ack = concurrent_armed != nullptr &&
        concurrent_armed->arm == concurrent_arm && concurrent_arm_elapsed < 4000;
    delete concurrent_armed_message;
    delete concurrent_source;
    for (int fd : compile_admission_fds) ::close(fd);
    REQUIRE(compile_admission_fds.size() == kCompileAdmissionBurstCount,
            "remote admission burst remains active during an ordinary CompileFile");
    REQUIRE(concurrent_prepare && concurrent_ready_valid && concurrent_arm_sent &&
                concurrent_arm_ack,
            "source-arm ACK stays within budget during ordinary work and admissions");
    const bool attachment_still_pending =
        read_file_suffix(log, attach_log_offset).find(attach_end_marker) ==
        std::string::npos;
    REQUIRE(attachment_still_pending,
            "ordinary CompileFile remains pending during concurrent source admission");

    bool attach_resumed = false;
    if (disconnect_pending) {
        delete attach_compile_client;
        attach_compile_client = nullptr;
        attach_resumed = attach_stop_sent && ::kill(attach_sidecar_pid, SIGCONT) == 0;
    }

    std::atomic<bool> attach_peer_connected{false};
    std::atomic<bool> attach_peer_greeting{false};
    std::thread attach_peer([&] {
        MsgChannel *peer = connect_tcp_bounded(daemon_port, 5000);
        attach_peer_connected.store(peer != nullptr);
        const bool sent = peer && peer->send_msg(CacheSessionMsg());
        attach_peer_greeting.store(sent);
        delete peer;
    });
    ::usleep(1200 * 1000);
    REQUIRE(attach_begin_seen && attach_peer_connected.load() && attach_peer_greeting.load() &&
                read_file_suffix(log, attach_log_offset).find(attach_end_marker) == std::string::npos,
            "peer handshake completes while exact attachment remains pending");
    if (!disconnect_pending)
        attach_resumed = attach_stop_sent && ::kill(attach_sidecar_pid, SIGCONT) == 0;
    REQUIRE(attach_resumed, "attachment diagnostic resumes its stopped sidecar");
    attach_peer.join();
    const bool attach_compile_bounded = disconnect_pending ||
        (attach_compile_client && wait_eof(attach_compile_client, 8000));
    delete attach_compile_client;
    delete attach_wrapper;
    REQUIRE(attach_peer_connected.load() && attach_peer_greeting.load(),
            "peer admission resumes after attachment sidecar continuation");
    REQUIRE(attach_compile_bounded, "attachment diagnostic CompileFile terminates boundedly");
    if (disconnect_pending) {
        // No input was committed in this fixture: unknown-record proves the
        // exact pending cancellation reached the sidecar, not lease revocation.
        const std::string cancelled = "P50 input settlement job " +
            std::to_string(attach_wire_id) + " action 1 status unknown-record";
        REQUIRE(wait_attachment_log(log, attach_log_offset, cancelled, 3000),
                "pending disconnect delivers CancelAttempt to the sidecar");
        REQUIRE(read_file_suffix(log, attach_log_offset).find(attach_end_marker) ==
                    std::string::npos,
                "disconnected attachment is not resumed or published");
    } else {
        REQUIRE(wait_attachment_log(log, attach_log_offset, attach_end_marker, 2000),
                "attachment handler emits exact END after sidecar continuation");
    }
    // Stop only this private test daemon, queue one more connection than the
    // 64-socket accept quantum, and resume it.  Every connection already
    // contains protocol negotiation and a deliberately unarmed CACHE_SESSION.
    // The 65th socket crosses the accept quantum and was absent from the
    // original poll snapshot. Admission must progress between ordinary client
    // activities; otherwise a busy compile turn can strand its protocol
    // greeting past a legacy client's fixed deadline.
    constexpr size_t kAdmissionBurstCount = 65;
    std::error_code size_error;
    const uintmax_t admission_log_offset = std::filesystem::file_size(log, size_error);
    const bool stop_sent = ::kill(daemon_pid, SIGSTOP) == 0;
    int stop_status = 0;
    const bool stopped = stop_sent &&
        ::waitpid(daemon_pid, &stop_status, WUNTRACED) == daemon_pid &&
        WIFSTOPPED(stop_status);
    std::vector<int> admission_fds;
    if (stopped) {
        for (size_t index = 0; index != kAdmissionBurstCount; ++index) {
            const int fd = connect_preframed_cache_session(daemon_port);
            if (fd < 0) break;
            admission_fds.push_back(fd);
        }
    }
    const bool resumed = ::kill(daemon_pid, SIGCONT) == 0;
    bool admission_eof = stopped && resumed &&
        admission_fds.size() == kAdmissionBurstCount;
    for (int fd : admission_fds) {
        if (!wait_raw_eof(fd, 5000)) admission_eof = false;
        ::close(fd);
    }
    const std::string admission_log = size_error
        ? std::string() : read_file_suffix(log, admission_log_offset);
    const std::string accepted_marker = "accepted ";
    const std::string refused_marker =
        "CACHE_SESSION refused: no authenticated cache sidecar";
    const size_t accepted_count = count_text(admission_log, accepted_marker);
    const size_t refused_count = count_text(admission_log, refused_marker);
    const size_t last_accept = admission_log.rfind(accepted_marker);
    const size_t first_refusal = admission_log.find(refused_marker);
    size_t refusals_before_last_accept = 0;
    if (last_accept != std::string::npos) {
        size_t offset = admission_log.find(refused_marker);
        while (offset != std::string::npos && offset < last_accept) {
            ++refusals_before_last_accept;
            offset = admission_log.find(refused_marker,
                                        offset + refused_marker.size());
        }
    }
    const bool admission_ordered = admission_eof &&
        accepted_count == kAdmissionBurstCount &&
        refused_count == kAdmissionBurstCount &&
        last_accept != std::string::npos && first_refusal != std::string::npos &&
        refusals_before_last_accept <= 1;
    REQUIRE(stopped && admission_fds.size() == kAdmissionBurstCount && resumed,
            "burst beyond the accept quantum queued before daemon admission");
    REQUIRE(admission_eof,
            "all preframed burst clients are terminally handled after admission");
    REQUIRE(admission_ordered,
            "turn-boundary admission waits behind at most one client activity");

    // Fill the remote share of the asynchronous handshake table with silent
    // TCP peers. The protected local share must still admit Unix wrappers;
    // a single global cap lets remote silence starve this path indefinitely.
    const int pre_saturation_daemon_fds = process_fd_count(daemon_pid);
    constexpr size_t kRemoteSaturationCount = 256;
    std::vector<int> silent_remote_fds;
    silent_remote_fds.reserve(kRemoteSaturationCount);
    for (size_t index = 0; index != kRemoteSaturationCount; ++index) {
        const int fd = connect_raw_tcp(daemon_port);
        if (fd < 0) break;
        silent_remote_fds.push_back(fd);
    }
    // Give the running daemon a deterministic chance to consume the public
    // queue up to its remote sub-limit before presenting the local peer.
    ::usleep(100 * 1000);
    const auto local_admission_started = Clock::now();
    MsgChannel *reserved_local = Service::createChannel(local_socket);
    const auto local_admission_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - local_admission_started).count();
    const bool local_admitted = reserved_local != nullptr &&
        local_admission_elapsed < 4000;
    const bool local_refused_normally = reserved_local != nullptr &&
        reserved_local->send_msg(CacheSessionMsg()) &&
        wait_eof(reserved_local, 4000);
    delete reserved_local;
    for (int fd : silent_remote_fds) ::close(fd);
    REQUIRE(silent_remote_fds.size() == kRemoteSaturationCount,
            "remote silent peers saturate more than the protected remote share");
    REQUIRE(local_admitted && local_refused_normally,
            "remote handshake saturation preserves prompt Unix-client admission");
    const int post_saturation_daemon_fds = wait_for_fd_count_at_most(
        daemon_pid, pre_saturation_daemon_fds + 2, 5000);
    REQUIRE(post_saturation_daemon_fds >= 0 &&
                post_saturation_daemon_fds <= pre_saturation_daemon_fds + 2,
            "remote saturation teardown releases every pending descriptor");

    const int baseline_daemon_fds = process_fd_count(daemon_pid);
    REQUIRE(baseline_daemon_fds > 0,
            "real daemon descriptor baseline is observable");

    // The removed implementation opened and retained one second P5FS control
    // relationship for every authoritative CacheSession.  Its table was
    // bounded at 64, so 65 sequential handoffs are the smallest production
    // witness that distinguishes the single authoritative path from that
    // shadow-owner design.
    constexpr size_t kAuthoritativeSessionCount = 65;
    size_t authoritative_sessions = 0;
    bool sequence_valid = baseline_daemon_fds > 0;
    bool first_prepare = false;
    bool first_ready = false;
    bool first_connected = false;
    bool first_arm_sent = false;
    bool first_armed = false;
    bool first_stalled_peer_opened = false;
    bool first_concurrent_stalled_peers_opened = false;
    bool first_arm_bypassed_stalled_peer = false;
    bool first_cache_session_sent = false;
    bool first_adopted_live = false;
    for (size_t index = 0; sequence_valid &&
                           index != kAuthoritativeSessionCount; ++index) {
        const uint32_t wire_id = static_cast<uint32_t>(7101 + index);
        const uint64_t nonce = UINT64_C(0x7101000000000001) + index;
        const bool prepared = scheduler && scheduler->send_msg(
            AssignPrepareMsg(epoch, wire_id, nonce, 1));
        if (index == 0) first_prepare = prepared;
        if (!prepared) {
            sequence_valid = false;
            break;
        }

        Msg *ready_message = wait_for_type(scheduler, Msg::ASSIGN_READY, 5000);
        auto *ready = dynamic_cast<AssignReadyMsg *>(ready_message);
        const bool ready_valid = ready != nullptr && ready->wire_id == wire_id &&
            ready->epoch() == epoch && ready->nonce() == nonce;
        if (index == 0) first_ready = ready_valid;
        delete ready_message;
        if (!ready_valid) {
            sequence_valid = false;
            break;
        }

        int stalled_protocol_fd = -1;
        std::vector<int> concurrent_stalled_fds;
        Clock::time_point arm_admission_started{};
        if (index == 0) {
            const bool stop_sent = ::kill(daemon_pid, SIGSTOP) == 0;
            int stop_status = 0;
            const bool stopped = stop_sent &&
                ::waitpid(daemon_pid, &stop_status, WUNTRACED) == daemon_pid &&
                WIFSTOPPED(stop_status);
            constexpr size_t kConcurrentStalledPeerCount = 64;
            if (stopped)
                stalled_protocol_fd = connect_raw_tcp(daemon_port);
            if (stalled_protocol_fd >= 0) {
                concurrent_stalled_fds.reserve(kConcurrentStalledPeerCount);
                for (size_t peer = 0; peer != kConcurrentStalledPeerCount; ++peer) {
                    const int fd = connect_raw_tcp(daemon_port);
                    if (fd < 0) break;
                    concurrent_stalled_fds.push_back(fd);
                }
            }
            first_stalled_peer_opened = stalled_protocol_fd >= 0;
            first_concurrent_stalled_peers_opened =
                concurrent_stalled_fds.size() == kConcurrentStalledPeerCount;
            const bool resumed = ::kill(daemon_pid, SIGCONT) == 0;
            if (!stopped || !first_stalled_peer_opened ||
                !first_concurrent_stalled_peers_opened || !resumed) {
                if (stalled_protocol_fd >= 0) ::close(stalled_protocol_fd);
                for (int fd : concurrent_stalled_fds) ::close(fd);
                sequence_valid = false;
                break;
            }
            arm_admission_started = Clock::now();
            // Let the daemon accept the silent socket first. The predecessor
            // then blocks its only event-loop thread in wait_for_protocol().
            ::usleep(50 * 1000);
        }

        MsgChannel *ordinary = connect_tcp_bounded(daemon_port, 5000);
        if (index == 0) first_connected = ordinary != nullptr;
        if (ordinary == nullptr) {
            if (stalled_protocol_fd >= 0) ::close(stalled_protocol_fd);
            for (int fd : concurrent_stalled_fds) ::close(fd);
            sequence_valid = false;
            break;
        }
        const P50SourceArmFields arm = source_arm(
            wire_id, epoch, nonce, static_cast<uint32_t>(daemon_port),
            static_cast<uint32_t>(daemon_port));
        const bool arm_sent = ordinary->send_msg(P50SourceArmMsg(arm));
        if (index == 0) first_arm_sent = arm_sent;
        if (!arm_sent) {
            if (stalled_protocol_fd >= 0) ::close(stalled_protocol_fd);
            for (int fd : concurrent_stalled_fds) ::close(fd);
            delete ordinary;
            sequence_valid = false;
            break;
        }
        Msg *armed_message = wait_for_type(ordinary, Msg::P50_SOURCE_ARMED, 5000);
        auto *armed = dynamic_cast<P50SourceArmedMsg *>(armed_message);
        const bool armed_valid = armed != nullptr && armed->arm == arm &&
            armed->f_store_generation != 0;
        if (index == 0) {
            first_armed = armed_valid;
            const auto arm_admission_elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - arm_admission_started).count();
            first_arm_bypassed_stalled_peer =
                armed_valid && arm_admission_elapsed < 4000;
        }
        delete armed_message;
        if (stalled_protocol_fd >= 0) ::close(stalled_protocol_fd);
        for (int fd : concurrent_stalled_fds) ::close(fd);
        if (!armed_valid) {
            delete ordinary;
            sequence_valid = false;
            break;
        }
        const bool cache_session_sent = ordinary->send_msg(CacheSessionMsg());
        if (index == 0) first_cache_session_sent = cache_session_sent;
        if (!cache_session_sent) {
            delete ordinary;
            sequence_valid = false;
            break;
        }
        // CACHE_SESSION changes this descriptor from framed icecream messages
        // to raw CacheWire.  A shadow-owner capacity refusal closes it here;
        // an accepted authoritative handoff remains live until this test ends
        // the intentionally empty raw session.
        const bool adopted_live = no_terminal_socket_event(ordinary, 50);
        if (index == 0) first_adopted_live = adopted_live;
        delete ordinary;
        if (!adopted_live) {
            sequence_valid = false;
            break;
        }
        ++authoritative_sessions;
        // Let the endpoint owner consume EOF and release its one live handoff
        // before presenting the next sequential session.
        ::usleep(20000);
    }
    REQUIRE(first_prepare, "source-arm PREPARE reaches the production daemon");
    REQUIRE(first_ready, "production daemon accepts the exact source-arm assignment");
    REQUIRE(first_connected,
            "ordinary Protocol-50 client reached the real public listener");
    REQUIRE(first_arm_sent, "exact source arm entered the production daemon path");
    REQUIRE(first_stalled_peer_opened,
            "silent protocol peer was admitted before the source-arm peer");
    REQUIRE(first_concurrent_stalled_peers_opened,
            "concurrent silent protocol peers were queued before the source-arm peer");
    REQUIRE(first_armed,
            "source-arm owner is acknowledged before CACHE_SESSION");
    REQUIRE(first_arm_bypassed_stalled_peer,
            "source-arm acknowledgement bypasses a silent accepted handshake");
    REQUIRE(first_cache_session_sent,
            "real CACHE_SESSION entered the production daemon path");
    REQUIRE(first_adopted_live,
            "authenticated one-shot handoff keeps the adopted session live");
    REQUIRE(sequence_valid &&
                authoritative_sessions == kAuthoritativeSessionCount,
            "more than 64 sequential authoritative CacheSessions remain accepted");

    // The event-loop conversion preserves the historical 15-second peer
    // budget: silence no longer blocks useful work, but it also cannot retain
    // a pending descriptor indefinitely.
    const int expiry_fd = connect_raw_tcp(daemon_port);
    const auto expiry_started = Clock::now();
    const bool expiry_eof = expiry_fd >= 0 && wait_raw_eof(expiry_fd, 17000);
    const auto expiry_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - expiry_started).count();
    if (expiry_fd >= 0) ::close(expiry_fd);
    REQUIRE(expiry_eof && expiry_elapsed >= 14000 && expiry_elapsed < 17000,
            "silent accepted handshake expires on the preserved bounded lifetime");

    const int settled_daemon_fds = wait_for_fd_count_at_most(
        daemon_pid, baseline_daemon_fds + 2, 5000);
    REQUIRE(settled_daemon_fds >= 0 &&
                settled_daemon_fds <= baseline_daemon_fds + 2,
            "authoritative CacheSessions leave no retained P5FS descriptors");
    Msg *spurious_login = wait_for_type(scheduler, Msg::LOGIN, 250);
    REQUIRE(spurious_login == nullptr,
            "accepted handoff keeps the READY advertisement stable");
    delete spurious_login;

    if (std::getenv("ICECC_TEST_SCHEDULER_BACKPRESSURE") != nullptr) {
        /*
         * This is deliberately opt-in: the fake scheduler stops reading its
         * established socket while the daemon processes authenticated
         * AssignPrepare requests.  AssignReady is emitted through the real
         * Daemon::send_scheduler() path, so a full scheduler receive window
         * exercises the same blocking flush as compile completion.  A fresh
         * protocol peer must still receive the daemon's greeting within the
         * existing five-second admission budget.
         *
         * The test is expected to fail against a daemon that blocks in
         * flush_writebuf(); the isolated daemon is terminated after the
         * bounded probe so the flood thread cannot outlive this test.
         */
        constexpr size_t kFloodCount = 20000;
        std::atomic<size_t> flood_sent{0};
        std::atomic<bool> flood_started{false};
        std::thread flood([&] {
            flood_started.store(true);
            for (size_t index = 0; index != kFloodCount; ++index) {
                const uint32_t wire_id = static_cast<uint32_t>(0x7f000000u + index);
                const uint64_t nonce = UINT64_C(0x7f00000000000001) + index;
                if (!scheduler || !scheduler->send_msg(
                        AssignPrepareMsg(epoch, wire_id, nonce, 1)))
                    break;
                ++flood_sent;
            }
        });
        while (!flood_started.load()) ::usleep(1000);
        const char *eagain_marker = std::getenv("ICECC_TEST_SEND_EAGAIN_MARKER");
        const auto eagain_deadline = Clock::now() + std::chrono::milliseconds(5000);
        while (eagain_marker && *eagain_marker && Clock::now() < eagain_deadline) {
            std::error_code marker_error;
            if (std::filesystem::file_size(eagain_marker, marker_error) > 0 &&
                !marker_error)
                break;
            ::usleep(10000);
        }
        std::error_code eagain_error;
        const uintmax_t eagain_count = eagain_marker && *eagain_marker
            ? std::filesystem::file_size(eagain_marker, eagain_error) : 0;
        const bool observed_backpressure = !eagain_error && eagain_count > 0;
        MsgChannel *probe = observed_backpressure ? Service::createChannelUntil(
            "127.0.0.1", static_cast<unsigned short>(daemon_port),
            Clock::now() + std::chrono::milliseconds(5000)) : nullptr;
        const bool greeting_valid = probe != nullptr;
        delete probe;
        REQUIRE(flood_sent.load() > 0,
                "scheduler backpressure flood entered the real daemon path");
        REQUIRE(observed_backpressure,
                "child daemon observed send-side EAGAIN under scheduler backpressure");
        REQUIRE(greeting_valid,
                "fresh protocol admission completes within five seconds after confirmed scheduler backpressure");
        if (greeting_valid && observed_backpressure) {
            flood.join();
            REQUIRE(flood_sent.load() == kFloodCount,
                    "finite scheduler input flood completed without loss");
            if (::getenv("ICECC_TEST_SCHEDULER_BACKPRESSURE_EXPIRE") != nullptr) {
                REQUIRE(wait_attachment_log(log, 0,
                            "scheduler deferred output deadline expired", 33000),
                        "silent scheduler backlog expires on its existing deadline");
            } else {
                size_t replies = 0;
                bool intact = true;
                const auto drain_deadline = Clock::now() + std::chrono::seconds(25);
                while (replies < kFloodCount && Clock::now() < drain_deadline) {
                    Msg *reply = scheduler->get_msg(1, true);
                    if (!reply) {
                        if (scheduler->at_eof()) break;
                        continue;
                    }
                    if (*reply == Msg::ASSIGN_READY) {
                        const auto *ready = dynamic_cast<const AssignReadyMsg *>(reply);
                        intact = intact && ready && ready->epoch() == epoch &&
                            ready->wire_id == static_cast<uint32_t>(0x7f000000u + replies) &&
                            ready->nonce() == UINT64_C(0x7f00000000000001) + replies;
                        ++replies;
                    }
                    delete reply;
                }
                REQUIRE(intact && replies == kFloodCount,
                        "all deferred scheduler replies drain intact in FIFO order");
            }
        }
        ::kill(daemon_pid, SIGKILL);
        int backpressure_status = 0;
        (void)::waitpid(daemon_pid, &backpressure_status, 0);
        if (flood.joinable()) flood.join();
        delete scheduler;
        ::close(scheduler_listener);
        std::fprintf(stderr, "retained backpressure work directory: %s; marker=%s; count=%ju; sent=%zu\n",
                     work.c_str(), eagain_marker ? eagain_marker : "missing",
                     eagain_count, flood_sent.load());
        return failures ? 1 : 0;
    }

    const int shutdown_pending_fd = connect_raw_tcp(daemon_port);
    ::usleep(50 * 1000);
    ::kill(daemon_pid, SIGTERM);
    Msg *shutdown_message = wait_for_type(scheduler, Msg::LOGIN, 5000);
    LoginMsg *shutdown_login = dynamic_cast<LoginMsg *>(shutdown_message);
    REQUIRE(absent(shutdown_login), "orderly shutdown withdraws before scheduler teardown");
    delete shutdown_message;
    const bool shutdown_pending_closed = shutdown_pending_fd >= 0 &&
        wait_raw_eof(shutdown_pending_fd, 5000);
    if (shutdown_pending_fd >= 0) ::close(shutdown_pending_fd);
    REQUIRE(shutdown_pending_closed,
            "orderly shutdown closes a still-pending protocol admission");

    int status = 0;
    bool reaped = wait_child(daemon_pid, 10000, &status);
    if (!reaped) {
        ::kill(daemon_pid, SIGKILL);
        (void)::waitpid(daemon_pid, &status, 0);
    }
    REQUIRE(reaped, "real positive iceccd exits under the shutdown bound");
    REQUIRE(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "real positive iceccd exits cleanly");

    delete scheduler;
    ::close(scheduler_listener);
    if (failures == 0) std::filesystem::remove_all(work);
    else std::fprintf(stderr, "retained failing work directory: %s\n", work.c_str());
    return failures ? 1 : 0;
#endif
}
