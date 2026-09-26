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
#include "../cache/p50_sidecar_identity.h"
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
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <future>
#include <sstream>
#include <limits>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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

static Msg *wait_for_any_type(MsgChannel *channel, int timeout_msec)
{
    if (!channel) return nullptr;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        Msg *message = channel->get_msg(1, true);
        if (message != nullptr) return message;
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
    P51CommitReceiptGate(int endpoint_port, uid_t sidecar_uid, size_t expected,
                         uint64_t first_ordinal = 1,
                         std::string abort_path = {})
        : endpoint_port_(endpoint_port), sidecar_uid_(sidecar_uid), expected_(expected),
          first_ordinal_(first_ordinal), abort_path_(std::move(abort_path))
    {
        listener_fd_ = listen_ephemeral(&proxy_port_);
        if (listener_fd_ < 0 || proxy_port_ <= 0) return;
        const auto rule = [&](const char *action) {
            return run_iptables_rule({"-t", "nat", action, "OUTPUT", "-p", "tcp",
                "--dport", std::to_string(endpoint_port_),
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
                "--dport", std::to_string(endpoint_port_),
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

    std::optional<icecc::p50::R2TxCommit> first_commit_witness() const
    {
        std::lock_guard lock(mutex_);
        if (commits_.empty() || commits_.front().size() < 4) return std::nullopt;
        try {
            const auto decoded = icecc::p50::decode_payload(
                icecc::p50::MessageType::R2_TX_COMMIT,
                std::span<const uint8_t>(commits_.front().data() + 4,
                                         commits_.front().size() - 4));
            return std::get<icecc::p50::R2TxCommit>(decoded);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::vector<icecc::p50::R2TxCommit> commit_witnesses() const
    {
        std::lock_guard lock(mutex_);
        std::vector<icecc::p50::R2TxCommit> result;
        result.reserve(commits_.size());
        for (const auto& frame : commits_) {
            if (frame.size() < 4) return {};
            try {
                const auto decoded = icecc::p50::decode_payload(
                    icecc::p50::MessageType::R2_TX_COMMIT,
                    std::span<const uint8_t>(frame.data() + 4, frame.size() - 4));
                result.push_back(std::get<icecc::p50::R2TxCommit>(decoded));
            } catch (...) {
                return {};
            }
        }
        return result;
    }

    bool wait_for_commits(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        const auto deadline = Clock::now() + timeout;
        bool woke = false;
        while (!failed_ && commits_.size() < expected_) {
            if (abort_requested()) {
                stop_.store(true, std::memory_order_release);
                failed_.store(true, std::memory_order_release);
                release_ = true;
                changed_.notify_all();
                break;
            }
            const auto now = Clock::now();
            if (now >= deadline) break;
            changed_.wait_until(lock, std::min(deadline, now +
                std::chrono::milliseconds(100)));
        }
        woke = failed_ || commits_.size() >= expected_;
        const bool ordinal_range_valid = !ordinals_.empty() &&
            (*ordinals_.rbegin() - *ordinals_.begin() + 1 == expected_) &&
            (first_ordinal_ == 0 || *ordinals_.begin() == first_ordinal_);
        const bool valid = woke && !failed_ && expected_ != 0 &&
            commits_.size() == expected_ && ordinals_.size() == expected_ &&
            ordinal_range_valid;
        if (!valid)
            std::fprintf(stderr,
                "P51_RECEIPT_GATE_FAIL port=%d expected=%zu first=%llu woke=%u failed=%u commits=%zu peak=%zu ordinal_count=%zu ordinal_min=%llu ordinal_max=%llu\n",
                endpoint_port_, expected_, static_cast<unsigned long long>(first_ordinal_),
                woke ? 1u : 0u, failed_.load(std::memory_order_acquire), commits_.size(),
                peak_commits_, ordinals_.size(),
                ordinals_.empty() ? 0ull : static_cast<unsigned long long>(*ordinals_.begin()),
                ordinals_.empty() ? 0ull : static_cast<unsigned long long>(*ordinals_.rbegin()));
        return valid;
    }

    bool rearm(size_t expected, uint64_t first_ordinal)
    {
        std::lock_guard lock(mutex_);
        if (failed_ || stop_.load(std::memory_order_acquire) ||
            !commits_.empty() ||
            (expected != 0 && first_ordinal != 0 &&
             first_ordinal > UINT64_MAX - (expected - 1)))
            return false;
        expected_ = expected;
        first_ordinal_ = first_ordinal;
        peak_commits_ = 0;
        ordinals_.clear();
        release_ = false;
        discard_ = false;
        return true;
    }

    void release_commits()
    {
        {
            std::lock_guard lock(mutex_);
            release_ = true;
        }
        changed_.notify_all();
    }

    void discard_held_commits()
    {
        {
            std::lock_guard lock(mutex_);
            discard_ = true;
            release_ = true;
        }
        changed_.notify_all();
    }

private:
    bool abort_requested() const
    {
        if (abort_path_.empty()) return false;
        std::error_code error;
        return std::filesystem::exists(abort_path_, error) && !error;
    }

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
            bool link_state_reported = false;
            while (!stop_.load(std::memory_order_acquire) && !failed_) {
                if (!read_relay_bytes(server_fd_, header, sizeof(header))) break;
                const uint32_t word = (uint32_t(header[0]) << 24) |
                    (uint32_t(header[1]) << 16) |
                    (uint32_t(header[2]) << 8) | uint32_t(header[3]);
                const uint8_t type_byte = header[0];
                if (!r2 && type_byte == static_cast<uint8_t>(
                        icecc::p50::MessageType::LINK_STATE)) {
                    r2 = true;
                    if (!link_state_reported) {
                        std::fprintf(stderr,
                            "P51_RECEIPT_GATE_LINK_STATE port=%d\n",
                            endpoint_port_);
                        link_state_reported = true;
                    }
                }
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
                        if (expected_ == 0 ||
                            (first_ordinal_ != 0 &&
                             commit.relationship_ordinal < first_ordinal_)) {
                            std::fprintf(stderr,
                                "P51_RECEIPT_GATE_PASSTHROUGH port=%d ordinal=%llu tu_seq=%llu\n",
                                endpoint_port_,
                                static_cast<unsigned long long>(commit.relationship_ordinal),
                                static_cast<unsigned long long>(commit.inner.tu_seq.value));
                            lock.unlock();
                            if (!write_relay_bytes(client_fd_, frame.data(), frame.size()))
                                break;
                            continue;
                        }
                        if (!ordinals_.insert(commit.relationship_ordinal).second) {
                            failed_ = true;
                            changed_.notify_all();
                            break;
                        }
                        if (commits_.size() >= expected_) {
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
                                failed_ || discard_) break;
                            for (const auto& held : commits_) {
                                if (!write_relay_bytes(
                                        client_fd_, held.data(), held.size())) {
                                    failed_ = true;
                                    break;
                                }
                            }
                            commits_.clear();
                            ordinals_.clear();
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
    uint64_t first_ordinal_ = 1;
    std::string abort_path_;
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
    bool discard_ = false;
    std::atomic<bool> failed_{false};
    std::atomic<bool> stop_{false};
};

static bool p51_gate_wait_for_path(const std::string& path,
                                   std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        std::error_code error;
        const auto abort_path = std::filesystem::path(path).parent_path() / "abort";
        if (std::filesystem::exists(abort_path, error) && !error) return false;
        error.clear();
        if (std::filesystem::exists(path, error) && !error) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

static bool p51_gate_write_marker(const std::string& path,
                                  const std::string& contents = "ok\n")
{
    const std::string temporary = path + ".tmp-" + std::to_string(::getpid());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << contents;
        output.flush();
        if (!output.good()) {
            output.close();
            (void)::unlink(temporary.c_str());
            return false;
        }
    }
    if (::rename(temporary.c_str(), path.c_str()) == 0) return true;
    (void)::unlink(temporary.c_str());
    return false;
}

static bool p51_read_settlement_ack(const std::string& path,
                                    uint64_t expected_request)
{
    std::ifstream input(path, std::ios::binary);
    std::string line;
    if (!std::getline(input, line)) return false;
    unsigned long long request = 0;
    unsigned explicit_release = 0;
    char trailing = '\0';
    return std::sscanf(line.c_str(), "request=%llu explicit=%u%c", &request,
                       &explicit_release, &trailing) == 2 &&
        request == expected_request && explicit_release == 1;
}

static bool p51_wait_settlement_ack(const std::string& path,
                                    uint64_t expected_request,
                                    std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    do {
        if (p51_read_settlement_ack(path, expected_request)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (Clock::now() < deadline);
    return false;
}

static bool p51_gate_wait_rearm(P51CommitReceiptGate& gate, size_t expected,
                                uint64_t first_ordinal,
                                const std::string& abort_path)
{
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < deadline) {
        std::error_code error;
        if (std::filesystem::exists(abort_path, error) && !error) return false;
        if (gate.rearm(expected, first_ordinal)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static int run_p51_commit_receipt_gate(int endpoint_port, uid_t sidecar_uid,
                                       size_t expected, uint64_t first_ordinal,
                                       const std::string& control_dir,
                                       bool one_shot = false)
{
    if (endpoint_port <= 0 || sidecar_uid == 0 || expected == 0 ||
        expected > 30 || control_dir.empty())
        return 2;
    std::error_code error;
    if (!std::filesystem::create_directories(control_dir, error) && error)
        return 2;
    P51CommitReceiptGate gate(endpoint_port, sidecar_uid, expected, first_ordinal,
                              control_dir + "/abort");
    if (!gate.ready() || !p51_gate_write_marker(control_dir + "/ready")) {
        (void)p51_gate_write_marker(control_dir + "/failed", "gate setup failed\n");
        return 1;
    }
    const auto wait_stage = [&](unsigned stage) {
        if (!gate.wait_for_commits(std::chrono::seconds(30)))
            return false;
        const auto witnesses = gate.commit_witnesses();
        if (witnesses.size() != expected) return false;
        uint64_t first = UINT64_MAX;
        uint64_t last = 0;
        for (const auto& witness : witnesses) {
            first = std::min(first, witness.relationship_ordinal);
            last = std::max(last, witness.relationship_ordinal);
        }
        const std::string summary = "count=" + std::to_string(witnesses.size()) +
            " first_ordinal=" + std::to_string(first) +
            " last_ordinal=" + std::to_string(last) + "\n";
        if (last - first + 1 != expected || !p51_gate_write_marker(
                control_dir + "/held-" + std::to_string(stage), summary))
            return false;
        const bool released = p51_gate_wait_for_path(
            control_dir + "/release-" + std::to_string(stage),
            std::chrono::seconds(30));
        if (!released) {
            std::error_code error;
            if (std::filesystem::exists(control_dir + "/abort", error) && !error) {
                gate.discard_held_commits();
                (void)p51_gate_write_marker(
                    control_dir + "/discarded-" + std::to_string(stage),
                    "explicit abort discarded held COMMIT interval\n");
            }
        }
        return released;
    };
    if (!wait_stage(1)) {
        (void)p51_gate_write_marker(control_dir + "/failed", "first receipt window failed\n");
        return 1;
    }
    gate.release_commits();
    if (!p51_gate_wait_rearm(gate, 0, 0, control_dir + "/abort") ||
        !p51_gate_write_marker(control_dir + "/released-1")) {
        (void)p51_gate_write_marker(control_dir + "/failed", "first release did not settle\n");
        return 1;
    }
    if (one_shot) {
        if (!p51_gate_wait_for_path(control_dir + "/finish", std::chrono::seconds(180))) {
            (void)p51_gate_write_marker(control_dir + "/failed", "one-shot batch did not finish before gate shutdown\n");
            return 1;
        }
        return 0;
    }
    if (!p51_gate_wait_for_path(control_dir + "/arm-2", std::chrono::seconds(30)) ||
        !p51_gate_wait_rearm(gate, expected, 0, control_dir + "/abort") ||
        !p51_gate_write_marker(control_dir + "/armed-2")) {
        (void)p51_gate_write_marker(control_dir + "/failed", "second receipt window did not arm\n");
        return 1;
    }
    if (!wait_stage(2)) {
        (void)p51_gate_write_marker(control_dir + "/failed", "second receipt window failed\n");
        return 1;
    }
    gate.release_commits();
    if (!p51_gate_wait_rearm(gate, 0, 0, control_dir + "/abort") ||
        !p51_gate_write_marker(control_dir + "/released-2")) {
        (void)p51_gate_write_marker(control_dir + "/failed", "second release did not settle\n");
        return 1;
    }
    if (!p51_gate_wait_for_path(control_dir + "/finish", std::chrono::seconds(180))) {
        (void)p51_gate_write_marker(control_dir + "/failed", "fresh batch did not finish before gate shutdown\n");
        return 1;
    }
    return 0;
}

/* Multi-connection receipt gate for real CxF W30 topology checks.  Unlike the
   single-link gate above, this accepts every C/F persistent TCP relationship
   to one F endpoint and independently holds one relation's 30 COMMITs while
   forwarding the other relations. */
class P51MultiLinkCommitGate {
public:
    P51MultiLinkCommitGate(int endpoint_port, uid_t sidecar_uid,
                           size_t expected_links, size_t jobs_per_link,
                           bool hold_one_relationship = true,
                           bool allow_followup_after_release = false)
        : endpoint_port_(endpoint_port), sidecar_uid_(sidecar_uid),
          expected_links_(expected_links), jobs_per_link_(jobs_per_link),
          hold_one_relationship_(hold_one_relationship),
          allow_followup_after_release_(allow_followup_after_release)
    {
        listener_fd_ = listen_ephemeral(&proxy_port_);
        if (listener_fd_ < 0 || proxy_port_ <= 0 || expected_links_ == 0 ||
            expected_links_ > 4 || jobs_per_link_ == 0 || jobs_per_link_ > 30)
            return;
        rule_installed_ = run_iptables_rule({"-t", "nat", "-A", "OUTPUT", "-p",
            "tcp", "-d", "127.0.0.1", "--dport", std::to_string(endpoint_port_),
            "-m", "owner", "--uid-owner", std::to_string(sidecar_uid_),
            "-j", "REDIRECT", "--to-ports", std::to_string(proxy_port_)});
        if (rule_installed_)
            accept_thread_ = std::thread([this] { accept_connections(); });
    }

    P51MultiLinkCommitGate(const P51MultiLinkCommitGate&) = delete;
    P51MultiLinkCommitGate& operator=(const P51MultiLinkCommitGate&) = delete;

    ~P51MultiLinkCommitGate()
    {
        stop_.store(true, std::memory_order_release);
        {
            std::lock_guard lock(mutex_);
            release_held_ = true;
        }
        changed_.notify_all();
        if (listener_fd_ >= 0) {
            (void)::shutdown(listener_fd_, SHUT_RDWR);
        }
        if (accept_thread_.joinable()) accept_thread_.join();
        if (listener_fd_ >= 0) {
            ::close(listener_fd_);
            listener_fd_ = -1;
        }
        {
            std::lock_guard lock(mutex_);
            for (const auto& pair : sockets_) {
                (void)::shutdown(pair.first, SHUT_RDWR);
                (void)::shutdown(pair.second, SHUT_RDWR);
            }
        }
        for (auto& thread : connection_threads_)
            if (thread.joinable()) thread.join();
        for (const auto& pair : sockets_) {
            if (pair.first >= 0) ::close(pair.first);
            if (pair.second >= 0) ::close(pair.second);
        }
        if (rule_installed_)
            (void)run_iptables_rule({"-t", "nat", "-D", "OUTPUT", "-p", "tcp",
                "-d", "127.0.0.1", "--dport", std::to_string(endpoint_port_),
                "-m", "owner", "--uid-owner", std::to_string(sidecar_uid_),
                "-j", "REDIRECT", "--to-ports", std::to_string(proxy_port_)});
    }

    bool ready() const noexcept { return listener_fd_ >= 0 && rule_installed_; }

    bool wait_for_all_links(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] {
            if (failed_ || relation_commits_.size() > expected_links_) return true;
            if (relation_commits_.size() < expected_links_) return false;
            for (const auto& [id, state] : relation_commits_) {
                (void)id;
                if (!state.complete) return false;
            }
            return true;
        }) && !failed_ && relation_commits_.size() == expected_links_ &&
            std::all_of(relation_commits_.begin(), relation_commits_.end(),
                [](const auto& pair) { return pair.second.complete; });
    }

    std::optional<icecc::p50::Id128> held_relationship() const
    {
        std::lock_guard lock(mutex_);
        return held_relationship_;
    }

    size_t accepted_connections() const
    {
        std::lock_guard lock(mutex_);
        return accepted_connections_;
    }

    size_t established_link_states() const
    {
        std::lock_guard lock(mutex_);
        return established_link_states_;
    }

    size_t peak_active_links() const
    {
        std::lock_guard lock(mutex_);
        return peak_active_links_;
    }

    size_t followup_commits() const
    {
        std::lock_guard lock(mutex_);
        return followup_commits_;
    }

    bool wait_for_healthy_links(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] {
            if (failed_) return true;
            if (!held_relationship_) return false;
            size_t healthy = 0;
            for (const auto& [id, state] : relation_commits_)
                if (id != *held_relationship_ && state.released) ++healthy;
            return healthy + 1 == expected_links_;
        }) && !failed_ && held_relationship_.has_value() &&
            [&] {
                size_t healthy = 0;
                for (const auto& [id, state] : relation_commits_)
                    if (id != *held_relationship_ && state.released) ++healthy;
                return healthy + 1 == expected_links_;
            }();
    }

    size_t peak_commits(const icecc::p50::Id128& relation) const
    {
        std::lock_guard lock(mutex_);
        const auto found = relation_commits_.find(relation);
        return found == relation_commits_.end() ? 0 : found->second.peak;
    }

    void dump_state(size_t f_index) const
    {
        std::lock_guard lock(mutex_);
        std::fprintf(stderr,
                     "P51 multilink diagnostic F%zu attempts=%zu expected=%zu "
                     "link_states=%zu peak_active=%zu failed=%d relations=%zu "
                     "held=%d released=%d\n",
                     f_index, accepted_connections_, expected_links_,
                     established_link_states_, peak_active_links_,
                     failed_.load(std::memory_order_acquire),
                     relation_commits_.size(), held_relationship_.has_value(),
                     release_held_);
        for (const auto& [id, state] : relation_commits_) {
            std::fprintf(stderr,
                         "P51 multilink diagnostic F%zu relation=%02x%02x "
                         "cguid=%02x%02x cgen=%llu commits=%zu peak=%zu complete=%d released=%d\n",
                         f_index, id.bytes[0], id.bytes[1],
                         state.c_store_guid[0], state.c_store_guid[1],
                         static_cast<unsigned long long>(state.c_store_generation),
                         state.ordinals.size(),
                         state.peak, state.complete, state.released);
        }
    }

    void release_held()
    {
        {
            std::lock_guard lock(mutex_);
            release_held_ = true;
        }
        changed_.notify_all();
    }

private:
    struct RelationCommitState {
        std::set<uint64_t> ordinals;
        std::vector<std::vector<uint8_t>> frames;
        std::array<uint8_t, 16> c_store_guid{};
        uint64_t c_store_generation = 0;
        size_t peak = 0;
        bool complete = false;
        bool released = false;
    };

    bool write_bytes(int fd, const void *buffer, size_t size)
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

    bool read_bytes(int fd, void *buffer, size_t size)
    {
        auto *position = static_cast<unsigned char *>(buffer);
        while (size != 0 && !stop_.load(std::memory_order_acquire)) {
            pollfd descriptor{fd, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (ready > 0 &&
                (descriptor.revents & POLLNVAL)) ||
                (ready > 0 && !(descriptor.revents & POLLIN) &&
                 (descriptor.revents & (POLLERR | POLLHUP))))
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

    void accept_connections()
    {
        size_t accepted = 0;
        const size_t attempt_limit = expected_links_ + 8;
        while (!stop_.load(std::memory_order_acquire)) {
            pollfd descriptor{listener_fd_, POLLIN, 0};
            int ready = -1;
            do { ready = ::poll(&descriptor, 1, 100); }
            while (ready < 0 && errno == EINTR);
            if (stop_.load(std::memory_order_acquire)) return;
            if (ready == 0) continue;
            if (ready < 0) { fail(); return; }
            const int client_fd = ::accept(listener_fd_, nullptr, nullptr);
            if (client_fd < 0) { fail(); return; }
            sockaddr_in peer{};
            socklen_t peer_length = sizeof(peer);
            if (::getpeername(client_fd, reinterpret_cast<sockaddr*>(&peer),
                              &peer_length) != 0) {
                ::close(client_fd);
                fail();
                return;
            }
            const int server_fd = connect_raw_tcp(endpoint_port_);
            if (server_fd < 0) {
                ::close(client_fd);
                fail();
                return;
            }
            {
                std::lock_guard lock(mutex_);
                sockets_.emplace_back(client_fd, server_fd);
                ++accepted_connections_;
            }
            const size_t attempt = ++accepted;
            std::fprintf(stderr,
                "P51_MULTILINK_TCP_ACCEPT attempt=%zu peer_port=%u expected=%zu\n",
                attempt, static_cast<unsigned>(ntohs(peer.sin_port)),
                expected_links_);
            connection_threads_.emplace_back(
                [this, client_fd, server_fd, attempt] {
                    relay_connection(client_fd, server_fd, attempt);
                });
            if (attempt >= attempt_limit) {
                fail();
                return;
            }
        }
        changed_.notify_all();
    }

    void relay_connection(int client_fd, int server_fd, size_t attempt)
    {
        std::thread client_to_server([&] {
            char bytes[8192];
            for (;;) {
                if (stop_.load(std::memory_order_acquire)) break;
                pollfd descriptor{client_fd, POLLIN, 0};
                const int ready = ::poll(&descriptor, 1, 100);
                if (ready < 0 && errno == EINTR) continue;
                if (ready < 0 || (ready > 0 &&
                    (descriptor.revents & (POLLERR | POLLNVAL)))) break;
                if (ready == 0) continue;
                const ssize_t count = ::recv(client_fd, bytes, sizeof(bytes), 0);
                if (count > 0) {
                    if (!write_bytes(server_fd, bytes, static_cast<size_t>(count))) break;
                    continue;
                }
                if (count < 0 && errno == EINTR) continue;
                break;
            }
            (void)::shutdown(server_fd, SHUT_WR);
        });

        unsigned char header[4];
        bool saw_link_state = false;
        if (!read_bytes(server_fd, header, sizeof(header)) ||
            !write_bytes(client_fd, header, sizeof(header)) ||
            !read_bytes(server_fd, header, sizeof(header)) ||
            !(header[0] == 51 && header[1] == 0 && header[2] == 0 && header[3] == 0) ||
            !write_bytes(client_fd, header, sizeof(header))) {
            fail();
        } else {
            bool r2 = false;
            std::optional<icecc::p50::Id128> relation;
            while (!stop_.load(std::memory_order_acquire) && !failed_) {
                if (!read_bytes(server_fd, header, sizeof(header))) break;
                const uint8_t type = header[0];
                const uint32_t word = (uint32_t(header[0]) << 24) |
                    (uint32_t(header[1]) << 16) |
                    (uint32_t(header[2]) << 8) | uint32_t(header[3]);
                if (!r2 && type == static_cast<uint8_t>(
                        icecc::p50::MessageType::LINK_STATE))
                    r2 = true;
                uint32_t payload_bytes = word;
                if (r2) {
                    try {
                        payload_bytes = icecc::p50::decode_frame_header(
                            std::span<const uint8_t>(header, sizeof(header))).payload_bytes;
                    } catch (...) { fail(); break; }
                } else if (payload_bytes > (1u << 20)) {
                    fail();
                    break;
                }
                std::vector<uint8_t> frame(header, header + sizeof(header));
                frame.resize(4 + payload_bytes);
                if (payload_bytes != 0 && !read_bytes(
                        server_fd, frame.data() + 4, payload_bytes)) break;
                try {
                    if (type == static_cast<uint8_t>(icecc::p50::MessageType::LINK_STATE)) {
                        const auto decoded = icecc::p50::decode_payload(
                            icecc::p50::MessageType::LINK_STATE,
                            std::span<const uint8_t>(frame.data() + 4, payload_bytes));
                        const auto& state = std::get<icecc::p50::LinkState>(decoded);
                        relation = state.relationship_id;
                        saw_link_state = true;
                        std::lock_guard lock(mutex_);
                        auto [entry, inserted] =
                            relation_commits_.try_emplace(*relation);
                        if (inserted) {
                            entry->second.c_store_guid = state.c_store_guid.bytes;
                            entry->second.c_store_generation = state.c_store_generation;
                        } else if (entry->second.c_store_guid !=
                                       state.c_store_guid.bytes ||
                                   entry->second.c_store_generation !=
                                       state.c_store_generation) {
                            failed_ = true;
                        }
                        const auto active = active_link_attempts_.find(*relation);
                        if (active != active_link_attempts_.end() &&
                            active->second != attempt) {
                            // A healthy relationship may have only one active
                            // physical stream. Sequential recovery is allowed
                            // after the earlier relay has been retired.
                            failed_ = true;
                        } else {
                            active_link_attempts_[*relation] = attempt;
                            attempt_relationships_[attempt] = *relation;
                            if (!attempt_counted_link_state_.contains(attempt)) {
                                attempt_counted_link_state_.insert(attempt);
                                ++established_link_states_;
                            }
                            peak_active_links_ = std::max(
                                peak_active_links_, active_link_attempts_.size());
                        }
                        std::fprintf(stderr,
                            "P51_MULTILINK_LINK_STATE attempt=%zu relation=%02x%02x "
                            "cguid=%02x%02x cgen=%llu phys=%llu\n",
                            attempt, state.relationship_id.bytes[0],
                            state.relationship_id.bytes[1],
                            state.c_store_guid.bytes[0], state.c_store_guid.bytes[1],
                            static_cast<unsigned long long>(state.c_store_generation),
                            static_cast<unsigned long long>(state.physical_link_generation));
                        std::fflush(stderr);
                        if (relation_commits_.size() > expected_links_) {
                            failed_ = true;
                            changed_.notify_all();
                            break;
                        }
                        changed_.notify_all();
                    } else if (type == static_cast<uint8_t>(
                                   icecc::p50::MessageType::R2_TX_COMMIT)) {
                        if (!relation) { fail(); break; }
                        const auto decoded = icecc::p50::decode_payload(
                            icecc::p50::MessageType::R2_TX_COMMIT,
                            std::span<const uint8_t>(frame.data() + 4, payload_bytes));
                        const auto& commit = std::get<icecc::p50::R2TxCommit>(decoded);
                        std::vector<std::vector<uint8_t>> ready_frames;
                        bool hold = false;
                        {
                            std::unique_lock lock(mutex_);
                            auto& state = relation_commits_[*relation];
                            if (!state.ordinals.insert(commit.relationship_ordinal).second) {
                                failed_ = true;
                                changed_.notify_all();
                                break;
                            }
                            if (state.released && allow_followup_after_release_) {
                                ++followup_commits_;
                                ready_frames.emplace_back(std::move(frame));
                            } else if (state.frames.size() >= jobs_per_link_) {
                                failed_ = true;
                                changed_.notify_all();
                                break;
                            } else {
                            state.frames.push_back(std::move(frame));
                            state.peak = std::max(state.peak, state.frames.size());
                            if (state.frames.size() == jobs_per_link_) {
                                state.complete = true;
                                if (hold_one_relationship_ && !held_relationship_)
                                    held_relationship_ = *relation;
                                hold = held_relationship_ && *held_relationship_ == *relation;
                                if (hold) {
                                    changed_.notify_all();
                                    if (!changed_.wait_for(lock, std::chrono::seconds(30), [&] {
                                            return release_held_ || failed_;
                                        }) || failed_) {
                                        failed_ = true;
                                        changed_.notify_all();
                                        break;
                                    }
                                }
                                ready_frames = std::move(state.frames);
                                state.released = true;
                                changed_.notify_all();
                            }
                            }
                        }
                        for (const auto& held : ready_frames)
                            if (!write_bytes(client_fd, held.data(), held.size())) {
                                fail();
                                break;
                            }
                        continue;
                    }
                } catch (...) { fail(); break; }
                if (!write_bytes(client_fd, frame.data(), frame.size())) break;
            }
        }
        if (!saw_link_state) {
            std::fprintf(stderr,
                "P51_MULTILINK_NO_STATE attempt=%zu accepted=%zu\n",
                attempt, accepted_connections());
            std::fflush(stderr);
        }
        (void)::shutdown(client_fd, SHUT_WR);
        (void)::shutdown(server_fd, SHUT_RDWR);
        if (client_to_server.joinable()) client_to_server.join();
        {
            std::lock_guard lock(mutex_);
            const auto attempt_relation = attempt_relationships_.find(attempt);
            if (attempt_relation != attempt_relationships_.end()) {
                const auto active = active_link_attempts_.find(
                    attempt_relation->second);
                if (active != active_link_attempts_.end() &&
                    active->second == attempt)
                    active_link_attempts_.erase(active);
                attempt_relationships_.erase(attempt_relation);
                changed_.notify_all();
            }
        }
    }

    int endpoint_port_ = 0;
    uid_t sidecar_uid_ = 0;
    size_t expected_links_ = 0;
    size_t jobs_per_link_ = 0;
    bool hold_one_relationship_ = true;
    bool allow_followup_after_release_ = false;
    int proxy_port_ = 0;
    int listener_fd_ = -1;
    bool rule_installed_ = false;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::pair<int, int>> sockets_;
    std::vector<std::thread> connection_threads_;
    std::thread accept_thread_;
    size_t accepted_connections_ = 0;
    size_t established_link_states_ = 0;
    size_t peak_active_links_ = 0;
    std::map<icecc::p50::Id128, RelationCommitState> relation_commits_;
    std::map<icecc::p50::Id128, size_t> active_link_attempts_;
    std::map<size_t, icecc::p50::Id128> attempt_relationships_;
    std::set<size_t> attempt_counted_link_state_;
    std::optional<icecc::p50::Id128> held_relationship_;
    bool release_held_ = false;
    size_t followup_commits_ = 0;
    std::atomic<bool> failed_{false};
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

static std::string read_file_tail(const std::string& path, uintmax_t max_bytes)
{
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(path, error);
    if (error)
        return {};
    return read_file_suffix(path, size > max_bytes ? size - max_bytes : 0);
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
        if (state != 'Z' && !wanted.empty() && bytes.find(wanted) != std::string::npos)
            return pid;
    }
    return -1;
}

static std::vector<pid_t> direct_children(pid_t parent)
{
    std::ifstream children("/proc/" + std::to_string(parent) + "/task/" +
                           std::to_string(parent) + "/children");
    std::vector<pid_t> result;
    pid_t child = -1;
    while (children >> child)
        result.push_back(child);
    return result;
}

static uint64_t process_start_time_ticks(pid_t process)
{
    std::ifstream stat("/proc/" + std::to_string(process) + "/stat");
    std::string line;
    if (!std::getline(stat, line)) return 0;
    const size_t comm_end = line.rfind(')');
    if (comm_end == std::string::npos || comm_end + 2 >= line.size()) return 0;
    std::istringstream fields(line.substr(comm_end + 2));
    std::string field;
    for (unsigned number = 3; number <= 22; ++number) {
        if (!(fields >> field)) return 0;
        if (number == 22) {
            char *end = nullptr;
            errno = 0;
            const unsigned long long ticks = std::strtoull(field.c_str(), &end, 10);
            if (errno != 0 || end == field.c_str() || *end != '\0') return 0;
            return static_cast<uint64_t>(ticks);
        }
    }
    return 0;
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
    if (::getenv("ICECC_TEST_CAPTURE_DAEMON_STDERR") != nullptr) {
        const int log_fd = ::open(log.c_str(), O_WRONLY | O_APPEND);
        if (log_fd >= 0) {
            (void)::dup2(log_fd, STDERR_FILENO);
            if (log_fd != STDERR_FILENO) ::close(log_fd);
        }
    }
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
    uint32_t f_port, int source_fd, uint32_t profile_mask,
    P51SourceArmedFields *observed_armed = nullptr,
    std::atomic<bool> *observed_armed_ready = nullptr,
    Clock::time_point *source_deadline_out = nullptr,
    bool stop_after_arm = false,
    P50SourceArmFields *observed_arm = nullptr,
    P51CacheControlIdentity *observed_control_identity = nullptr)
{
    using namespace icecc::p50;
    using namespace icecc::p50::local;
    P50SourceTransferResult failed{};
    const char *stage = "protocol";
    const auto fail_at = [&](const char *where) {
        std::fprintf(stderr, "P51_KIND8_FAIL job=%u stage=%s\n", wire_id, where);
        return failed;
    };
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    if (source_deadline_out != nullptr) *source_deadline_out = deadline;
    if (!protocol_supports_cache_r2(local.protocol)) {
        ::close(source_fd);
        return fail_at(stage);
    }

    P51SourceLeaseRequestFields lease_request;
    lease_request.wire_job_id = wire_id;
    lease_request.assignment_epoch = epoch;
    lease_request.assignment_nonce = nonce;
    lease_request.profile = profile_mask;
    lease_request.requested_cache_revision = CACHE_WIRE_REVISION_R2;
    lease_request.requested_window = 30;
    stage = "lease-request-send";
    if (!lease_request.valid() || !local.send_msg(P51SourceLeaseRequestMsg(lease_request))) {
        ::close(source_fd);
        return fail_at(stage);
    }
    stage = "lease-fd-reply";
    P51CacheControlIdentity control_identity;
    const int control_fd = local.receive_p51_cache_fd_reply(
        lease_request, control_identity, deadline);
    if (control_fd < 0 || !control_identity.valid()) {
        if (control_fd >= 0) ::close(control_fd);
        ::close(source_fd);
        return fail_at(stage);
    }
    if (observed_control_identity != nullptr)
        *observed_control_identity = control_identity;

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
    stage = "source-arm-send";
    if (!arm.valid() || !arm_message.valid_for_protocol(compiler_channel.protocol) ||
        !compiler_channel.send_msg(arm_message)) {
        ::close(control_fd);
        ::close(source_fd);
        return fail_at(stage);
    }
    stage = "source-armed-reply";
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
        return fail_at(stage);
    }
    std::fprintf(stderr,
        "P51 fixture: ARMED offer_window=%u selected_revision=%u selected_window=%u\n",
        arm.requested_window, armed_message->selected_revision,
        armed_message->selected_window);
    if (observed_armed != nullptr) {
        *observed_armed = static_cast<const P51SourceArmedFields&>(*armed_message);
        if (observed_armed_ready != nullptr)
            observed_armed_ready->store(true, std::memory_order_release);
    }
    if (observed_arm != nullptr)
        *observed_arm = source;
    if (stop_after_arm) {
        ::close(control_fd);
        ::close(source_fd);
        return failed;
    }

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
    stage = "kind8-begin";
    const DaemonControlStatus started = control.begin_authenticated(
        control_fd, operation, source_fd, credentials, identity, deadline,
        DaemonControlLimits{}, DaemonControlFdOwnership::Owned);
    if (started != DaemonControlStatus::InProgress) {
        std::fprintf(stderr, "P51_KIND8_STATUS job=%u stage=kind8-begin status=%u\n",
                     wire_id, static_cast<unsigned>(started));
        return fail_at(stage);
    }

    stage = "kind8-reply";
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
        !control.source_transfer_result().has_value()) {
        std::fprintf(stderr,
            "P51_KIND8_STATUS job=%u stage=kind8-reply status=%u has_result=%u\n",
            wire_id, static_cast<unsigned>(control.status()),
            control.source_transfer_result().has_value() ? 1u : 0u);
        return fail_at(stage);
    }
    const auto result = *control.source_transfer_result();
    if (result.code != SourceTransferResultCode::Committed) {
        const auto *armed = observed_armed;
        std::fprintf(stderr,
            "P51_KIND8_RESULT job=%u code=%u error=%u attempts=%u "
            "relationship=%02x%02x reservation=%02x%02x\n",
            wire_id, static_cast<unsigned>(result.code), result.error_code,
            result.attempts,
            armed ? armed->logical_relationship_id[0] : 0,
            armed ? armed->logical_relationship_id[1] : 0,
            armed ? armed->reservation_id[0] : 0,
            armed ? armed->reservation_id[1] : 0);
    }
    return result;
}

// Retry only the already-armed source operation on a fresh authenticated C
// control lease. This fixture intentionally does not arm F again: the exact
// ARMED reservation, original deadline, and retained source bytes are reused.
static icecc::p50::local::P50SourceTransferResult retry_p51_kind8_same_arm(
    MsgChannel& local, uint32_t wire_id, uint64_t epoch, uint64_t nonce,
    const P51SourceArmedFields& armed, int source_fd, uint32_t profile_mask,
    Clock::time_point deadline,
    const P51CacheControlIdentity& expected_control_identity)
{
    using namespace icecc::p50;
    using namespace icecc::p50::local;
    P50SourceTransferResult failed{};
    bool source_owned_here = true;
    const auto fail_at = [&](const char *stage) {
        std::fprintf(stderr,
            "P51_CAPACITY_OVERLAP_RETRY_FAIL job=%u stage=%s\n", wire_id, stage);
        if (source_owned_here && source_fd >= 0) ::close(source_fd);
        return failed;
    };
    if (Clock::now() >= deadline) return fail_at("expired-before-lease");

    P51SourceLeaseRequestFields lease_request;
    lease_request.wire_job_id = wire_id;
    lease_request.assignment_epoch = epoch;
    lease_request.assignment_nonce = nonce;
    lease_request.profile = profile_mask;
    lease_request.requested_cache_revision = CACHE_WIRE_REVISION_R2;
    lease_request.requested_window = 30;
    if (!lease_request.valid() ||
        !local.send_msg(P51SourceLeaseRequestMsg(lease_request)))
        return fail_at("lease-request-send");
    P51CacheControlIdentity control_identity;
    const int control_fd = local.receive_p51_cache_fd_reply(
        lease_request, control_identity, deadline);
    if (control_fd < 0 || !control_identity.valid()) {
        if (control_fd >= 0) ::close(control_fd);
        return fail_at("lease-fd-reply");
    }
    if (control_identity != expected_control_identity) {
        ::close(control_fd);
        return fail_at("changed-C-identity");
    }

    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto absolute_deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        deadline, clock.clock_domain_id, clock.time_namespace_id);
    const P51SourceTransferRequest request{armed, absolute_deadline};
    const Identity identity{control_identity.control_generation,
                            control_identity.control_attempt};
    const ControlOperation operation = make_p51_source_transfer_operation(
        identity, request, armed.arm.source.source_request_id);
    CredentialExpectation credentials;
    credentials.uid = control_identity.peer_uid;
    credentials.gid = control_identity.peer_gid;
    DaemonControlOperation control;
    source_owned_here = false; // begin_authenticated owns the descriptor argument
    const DaemonControlStatus started = control.begin_authenticated(
        control_fd, operation, source_fd, credentials, identity, deadline,
        DaemonControlLimits{}, DaemonControlFdOwnership::Owned);
    if (started != DaemonControlStatus::InProgress)
        return fail_at("kind8-begin");

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
        return fail_at("kind8-reply");
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

enum class P51CancelScenario : uint8_t {
    None = 0,
    BeforePublication,
    AfterSourceDeadline,
    RetainedCommitted,
};

static int run_p51_vertical(const char *daemon_binary, const char *cache_service,
                            passwd *icecc, unsigned job_count,
                            uint32_t profile_mask,
                            bool drop_lost_receipts = false,
                            P51CancelScenario cancel_scenario =
                                P51CancelScenario::None)
{
    struct EnvironmentRestore {
        std::optional<std::string> capture_stderr;
        std::optional<std::string> debug_attach;
        std::optional<std::string> source_budget;
        bool active = false;
        ~EnvironmentRestore()
        {
            if (!active) return;
            const auto restore = [](const char *name,
                                    const std::optional<std::string>& previous) {
                if (previous) (void)::setenv(name, previous->c_str(), 1);
                else (void)::unsetenv(name);
            };
            restore("ICECC_TEST_CAPTURE_DAEMON_STDERR", capture_stderr);
            restore("ICECC_P50_DEBUG_ATTACH", debug_attach);
            restore("ICECC_TEST_P50_SOURCE_BUDGET_MSEC", source_budget);
        }
    } environment_restore;
    if (drop_lost_receipts ||
        cancel_scenario != P51CancelScenario::None) {
        if (const char *previous = ::getenv("ICECC_TEST_CAPTURE_DAEMON_STDERR"))
            environment_restore.capture_stderr = previous;
        if (const char *previous = ::getenv("ICECC_P50_DEBUG_ATTACH"))
            environment_restore.debug_attach = previous;
        if (const char *previous =
                ::getenv("ICECC_TEST_P50_SOURCE_BUDGET_MSEC"))
            environment_restore.source_budget = previous;
        environment_restore.active = true;
        if (::setenv("ICECC_TEST_CAPTURE_DAEMON_STDERR", "1", 1) != 0 ||
            ::setenv("ICECC_P50_DEBUG_ATTACH", "1", 1) != 0)
            return 2;
        if (cancel_scenario == P51CancelScenario::AfterSourceDeadline &&
            ::setenv("ICECC_TEST_P50_SOURCE_BUDGET_MSEC", "5000", 1) != 0)
            return 2;
    }
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
    if (drop_lost_receipts) {
        bool captured_logs_ready = true;
        for (const std::string& directory : {cdir, fdir}) {
            const std::string path = directory + "/iceccd.log";
            const int log_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND,
                                      0600);
            const bool ready = log_fd >= 0 &&
                ::fchown(log_fd, icecc->pw_uid, icecc->pw_gid) == 0 &&
                ::fchmod(log_fd, 0600) == 0;
            if (log_fd >= 0) ::close(log_fd);
            captured_logs_ready &= ready;
        }
        REQUIRE(captured_logs_ready,
                "daemon-owned logs capture source cancellation and lifecycle evidence");
        if (!captured_logs_ready) return 2;
    }

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
        P51SourceArmedFields armed{};
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

    if (cancel_scenario != P51CancelScenario::None) {
        const bool after_source_deadline =
            cancel_scenario == P51CancelScenario::AfterSourceDeadline;
        const bool retained_committed =
            cancel_scenario == P51CancelScenario::RetainedCommitted;
        const bool before_publication =
            cancel_scenario == P51CancelScenario::BeforePublication;
        REQUIRE(job_count == 1,
                "focused exact source-cancel cell owns one assignment");
        auto& victim = jobs.front();
        P50SourceArmFields arm{};
        P51SourceArmedFields armed{};
        const uintmax_t cancel_log_offset = std::filesystem::file_size(
            fdir + "/iceccd.log");
        const auto source_result = execute_p51_kind8(
            *victim.wrapper, *victim.compiler, victim.wire_id, epoch,
            victim.nonce, static_cast<uint32_t>(f_port), victim.source_fd,
            profile_mask, &armed, nullptr, nullptr, !retained_committed, &arm);
        const auto advertised_source_deadline = Clock::now() +
            std::chrono::milliseconds(armed.source_budget_msec);
        victim.result = source_result;
        victim.source_fd = -1;
        victim.armed = armed;
        const P51SourceArmMsg arm_message{P51SourceArmFields{arm, 30}};
        REQUIRE(arm_message.valid_payload() && armed.valid() &&
                    armed.acknowledges(arm_message),
                "exact P51 cancellation case retains its original ARM/ARMED identity");
        REQUIRE(retained_committed
                    ? source_result.code ==
                          icecc::p50::local::SourceTransferResultCode::Committed
                    : source_result.code ==
                          icecc::p50::local::SourceTransferResultCode::None,
                retained_committed
                    ? "committed source is retained before its exact cancellation is attempted"
                    : "source transfer remains unstarted before cancellation");
        if (after_source_deadline)
            REQUIRE(armed.source_budget_msec >= 4000 &&
                        armed.source_budget_msec <= 5000,
                    "expired-source case observes the requested bounded 5000ms F ARM budget");

        const pid_t f_sidecar = find_attachment_sidecar(f_pid, cache_service);
        const uint64_t f_sidecar_start_before =
            process_start_time_ticks(f_sidecar);
        const bool pause_f_sidecar = before_publication || after_source_deadline;
        const bool sidecar_stop_sent = pause_f_sidecar && f_sidecar > 1 &&
            ::kill(f_sidecar, SIGSTOP) == 0;
        REQUIRE(!pause_f_sidecar || sidecar_stop_sent,
                "only the test-owned F sidecar is paused at the exact cancellation barrier");
        bool sidecar_stopped = false;
        const auto stopped_deadline = Clock::now() + std::chrono::seconds(2);
        while (pause_f_sidecar && f_sidecar > 1 &&
               Clock::now() < stopped_deadline) {
            std::ifstream status(std::string("/proc/") +
                                std::to_string(f_sidecar) + "/status");
            std::string line;
            while (std::getline(status, line))
                if (line.rfind("State:", 0) == 0 &&
                    line.find('T') != std::string::npos)
                    sidecar_stopped = true;
            if (sidecar_stopped) break;
            ::usleep(10000);
        }
        struct ResumeSidecar {
            pid_t pid;
            bool needed;
            ~ResumeSidecar() {
                if (needed && pid > 1) (void)::kill(pid, SIGCONT);
            }
        } resume_sidecar{f_sidecar, sidecar_stop_sent};
        REQUIRE(!pause_f_sidecar || sidecar_stopped,
                "F sidecar is stopped only for the selected cancellation barrier");
        if (pause_f_sidecar && !sidecar_stopped) {
            if (sidecar_stop_sent) {
                (void)::kill(f_sidecar, SIGCONT);
                resume_sidecar.needed = false;
            }
            delete victim.wrapper;
            victim.wrapper = nullptr;
            delete victim.compiler;
            victim.compiler = nullptr;
            if (victim.source_fd >= 0) ::close(victim.source_fd);
            return 2;
        }

        const uintmax_t attach_log_offset = cancel_log_offset;
        if (!retained_committed) {
            const std::string victim_input =
                "d07-cancel-before-start-" + std::to_string(victim.wire_id) + ".ii";
            const std::string victim_output =
                "d07-cancel-before-start-" + std::to_string(victim.wire_id) + ".o";
            CompileJob compile_job = attachment_compile_job(
                victim.wire_id, epoch, victim.nonce, arm, nullptr);
            compile_job.setInputFile(victim_input);
            compile_job.setOutputFile(victim_output);
            CompileInputIdentity input;
            input.profile = profile_mask == CACHE_PROFILE_P29V1
                ? CompileInputIdentity::P29V1Profile
                : profile_mask == CACHE_PROFILE_ZSTD_ROUTE
                    ? CompileInputIdentity::ZstdRouteProfile
                    : CompileInputIdentity::ZstdTuProfile;
            input.c_store_guid = arm.c_store_guid;
            input.tu_seq = 0;
            input.raw_bytes = victim.bytes.size();
            input.raw_digest = icecc::digest128(victim.bytes).bytes;
            input.attempt_id = victim.nonce;
            input.request_id = arm.source_request_id;
            compile_job.setCompileInputIdentity(input);
            const bool compile_sent = victim.compiler &&
                victim.compiler->send_msg(CompileFileMsg(&compile_job));
            REQUIRE(compile_sent,
                    "exact R2 CompileFile is submitted while its input is unpublished");
            const std::string attach_begin = "P50_INPUT_ATTACH_BEGIN job=" +
                std::to_string(victim.wire_id) + " epoch=" +
                std::to_string(epoch) + " nonce=" +
                std::to_string(victim.nonce) + " request=" +
                std::to_string(arm.source_request_id);
            const bool attach_waiting = compile_sent &&
                wait_attachment_log(fdir + "/iceccd.log", attach_log_offset,
                                    attach_begin, 3000);
            REQUIRE(attach_waiting,
                    "F entered WAITP50INPUT for the exact armed assignment");
        }

        if (after_source_deadline) {
            const bool daemon_stop_sent = f_pid > 1 &&
                ::kill(f_pid, SIGSTOP) == 0;
            bool daemon_stopped = false;
            const auto daemon_stopped_deadline =
                Clock::now() + std::chrono::seconds(2);
            while (daemon_stop_sent && Clock::now() < daemon_stopped_deadline) {
                std::ifstream status(std::string("/proc/") +
                                    std::to_string(f_pid) + "/status");
                std::string line;
                while (std::getline(status, line))
                    if (line.rfind("State:", 0) == 0 &&
                        line.find('T') != std::string::npos)
                        daemon_stopped = true;
                if (daemon_stopped) break;
                ::usleep(10000);
            }
            struct ResumeDaemon {
                pid_t pid;
                bool needed;
                ~ResumeDaemon() {
                    if (needed && pid > 1) (void)::kill(pid, SIGCONT);
                }
            } resume_daemon{f_pid, daemon_stop_sent};
            REQUIRE(daemon_stopped,
                    "F daemon is held after WAITP50INPUT before its source deadline");
            if (daemon_stopped) {
                std::this_thread::sleep_until(
                    advertised_source_deadline + std::chrono::milliseconds(100));
                REQUIRE(Clock::now() > advertised_source_deadline,
                        "original F source deadline elapses while its owner is held");
            }
            // Closing these channels while F is stopped prevents its ordinary
            // deadline watcher from queueing the cancellation before expiry.
            delete victim.compiler;
            victim.compiler = nullptr;
            delete victim.wrapper;
            victim.wrapper = nullptr;
            if (resume_daemon.needed) {
                (void)::kill(f_pid, SIGCONT);
                resume_daemon.needed = false;
            }
        }
        if (before_publication) {
            const auto children_before_cancel = direct_children(f_pid);
            const bool sidecar_is_only_child = children_before_cancel.size() == 1 &&
                children_before_cancel.front() == f_sidecar;
            REQUIRE(sidecar_is_only_child,
                    "no compiler child exists before the prepublication cancel");
        }
        if (!after_source_deadline) {
            delete victim.wrapper;
            victim.wrapper = nullptr;
            delete victim.compiler;
            victim.compiler = nullptr;
        }
        if (sidecar_stop_sent) {
            (void)::kill(f_sidecar, SIGCONT);
            resume_sidecar.needed = false;
        }

        const auto hex_id = [](const std::array<uint8_t, 16>& bytes) {
            static constexpr char digits[] = "0123456789abcdef";
            std::string value;
            value.reserve(bytes.size() * 2);
            for (const uint8_t byte : bytes) {
                value.push_back(digits[byte >> 4]);
                value.push_back(digits[byte & 0x0f]);
            }
            return value;
        };
        const std::string cancel_result_prefix =
            "P51_SOURCE_CANCEL_RESULT job=" + std::to_string(victim.wire_id) +
            " epoch=" + std::to_string(epoch) +
            " nonce=" + std::to_string(victim.nonce) +
            " request=" + std::to_string(arm.source_request_id) +
            " reservation=" + hex_id(armed.reservation_id) + " cancelled=";
        if (after_source_deadline) {
            const std::string queued_after_expiry =
                "P51_SOURCE_CANCEL_QUEUED job=" +
                std::to_string(victim.wire_id) + " epoch=" +
                std::to_string(epoch) + " nonce=" +
                std::to_string(victim.nonce) + " request=" +
                std::to_string(arm.source_request_id) + " reservation=" +
                hex_id(armed.reservation_id) + " source_expired=1";
            const bool exact_queue_witness = wait_attachment_log(
                fdir + "/iceccd.log", cancel_log_offset,
                queued_after_expiry, 5000);
            REQUIRE(exact_queue_witness,
                    "daemon queued exact cancellation after original source deadline");
        }
        const bool exact_cancel_result_seen = wait_attachment_log(
            fdir + "/iceccd.log", attach_log_offset,
            cancel_result_prefix, 5000);
        const std::string cancel_log = read_file_suffix(
            fdir + "/iceccd.log", attach_log_offset);
        const bool cancel_accepted = cancel_log.find(
            cancel_result_prefix + "1") != std::string::npos;
        const bool cancel_rejected = cancel_log.find(
            cancel_result_prefix + "0") != std::string::npos;
        REQUIRE(exact_cancel_result_seen &&
                    (after_source_deadline
                         ? (cancel_accepted || cancel_rejected)
                         : retained_committed ? cancel_rejected
                         : cancel_accepted),
                "daemon completed one bounded exact cancellation control exchange with the expected disposition");
        const std::string settled_unknown = "P50 input settlement job " +
            std::to_string(victim.wire_id) + " action 1 status unknown-record";
        const bool attempt_cancelled_before_publication = before_publication &&
            wait_attachment_log(fdir + "/iceccd.log", attach_log_offset,
                                settled_unknown, 5000);
        if (before_publication)
            REQUIRE(attempt_cancelled_before_publication,
                    "exact prepublication InputLifecycle CancelAttempt finds no committed input");
        if (retained_committed)
            REQUIRE(cancel_rejected,
                    "daemon rejects cancellation after the exact committed transfer");

        bool no_victim_child = true;
        if (!retained_committed) {
            const auto no_start_deadline = Clock::now() + std::chrono::milliseconds(500);
            no_victim_child = false;
            while (Clock::now() < no_start_deadline) {
                const auto children = direct_children(f_pid);
                no_victim_child = children.size() == 1 &&
                                  children.front() == f_sidecar;
                if (!no_victim_child) break;
                ::usleep(10000);
            }
        }
        const std::string f_suffix = read_file_suffix(
            fdir + "/iceccd.log", attach_log_offset);
        const std::string accepted_attach = "P50_INPUT_ATTACH_END job=" +
            std::to_string(victim.wire_id) + " epoch=" +
            std::to_string(epoch) + " nonce=" +
            std::to_string(victim.nonce) + " request=" +
            std::to_string(arm.source_request_id);
        bool no_successful_attach = true;
        for (size_t at = 0; (at = f_suffix.find(accepted_attach, at)) !=
                             std::string::npos;) {
            const size_t end = f_suffix.find('\n', at);
            const std::string_view line(f_suffix.data() + at,
                (end == std::string::npos ? f_suffix.size() : end) - at);
            if (line.find(" status=0") != std::string_view::npos) {
                no_successful_attach = false;
                break;
            }
            ++at;
        }
        if (!retained_committed) {
            REQUIRE(no_victim_child &&
                        f_suffix.find("final arguments:") == std::string::npos,
                    "prepublication cancellation starts no compiler process");
            REQUIRE(no_successful_attach,
                    "prepublication cancellation did not publish an accepted attachment");
        }
        const uint64_t c_start_time = process_start_time_ticks(c_pid);
        const uint64_t f_start_time = process_start_time_ticks(f_pid);
        const uint64_t f_sidecar_start_time =
            process_start_time_ticks(f_sidecar);
        REQUIRE(c_start_time != 0 && f_start_time != 0 &&
                    f_sidecar_start_before != 0 &&
                    f_sidecar_start_time == f_sidecar_start_before,
                "daemon and sidecar process start-time witnesses are retained");
        for (pid_t pid : {c_pid, f_pid}) (void)::kill(pid, SIGTERM);
        int c_status = 0, f_status = 0;
        const bool c_reaped = wait_child(c_pid, 10000, &c_status);
        const bool f_reaped = wait_child(f_pid, 10000, &f_status);
        if (c_reaped) daemon_cleanup.c_pid = -1;
        if (f_reaped) daemon_cleanup.f_pid = -1;
        REQUIRE(c_reaped && WIFEXITED(c_status) && WEXITSTATUS(c_status) == 0,
                "C daemon exits cleanly after the exact cancellation fixture");
        REQUIRE(f_reaped && WIFEXITED(f_status) && WEXITSTATUS(f_status) == 0,
                "F daemon exits cleanly after the exact cancellation fixture");
        delete c_scheduler;
        delete f_scheduler;
        ::close(c_scheduler_listener);
        ::close(f_scheduler_listener);
        if (failures == 0) {
            if (retained_committed) {
                std::fprintf(stderr,
                    "P51_D07_RETAINED_CANCEL_REJECT_PASS profile=%u job=%u epoch=%llu nonce=%llu request=%llu reservation=%s cancel_accepted=0 source_result=committed byte_preservation=covered_by_service_test\n",
                    profile_mask, victim.wire_id,
                    static_cast<unsigned long long>(epoch),
                    static_cast<unsigned long long>(victim.nonce),
                    static_cast<unsigned long long>(arm.source_request_id),
                    hex_id(armed.reservation_id).c_str());
            } else {
                std::fprintf(stderr,
                    "P51_D07_PREPUBLICATION_CANCEL_PASS profile=%u job=%u epoch=%llu nonce=%llu request=%llu reservation=%s c_pid=%d c_start=%llu f_pid=%d f_start=%llu f_sidecar_pid=%d f_sidecar_start=%llu no_source_transfer=1 no_compiler_start=1\n",
                    profile_mask, victim.wire_id,
                    static_cast<unsigned long long>(epoch),
                    static_cast<unsigned long long>(victim.nonce),
                    static_cast<unsigned long long>(arm.source_request_id),
                    hex_id(armed.reservation_id).c_str(), c_pid,
                    static_cast<unsigned long long>(c_start_time), f_pid,
                    static_cast<unsigned long long>(f_start_time), f_sidecar,
                    static_cast<unsigned long long>(f_sidecar_start_time));
            }
            std::filesystem::remove_all(work);
            return 0;
        }
        std::fprintf(stderr,
            "retained exact-cancellation work directory: %s\n", work.c_str());
        return 1;
    }

    const pid_t f_sidecar_before = drop_lost_receipts
        ? find_attachment_sidecar(f_pid, cache_service) : -1;
    REQUIRE(!drop_lost_receipts || f_sidecar_before > 1,
            "D04 records the live F sidecar incarnation before dropping receipts");
    uintmax_t f_protocol_log_offset = 0;
    if (drop_lost_receipts) {
        std::error_code log_error;
        f_protocol_log_offset = std::filesystem::file_size(
            fdir + "/iceccd.log", log_error);
        REQUIRE(!log_error,
                "D04 captures the F log boundary before the first R2 link");
        if (log_error) return 2;
    }
    std::unique_ptr<P51CommitReceiptGate> receipt_gate;
    if (job_count > 1 || drop_lost_receipts) {
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
                profile_mask, drop_lost_receipts ? &current.armed : nullptr);
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
    const std::vector<icecc::p50::R2TxCommit> held_commit_witnesses =
        receipt_gate ? receipt_gate->commit_witnesses()
                     : std::vector<icecc::p50::R2TxCommit>{};
    bool no_transfer_completed_before_release = true;
    if (receipt_gate) {
        for (const auto& job : jobs)
            no_transfer_completed_before_release &=
                !job.finished.load(std::memory_order_acquire);
    }
    const size_t held_commit_count = receipt_gate
        ? receipt_gate->observed_commits() : 0;
    if (receipt_gate && drop_lost_receipts) {
        // F has fully published these commits before emitting their frames.
        // Drop the entire observed receipt window and close only this physical
        // link; both daemons and the F sidecar remain alive for recovery.
        receipt_gate->discard_held_commits();
        receipt_gate.reset();
    } else if (receipt_gate) {
        receipt_gate->release_commits();
    }
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
    bool recovered_held_commits_exactly = !drop_lost_receipts ||
        held_commit_witnesses.size() == job_count;
    if (drop_lost_receipts && held_commit_witnesses.size() == job_count) {
        std::set<uint64_t> matched_tu_sequences;
        std::set<uint64_t> held_ordinals;
        std::set<uint64_t> held_relative_sequences;
        const auto& anchor = jobs.front().armed;
        for (const auto& witness : held_commit_witnesses) {
            const uint64_t tu_seq = witness.inner.tu_seq.value;
            const auto item = std::find_if(jobs.begin(), jobs.end(),
                [&](const VerticalJob& job) {
                    return job.result.tu_seq == tu_seq &&
                           job.result.code ==
                               icecc::p50::local::SourceTransferResultCode::Committed;
                });
            const bool exact_result = item != jobs.end() &&
                matched_tu_sequences.insert(tu_seq).second &&
                item->result.raw_bytes == item->bytes.size() &&
                item->result.raw_digest == witness.inner.raw_digest &&
                item->result.raw_digest == icecc::digest128(item->bytes) &&
                item->result.c_store_guid.bytes ==
                    item->armed.arm.source.c_store_guid &&
                item->armed.valid() &&
                item->armed.arm.source.cache_profile == profile_mask &&
                item->armed.selected_revision == CACHE_WIRE_REVISION_R2 &&
                item->armed.relationship_epoch != 0 &&
                item->armed.logical_relationship_id ==
                    anchor.logical_relationship_id &&
                item->armed.relationship_epoch == anchor.relationship_epoch &&
                item->armed.f_store_guid == anchor.f_store_guid &&
                item->armed.f_store_generation == anchor.f_store_generation &&
                item->armed.selected_window == anchor.selected_window &&
                witness.relationship_ordinal != 0 &&
                held_ordinals.insert(witness.relationship_ordinal).second &&
                held_relative_sequences.insert(witness.inner.rel_seq.value).second &&
                witness.inner.history_nonce.value != 0 &&
                witness.inner.transaction_digest != icecc::p50::Digest128{} &&
                witness.binding_digest != icecc::p50::Digest128{} &&
                witness.transaction_digest != icecc::p50::Digest128{};
            recovered_held_commits_exactly &= exact_result;
        }
        const auto contiguous = [&](const std::set<uint64_t>& values) {
            return !values.empty() && values.size() == job_count &&
                *values.rbegin() - *values.begin() + 1 == values.size();
        };
        recovered_held_commits_exactly &=
            contiguous(held_ordinals) && contiguous(held_relative_sequences);
    }
    bool one_publication_and_attachment_per_job = true;
    size_t recovered_physical_links = 0;
    bool same_f_sidecar_incarnation = false;
    REQUIRE(held_all_commits,
            "F emitted the configured exact COMMIT window before receipt release/drop");
    REQUIRE(no_transfer_completed_before_release,
            "all source-transfer callers remain pending while COMMIT receipts are withheld");
    REQUIRE(all_transfers_committed && contiguous_tu_sequences && elapsed < 30000,
            "real C kind-8 service completes the exact distinct input window and TU identities");
    REQUIRE(!drop_lost_receipts || recovered_held_commits_exactly,
            "each recovered committed result matches one unique held COMMIT TU/raw-digest witness");
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
    if (drop_lost_receipts) {
        const std::string f_log = read_file_suffix(
            fdir + "/iceccd.log", f_protocol_log_offset);
        const auto occurrences = [&](const std::string& needle) {
            size_t count = 0;
            for (size_t at = 0; (at = f_log.find(needle, at)) != std::string::npos;
                 at += needle.size())
                ++count;
            return count;
        };
        recovered_physical_links = occurrences("P51_CACHE_LINK_READY request=");
        for (const auto& job : jobs) {
            const std::string published = "P50 sidecar lifecycle commit tu=" +
                std::to_string(job.result.tu_seq) + " retained=1 observed=1\n";
            const std::string attached = "P50_INPUT_ATTACH_END job=" +
                std::to_string(job.wire_id) + " epoch=" + std::to_string(epoch) +
                " nonce=" + std::to_string(job.nonce) + " request=" +
                std::to_string(job.nonce) + " elapsed_ms=";
            const size_t attachment_at = f_log.find(attached);
            const size_t attachment_end = attachment_at == std::string::npos
                ? std::string::npos : f_log.find('\n', attachment_at);
            const bool accepted_once = attachment_at != std::string::npos &&
                attachment_end != std::string::npos &&
                f_log.substr(attachment_at, attachment_end - attachment_at)
                    .find(" status=0") != std::string::npos &&
                occurrences(attached) == 1;
            one_publication_and_attachment_per_job &=
                occurrences(published) == 1 && accepted_once;
        }
        const pid_t f_sidecar_after = find_attachment_sidecar(f_pid, cache_service);
        same_f_sidecar_incarnation = f_sidecar_before > 1 &&
            f_sidecar_after == f_sidecar_before &&
            ::kill(f_sidecar_before, 0) == 0;
        std::fprintf(stderr,
            "P51_D04_OBSERVED count=%u held=%zu witnesses=%zu publication_attachment_ok=%u physical_links=%zu f_pid=%ld/%ld\n",
            job_count, held_commit_count, held_commit_witnesses.size(),
            one_publication_and_attachment_per_job ? 1u : 0u,
            recovered_physical_links, static_cast<long>(f_sidecar_before),
            static_cast<long>(f_sidecar_after));
    }
    REQUIRE(!drop_lost_receipts || one_publication_and_attachment_per_job,
            "F published and accepted exactly one matching attachment for each recovered input");
    REQUIRE(!drop_lost_receipts ||
                (recovered_physical_links >= 2 && same_f_sidecar_incarnation),
            "lost receipts use a new physical R2 connection to the same live F sidecar incarnation");
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
        ? receipt_gate->observed_commits() : held_commit_count;
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
    if (drop_lost_receipts && failures == 0)
        std::fprintf(stderr,
            "P51_D04_LOST_RECEIPTS_PASS count=%u profile=%u held=%zu recovered=%zu physical_links=%zu same_f_sidecar=1 suffix_uncommitted=not-covered\n",
            job_count, profile_mask, held_commit_count, held_commit_witnesses.size(),
            recovered_physical_links);
    return failures ? 1 : 0;
}

static int run_p51_multilink_topology(const char *daemon_binary,
                                      const char *cache_service,
                                      passwd *icecc, unsigned c_count,
                                      unsigned f_count, uint32_t profile_mask)
{
    constexpr unsigned jobs_per_link = 30;
    const size_t pair_count = static_cast<size_t>(c_count) * f_count;
    const size_t total_jobs = pair_count * jobs_per_link;
    const std::string topology = "C" + std::to_string(c_count) + "F" +
        std::to_string(f_count);
    const char *capacity_overlap_env =
        ::getenv("ICECC_TEST_P51_MULTILINK_CAPACITY_OVERLAP");
    const bool capacity_overlap = capacity_overlap_env != nullptr &&
        std::strcmp(capacity_overlap_env, "1") == 0;
    REQUIRE(c_count >= 1 && c_count <= 4 && f_count >= 1 && f_count <= 4 &&
                pair_count > 1 && total_jobs <= 120,
            "multi-link topology is within the 4-role/120-job bounds");
    if (c_count == 0 || c_count > 4 || f_count == 0 || f_count > 4 ||
        pair_count <= 1 || total_jobs > 120)
        return 2;
    REQUIRE(!capacity_overlap || (c_count == 1 && f_count == 4),
            "capacity-overlap gate uses exactly one C and four independent F roles");
    if (capacity_overlap && (c_count != 1 || f_count != 4))
        return 2;
    ::signal(SIGPIPE, SIG_IGN);
    const char *temporary_root = ::getenv("TMPDIR");
    const std::string prefix = temporary_root && *temporary_root
        ? temporary_root : "/tmp";
    std::string pattern = prefix + "/p51multi.XXXXXX";
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    char *created = ::mkdtemp(mutable_pattern.data());
    REQUIRE(created != nullptr, "multi-link test temporary root created");
    if (created == nullptr) return 2;
    const std::string work(created);
    const std::string settlement_gate_dir = work + "/settlement-gate";

    struct Role {
        std::string name;
        std::string directory;
        std::string log;
        std::string envdir;
        std::string runtime;
        int scheduler_listener = -1;
        int scheduler_port = 0;
        int endpoint_port = 0;
        pid_t pid = -1;
        std::unique_ptr<MsgChannel> scheduler;
    };
    std::vector<Role> c_roles(c_count), f_roles(f_count);
    struct RoleCleanup {
        std::vector<Role> *c_roles;
        std::vector<Role> *f_roles;
        ~RoleCleanup()
        {
            auto clean = [](std::vector<Role> *roles) {
                if (roles == nullptr) return;
                for (Role& role : *roles) {
                    role.scheduler.reset();
                    if (role.scheduler_listener >= 0) {
                        ::close(role.scheduler_listener);
                        role.scheduler_listener = -1;
                    }
                    if (role.pid > 0) {
                        (void)::kill(role.pid, SIGTERM);
                        int status = 0;
                        if (!wait_child(role.pid, 3000, &status)) {
                            (void)::kill(role.pid, SIGKILL);
                            (void)::waitpid(role.pid, &status, 0);
                        }
                        role.pid = -1;
                    }
                }
            };
            clean(c_roles);
            clean(f_roles);
        }
    } role_cleanup{&c_roles, &f_roles};

    REQUIRE(::chown(work.c_str(), icecc->pw_uid, icecc->pw_gid) == 0 &&
                ::chmod(work.c_str(), 0700) == 0,
            "multi-link work root has exact icecc ownership");
    if (capacity_overlap) {
        const bool gate_ready = ::mkdir(settlement_gate_dir.c_str(), 0700) == 0 &&
            ::chown(settlement_gate_dir.c_str(), icecc->pw_uid, icecc->pw_gid) == 0 &&
            ::setenv("ICECC_TEST_P51_SETTLEMENT_GATE_DIR",
                     settlement_gate_dir.c_str(), 1) == 0 &&
            ::setenv("ICECC_TEST_P51_SETTLEMENT_GATE_COUNT", "30", 1) == 0 &&
            ::setenv("ICECC_TEST_P51_ARM_TRACE", "1", 1) == 0;
        REQUIRE(gate_ready,
                "bounded test-only 30-Goodbye settlement gate is configured for C");
        if (!gate_ready) return 2;
    }
    auto prepare_role = [&](Role& role, const std::string& name) {
        role.name = name;
        role.directory = work + "/" + name;
        role.log = role.directory + "/iceccd.log";
        role.envdir = role.directory + "/envs";
        role.runtime = role.directory + "/runtime";
        const bool made = ::mkdir(role.directory.c_str(), 0700) == 0 &&
            ::chown(role.directory.c_str(), icecc->pw_uid, icecc->pw_gid) == 0 &&
            ::mkdir(role.envdir.c_str(), 0700) == 0 &&
            ::chown(role.envdir.c_str(), icecc->pw_uid, icecc->pw_gid) == 0 &&
            ::mkdir(role.runtime.c_str(), 0700) == 0 &&
            ::chown(role.runtime.c_str(), icecc->pw_uid, icecc->pw_gid) == 0;
        if (!made || ::getenv("ICECC_TEST_CAPTURE_DAEMON_STDERR") == nullptr)
            return made;
        const int log_fd = ::open(role.log.c_str(), O_CREAT | O_WRONLY | O_APPEND,
                                  0644);
        if (log_fd < 0) return false;
        const bool log_ready = ::fchown(log_fd, icecc->pw_uid, icecc->pw_gid) == 0;
        ::close(log_fd);
        return log_ready;
    };
    bool directories_ready = true;
    for (unsigned i = 0; i < c_count; ++i)
        directories_ready &= prepare_role(c_roles[i], "c" + std::to_string(i));
    for (unsigned i = 0; i < f_count; ++i)
        directories_ready &= prepare_role(f_roles[i], "f" + std::to_string(i));
    REQUIRE(directories_ready, "all bounded C/F role directories are private");
    if (!directories_ready) return 2;

    auto prepare_ports = [&](Role& role) {
        role.scheduler_listener = listen_ephemeral(&role.scheduler_port);
        role.endpoint_port = reserve_port();
        return role.scheduler_listener >= 0 && role.scheduler_port > 0 &&
            role.endpoint_port > 0;
    };
    bool ports_ready = true;
    for (Role& role : c_roles) ports_ready &= prepare_ports(role);
    for (Role& role : f_roles) ports_ready &= prepare_ports(role);
    REQUIRE(ports_ready, "every C/F role has independent ordinary and cache ports");
    if (!ports_ready) return 2;

    const unsigned max_jobs = static_cast<unsigned>(total_jobs + 2);
    auto start_role = [&](Role& role) {
        role.pid = launch_vertical_daemon(
            daemon_binary, cache_service, role.directory + "/iceccd.sock",
            role.envdir, role.runtime, role.log, role.scheduler_port,
            role.endpoint_port, role.name.c_str(), max_jobs);
        return role.pid > 0;
    };
    bool started = true;
    for (Role& role : c_roles) started &= start_role(role);
    for (Role& role : f_roles) started &= start_role(role);
    REQUIRE(started, "all bounded real C/F daemons started");
    if (!started) return 2;

    bool logged_in = true;
    for (Role& role : c_roles) {
        role.scheduler.reset(accept_channel(role.scheduler_listener, 10000));
        std::unique_ptr<Msg> initial(role.scheduler
            ? wait_for_type(role.scheduler.get(), Msg::LOGIN, 5000) : nullptr);
        const auto *login = dynamic_cast<const LoginMsg *>(initial.get());
        logged_in &= absent(login);
        if (!absent(login))
            std::fprintf(stderr,
                "P51_MULTILINK_STARTUP role=%s pid=%ld login=%u cache=%u/%u/%u log=%s\n",
                role.name.c_str(), static_cast<long>(role.pid), login ? 1u : 0u,
                login ? login->cache_endpoint_port : 0,
                login ? login->cache_protocol : 0,
                login ? login->cache_profile_mask : 0, role.log.c_str());
    }
    for (Role& role : f_roles) {
        role.scheduler.reset(accept_channel(role.scheduler_listener, 10000));
        std::unique_ptr<Msg> initial(role.scheduler
            ? wait_for_type(role.scheduler.get(), Msg::LOGIN, 5000) : nullptr);
        const auto *login = dynamic_cast<const LoginMsg *>(initial.get());
        logged_in &= absent(login);
        if (!absent(login))
            std::fprintf(stderr,
                "P51_MULTILINK_STARTUP role=%s pid=%ld login=%u cache=%u/%u/%u log=%s\n",
                role.name.c_str(), static_cast<long>(role.pid), login ? 1u : 0u,
                login ? login->cache_endpoint_port : 0,
                login ? login->cache_protocol : 0,
                login ? login->cache_profile_mask : 0, role.log.c_str());
    }
    REQUIRE(logged_in, "all multi-link roles start cache-absent before activation");
    if (!logged_in) {
        for (const Role& role : c_roles)
            std::fprintf(stderr, "P51_MULTILINK_LOG role=%s path=%s contents=%s\n",
                role.name.c_str(), role.log.c_str(),
                read_file_suffix(role.log, 0).c_str());
        for (const Role& role : f_roles)
            std::fprintf(stderr, "P51_MULTILINK_LOG role=%s path=%s contents=%s\n",
                role.name.c_str(), role.log.c_str(),
                read_file_suffix(role.log, 0).c_str());
        std::fprintf(stderr, "retained multi-link startup directory: %s\n", work.c_str());
        return 1;
    }

    const uint64_t epoch = UINT64_C(0x51d0000000000001);
    const ConfCSMsg activate(epoch, ConfCSMsg::StrictNonce);
    bool activated = true;
    for (Role& role : c_roles)
        activated &= role.scheduler && role.scheduler->send_msg(activate);
    for (Role& role : f_roles)
        activated &= role.scheduler && role.scheduler->send_msg(activate);
    bool ready = activated;
    auto await_ready = [&](Role& role) {
        std::unique_ptr<Msg> login(role.scheduler
            ? wait_for_type(role.scheduler.get(), Msg::LOGIN, 10000) : nullptr);
        return present_revision(dynamic_cast<LoginMsg *>(login.get()),
            static_cast<uint32_t>(role.endpoint_port), CACHE_WIRE_REVISION_R2);
    };
    for (Role& role : c_roles) ready &= await_ready(role);
    for (Role& role : f_roles) ready &= await_ready(role);
    REQUIRE(ready, "all real C/F role sidecars publish protocol-51 READY");
    if (!ready) return 1;

    struct Job {
        size_t c_index = 0;
        size_t f_index = 0;
        size_t pair_index = 0;
        uint32_t wire_id = 0;
        uint64_t nonce = 0;
        std::unique_ptr<MsgChannel> wrapper;
        std::unique_ptr<MsgChannel> compiler;
        std::string bytes;
        int source_fd = -1;
        P51SourceArmedFields armed{};
        std::atomic<bool> armed_ready{false};
        icecc::p50::local::P50SourceTransferResult result{};
        std::atomic<bool> finished{false};
        uintmax_t attach_log_offset = 0;
        bool compile_sent = false;
        bool attached = false;
        bool compile_bounded = false;
    };
    std::vector<Job> jobs(total_jobs);
    struct JobCleanup {
        std::vector<Job> *jobs;
        ~JobCleanup()
        {
            if (jobs == nullptr) return;
            for (Job& job : *jobs) {
                if (job.source_fd >= 0) {
                    ::close(job.source_fd);
                    job.source_fd = -1;
                }
                job.wrapper.reset();
                job.compiler.reset();
            }
        }
    } job_cleanup{&jobs};

    bool assignments_ready = true;
    size_t job_index = 0;
    for (size_t c = 0; c < c_roles.size(); ++c) {
        for (size_t f = 0; f < f_roles.size(); ++f) {
            const size_t pair_index = c * f_roles.size() + f;
            for (unsigned ordinal = 0; ordinal < jobs_per_link; ++ordinal, ++job_index) {
                Job& job = jobs[job_index];
                job.c_index = c;
                job.f_index = f;
                job.pair_index = pair_index;
                job.wire_id = static_cast<uint32_t>(0x51d001 + job_index);
                job.nonce = UINT64_C(0x51d00100000001) + job_index;
                const bool c_prepared = c_roles[c].scheduler->send_msg(
                    AssignPrepareMsg(epoch, job.wire_id, job.nonce, 1));
                const bool f_prepared = f_roles[f].scheduler->send_msg(
                    AssignPrepareMsg(epoch, job.wire_id, job.nonce, 1));
                std::unique_ptr<Msg> c_ready_msg(c_prepared
                    ? wait_for_type(c_roles[c].scheduler.get(), Msg::ASSIGN_READY, 5000)
                    : nullptr);
                std::unique_ptr<Msg> f_ready_msg(f_prepared
                    ? wait_for_type(f_roles[f].scheduler.get(), Msg::ASSIGN_READY, 5000)
                    : nullptr);
                const auto *c_ready = dynamic_cast<const AssignReadyMsg *>(c_ready_msg.get());
                const auto *f_ready = dynamic_cast<const AssignReadyMsg *>(f_ready_msg.get());
                const bool assigned = c_ready && f_ready &&
                    c_ready->wire_id == job.wire_id && f_ready->wire_id == job.wire_id &&
                    c_ready->epoch() == epoch && f_ready->epoch() == epoch &&
                    c_ready->nonce() == job.nonce && f_ready->nonce() == job.nonce;
                job.compiler.reset(assigned
                    ? connect_tcp_bounded(f_roles[f].endpoint_port, 5000) : nullptr);
                job.wrapper.reset(Service::createChannel(
                    c_roles[c].directory + "/iceccd.sock"));
                Environments source_envs;
                source_envs.emplace_back("x86_64", "multilink-env");
                GetCSMsg source_get(source_envs, "multilink.cpp", CompileJob::Lang_CXX,
                    1, "x86_64", 0, "", PROTOCOL_VERSION, 0, 0);
                source_get.cache_protocol = CACHE_WIRE_REVISION_R2;
                source_get.cache_profile_mask = profile_mask;
                const bool get_sent = job.wrapper && job.wrapper->send_msg(source_get);
                std::unique_ptr<Msg> forwarded(get_sent
                    ? wait_for_type(c_roles[c].scheduler.get(), Msg::GET_CS, 5000)
                    : nullptr);
                const auto *get = dynamic_cast<const GetCSMsg *>(forwarded.get());
                const uint32_t client_id = get ? get->client_id : 0;
                const bool use_sent = get && c_roles[c].scheduler->send_msg(UseCSMsg(
                    "x86_64", "127.0.0.1", f_roles[f].endpoint_port,
                    job.wire_id, true, client_id, 0, epoch, job.nonce,
                    f_roles[f].endpoint_port, CACHE_WIRE_REVISION_R2, profile_mask));
                std::unique_ptr<Msg> use_reply(use_sent
                    ? job.wrapper->get_msg_until(Clock::now() + std::chrono::seconds(5))
                    : nullptr);
                const auto *use = dynamic_cast<const UseCSMsg *>(use_reply.get());
                const bool source_ready = use && use->job_id == job.wire_id &&
                    use->assignmentEpoch() == epoch && use->assignmentNonce() == job.nonce &&
                    use->cache_endpoint_port == static_cast<uint32_t>(f_roles[f].endpoint_port) &&
                    use->cache_protocol == CACHE_WIRE_REVISION_R2 &&
                    use->cache_profile_mask == profile_mask;
                job.bytes = "P51 multi-link exact input " + std::to_string(c) + "/" +
                    std::to_string(f) + "/" + std::to_string(ordinal) + "\n";
                job.source_fd = job.compiler
                    ? make_vertical_source_fd(work, job.bytes) : -1;
                assignments_ready &= assigned && job.compiler && job.wrapper &&
                    source_ready && job.source_fd >= 0;
            }
        }
    }
    REQUIRE(assignments_ready,
            "all C/F Cartesian assignments have exact original compiler channels and inputs");
    if (!assignments_ready) return 1;

    std::vector<std::unique_ptr<P51MultiLinkCommitGate>> gates;
    gates.reserve(f_count);
    bool gates_ready = true;
    for (size_t f = 0; f < f_roles.size(); ++f) {
        gates.emplace_back(std::make_unique<P51MultiLinkCommitGate>(
            f_roles[f].endpoint_port, icecc->pw_uid, c_count, jobs_per_link,
            capacity_overlap || f_count == 1 || f == 0,
            capacity_overlap));
        gates_ready &= gates.back()->ready();
    }
    REQUIRE(gates_ready,
            "every F endpoint has a bounded multi-connection exact-COMMIT receipt gate");
    if (!gates_ready) return 2;

    std::vector<std::thread> transfers;
    transfers.reserve(jobs.size());
    for (Job& job : jobs) {
        transfers.emplace_back([&job, &f_roles, profile_mask, epoch] {
            const int source_fd = std::exchange(job.source_fd, -1);
            job.result = execute_p51_kind8(*job.wrapper, *job.compiler,
                job.wire_id, epoch, job.nonce,
                static_cast<uint32_t>(f_roles[job.f_index].endpoint_port),
                source_fd, profile_mask, &job.armed, &job.armed_ready);
            job.finished.store(true, std::memory_order_release);
        });
    }

    bool gate_complete = true;
    const auto gate_deadline = Clock::now() + std::chrono::seconds(45);
    for (size_t f = 0; f < gates.size(); ++f) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            gate_deadline - Clock::now());
        if (remaining <= std::chrono::milliseconds::zero() ||
            !gates[f]->wait_for_all_links(remaining)) {
            gate_complete = false;
            gates[f]->dump_state(f);
            break;
        }
    }
    REQUIRE(gate_complete,
            "each link receives 30 unique TUs within a C-wide contiguous allocation");
    if (!gate_complete) {
        size_t armed_count = 0;
        size_t finished_count = 0;
        for (const Job& job : jobs) {
            armed_count += job.armed_ready.load(std::memory_order_acquire) ? 1 : 0;
            finished_count += job.finished.load(std::memory_order_acquire) ? 1 : 0;
        }
        std::fprintf(stderr,
                     "P51 multilink failure topology=%s profile=%u armed=%zu/%zu finished=%zu/%zu\n",
                     topology.c_str(), profile_mask, armed_count, jobs.size(),
                     finished_count, jobs.size());
        for (const Job& job : jobs) {
            if (job.finished.load(std::memory_order_acquire)) {
                std::fprintf(stderr,
                    "P51 multilink job f=%zu c=%zu job=%u armed=%u "
                    "relation=%02x%02x reservation=%02x%02x cguid=%02x%02x "
                    "cgen=%llu fguid=%02x%02x result=%u error=%u raw=%llu tu=%llu\n",
                    job.f_index, job.c_index, job.wire_id,
                    job.armed_ready.load(std::memory_order_acquire) ? 1u : 0u,
                    job.armed.logical_relationship_id[0],
                    job.armed.logical_relationship_id[1],
                    job.armed.reservation_id[0], job.armed.reservation_id[1],
                    job.armed.arm.source.c_store_guid[0],
                    job.armed.arm.source.c_store_guid[1],
                    static_cast<unsigned long long>(
                        job.armed.arm.source.c_store_generation),
                    job.armed.f_store_guid[0], job.armed.f_store_guid[1],
                    static_cast<unsigned>(job.result.code), job.result.error_code,
                    static_cast<unsigned long long>(job.result.raw_bytes),
                    static_cast<unsigned long long>(job.result.tu_seq));
            }
        }
        for (size_t f = 0; f < gates.size(); ++f) gates[f]->dump_state(f);
        for (const Role& role : c_roles)
            std::fprintf(stderr, "P51_MULTILINK_LOG role=%s path=%s contents=%s\n",
                role.name.c_str(), role.log.c_str(),
                read_file_tail(role.log, 12000).c_str());
        for (const Role& role : f_roles)
            std::fprintf(stderr, "P51_MULTILINK_LOG role=%s path=%s contents=%s\n",
                role.name.c_str(), role.log.c_str(),
                read_file_tail(role.log, 12000).c_str());
        std::fprintf(stderr, "retained multi-link failure directory: %s\n", work.c_str());
        for (const auto& gate : gates) gate->release_held();
        for (auto& transfer : transfers) if (transfer.joinable()) transfer.join();
        return 1;
    }
    bool every_link_reached_w30 = true;
    for (const auto& gate : gates) {
        every_link_reached_w30 &= gate->accepted_connections() <= c_count + 8;
        every_link_reached_w30 &= gate->established_link_states() == c_count;
        every_link_reached_w30 &= gate->peak_active_links() == c_count;
        for (const Job& job : jobs) {
            if (job.f_index != static_cast<size_t>(&gate - gates.data())) continue;
            if (!job.armed_ready.load(std::memory_order_acquire)) {
                every_link_reached_w30 = false;
                continue;
            }
            const auto relation =
                icecc::p50::Id128{job.armed.logical_relationship_id};
            every_link_reached_w30 &= gate->peak_commits(relation) == jobs_per_link;
        }
    }
    REQUIRE(every_link_reached_w30,
            "each relationship uses one active physical R2 link for its 30 unique COMMITs");

    if (capacity_overlap) {
        const auto release_and_join = [&] {
            (void)p51_gate_write_marker(settlement_gate_dir + "/release-1");
            (void)p51_gate_write_marker(settlement_gate_dir + "/release-rest");
            for (const auto& gate : gates) gate->release_held();
            for (auto& transfer : transfers)
                if (transfer.joinable()) transfer.join();
        };
        bool all_four_held = gates.size() == 4;
        size_t simultaneous_pending = 0;
        for (size_t f = 0; f < gates.size(); ++f) {
            const auto relation = gates[f]->held_relationship();
            all_four_held &= relation.has_value() &&
                gates[f]->peak_commits(*relation) == 30;
            for (const Job& job : jobs) {
                if (job.f_index == f)
                    all_four_held &= job.armed_ready.load(std::memory_order_acquire) &&
                        !job.finished.load(std::memory_order_acquire);
            }
        }
        for (const Job& job : jobs)
            simultaneous_pending +=
                !job.finished.load(std::memory_order_acquire) ? 1u : 0u;
        all_four_held &= simultaneous_pending == 120;
        if (!all_four_held) {
            std::fprintf(stderr,
                "FAIL: four-link gate did not prove exact simultaneous 4x30 pending; pending=%zu\n",
                simultaneous_pending);
            for (size_t f = 0; f < gates.size(); ++f) gates[f]->dump_state(f);
            release_and_join();
            return 1;
        }
        std::fprintf(stderr,
            "P51_CAPACITY_OVERLAP_W30_HELD links=4 per_link=30 pending=120\n");

        // Complete F0's 30 transfers but retain their C operation credits
        // after the local Goodbyes; F1..F3 remain wire-pending (90 total).
        gates[0]->release_held();
        auto wait_until = [](auto predicate, std::chrono::milliseconds timeout) {
            const auto until = Clock::now() + timeout;
            while (Clock::now() < until) {
                if (predicate()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return predicate();
        };
        const bool first_link_results = wait_until([&] {
            for (const Job& job : jobs)
                if (job.f_index == 0 &&
                    !job.finished.load(std::memory_order_acquire)) return false;
            return true;
        }, std::chrono::seconds(5));
        bool all_settlements_held = true;
        for (unsigned settlement = 1; settlement <= 30; ++settlement)
            all_settlements_held &= p51_gate_wait_for_path(
                settlement_gate_dir + "/held-" + std::to_string(settlement),
                std::chrono::seconds(5));
        bool other_links_pending = true;
        for (const Job& job : jobs)
            if (job.f_index != 0)
                other_links_pending &= !job.finished.load(std::memory_order_acquire);
        if (!first_link_results || !all_settlements_held || !other_links_pending ||
            std::filesystem::exists(settlement_gate_dir + "/released-1")) {
            std::fprintf(stderr,
                "FAIL: expected exact 90 wire-pending plus 30 Goodbye-settlement credits\n");
            release_and_join();
            return 1;
        }
        std::fprintf(stderr,
            "P51_CAPACITY_OVERLAP_CREDITS wire_pending=90 goodbye_settlement=30 total=120\n");

        struct OverflowJobCleanup {
            Job *job;
            ~OverflowJobCleanup() {
                if (job == nullptr) return;
                if (job->source_fd >= 0) ::close(job->source_fd);
                job->wrapper.reset();
                job->compiler.reset();
            }
        };
        Job overflow;
        OverflowJobCleanup overflow_cleanup{&overflow};
        overflow.c_index = 0;
        overflow.f_index = 0;
        overflow.pair_index = f_roles.size();
        overflow.wire_id = static_cast<uint32_t>(0x51d001 + total_jobs);
        overflow.nonce = UINT64_C(0x51d00100000001) + total_jobs;
        overflow.compiler.reset(connect_tcp_bounded(
            f_roles[0].endpoint_port, 5000));
        overflow.wrapper.reset(Service::createChannel(
            c_roles[0].directory + "/iceccd.sock"));
        const bool c_prepared = c_roles[0].scheduler->send_msg(
            AssignPrepareMsg(epoch, overflow.wire_id, overflow.nonce, 1));
        const bool f_prepared = f_roles[0].scheduler->send_msg(
            AssignPrepareMsg(epoch, overflow.wire_id, overflow.nonce, 1));
        std::unique_ptr<Msg> c_ready_msg(c_prepared
            ? wait_for_type(c_roles[0].scheduler.get(), Msg::ASSIGN_READY, 5000)
            : nullptr);
        std::unique_ptr<Msg> f_ready_msg(f_prepared
            ? wait_for_type(f_roles[0].scheduler.get(), Msg::ASSIGN_READY, 5000)
            : nullptr);
        const auto *c_ready = dynamic_cast<const AssignReadyMsg *>(c_ready_msg.get());
        const auto *f_ready = dynamic_cast<const AssignReadyMsg *>(f_ready_msg.get());
        const bool overflow_assigned = c_ready && f_ready &&
            c_ready->wire_id == overflow.wire_id && f_ready->wire_id == overflow.wire_id &&
            c_ready->epoch() == epoch && f_ready->epoch() == epoch &&
            c_ready->nonce() == overflow.nonce && f_ready->nonce() == overflow.nonce;
        Environments overflow_envs;
        overflow_envs.emplace_back("x86_64", "multilink-env");
        GetCSMsg overflow_get(overflow_envs, "capacity-overlap.cpp",
            CompileJob::Lang_CXX, 1, "x86_64", 0, "", PROTOCOL_VERSION, 0, 0);
        overflow_get.cache_protocol = CACHE_WIRE_REVISION_R2;
        overflow_get.cache_profile_mask = profile_mask;
        const bool overflow_get_sent = overflow.wrapper &&
            overflow.wrapper->send_msg(overflow_get);
        std::unique_ptr<Msg> overflow_forwarded(overflow_get_sent
            ? wait_for_type(c_roles[0].scheduler.get(), Msg::GET_CS, 5000)
            : nullptr);
        const auto *overflow_forwarded_get =
            dynamic_cast<const GetCSMsg *>(overflow_forwarded.get());
        const bool overflow_use_sent = overflow_forwarded_get &&
            c_roles[0].scheduler->send_msg(UseCSMsg(
                "x86_64", "127.0.0.1", f_roles[0].endpoint_port,
                overflow.wire_id, true, overflow_forwarded_get->client_id, 0,
                epoch, overflow.nonce, f_roles[0].endpoint_port,
                CACHE_WIRE_REVISION_R2, profile_mask));
        std::unique_ptr<Msg> overflow_use_reply(overflow_use_sent
            ? overflow.wrapper->get_msg_until(Clock::now() + std::chrono::seconds(5))
            : nullptr);
        const auto *overflow_use =
            dynamic_cast<const UseCSMsg *>(overflow_use_reply.get());
        const bool overflow_source_ready = overflow_use &&
            overflow_use->job_id == overflow.wire_id &&
            overflow_use->assignmentEpoch() == epoch &&
            overflow_use->assignmentNonce() == overflow.nonce &&
            overflow_use->cache_endpoint_port ==
                static_cast<uint32_t>(f_roles[0].endpoint_port) &&
            overflow_use->cache_protocol == CACHE_WIRE_REVISION_R2 &&
            overflow_use->cache_profile_mask == profile_mask;
        overflow.bytes = "P51 capacity overlap immutable source 121\n";
        overflow.source_fd = make_vertical_source_fd(work, overflow.bytes);
        if (!overflow_assigned || !overflow.compiler || !overflow.wrapper ||
            !overflow_source_ready || overflow.source_fd < 0) {
            std::fprintf(stderr,
                "FAIL: exact 121st C/F assignment did not prepare a source request\n");
            release_and_join();
            return 1;
        }
        struct stat original_source_stat{};
        const bool source_stat_ok = ::fstat(overflow.source_fd,
                                            &original_source_stat) == 0;
        const off_t source_offset_before = ::lseek(overflow.source_fd, 0, SEEK_CUR);
        const int first_source_fd = ::fcntl(
            overflow.source_fd, F_DUPFD_CLOEXEC, 0);
        Clock::time_point overflow_deadline;
        P50SourceArmFields overflow_arm{};
        P51CacheControlIdentity overflow_control_identity{};
        if (first_source_fd < 0) {
            std::fprintf(stderr, "FAIL: could not duplicate immutable source for first attempt\n");
            release_and_join();
            return 1;
        }
        overflow.result = execute_p51_kind8(
            *overflow.wrapper, *overflow.compiler, overflow.wire_id, epoch,
            overflow.nonce, static_cast<uint32_t>(f_roles[0].endpoint_port),
            first_source_fd, profile_mask, &overflow.armed,
            &overflow.armed_ready, &overflow_deadline, false, &overflow_arm,
            &overflow_control_identity);
        overflow.finished.store(true, std::memory_order_release);
        const auto capacity_error = static_cast<uint16_t>(
            icecc::p50::local::SourceTransferErrorCode::CapacityBusy);
        const bool exact_busy = overflow.armed_ready.load(std::memory_order_acquire) &&
            overflow.result.code ==
                icecc::p50::local::SourceTransferResultCode::Error &&
            overflow.result.error_code == capacity_error &&
            overflow.result.attempts == 0 && overflow.result.raw_bytes == 0 &&
            overflow.result.tu_seq == 0 &&
            overflow.result.raw_digest == icecc::p50::Digest128{} &&
            overflow.result.c_store_guid == icecc::p50::CStoreGuid{} && source_stat_ok &&
            source_offset_before == 0 &&
            ::lseek(overflow.source_fd, 0, SEEK_CUR) == 0 &&
            overflow_deadline > Clock::now();
        if (!exact_busy) {
            std::fprintf(stderr,
                "FAIL: request 121 did not receive exact pre-read CapacityBusy code=%u error=%u attempts=%u\n",
                static_cast<unsigned>(overflow.result.code),
                overflow.result.error_code, overflow.result.attempts);
            release_and_join();
            return 1;
        }
        std::fprintf(stderr,
            "P51_CAPACITY_OVERLAP_121_BUSY job=%u epoch=%llu nonce=%llu request=%llu attempts=0 witness=none source_offset=0\n",
            overflow.wire_id, static_cast<unsigned long long>(epoch),
            static_cast<unsigned long long>(overflow.nonce),
            static_cast<unsigned long long>(overflow_arm.source_request_id));

        std::string first_goodbye;
        std::array<uint64_t, 31> held_request_ids{};
        {
            std::ifstream marker(settlement_gate_dir + "/held-1");
            std::getline(marker, first_goodbye);
        }
        bool one_prior_assignment_released = false;
        unsigned long first_ordinal = 0;
        unsigned long long first_request = 0;
        if (std::sscanf(first_goodbye.c_str(), "ordinal=%lu request=%llu",
                        &first_ordinal, &first_request) != 2)
            first_goodbye.clear();
        held_request_ids[1] = static_cast<uint64_t>(first_request);
        bool held_markers_valid = !first_goodbye.empty() && first_ordinal == 1;
        for (unsigned ordinal = 2; ordinal <= 30; ++ordinal) {
            std::string line;
            std::ifstream marker(settlement_gate_dir + "/held-" +
                                 std::to_string(ordinal));
            unsigned parsed_ordinal = 0;
            unsigned long long parsed_request = 0;
            char trailing = '\0';
            if (!std::getline(marker, line) ||
                std::sscanf(line.c_str(), "ordinal=%u request=%llu%c",
                            &parsed_ordinal, &parsed_request, &trailing) != 2 ||
                parsed_ordinal != ordinal || parsed_request == 0) {
                held_markers_valid = false;
                break;
            }
            held_request_ids[ordinal] = static_cast<uint64_t>(parsed_request);
        }
        for (const Job& job : jobs) {
            if (job.f_index != 0) continue;
            if (first_request == job.nonce)
                one_prior_assignment_released = true;
        }
        if (!held_markers_valid || first_goodbye.empty() || first_ordinal != 1 ||
            !one_prior_assignment_released ||
            first_request == overflow.nonce) {
            std::fprintf(stderr,
                "FAIL: held local Goodbye marker was not one of the original 30 F0 assignments: %s\n",
                first_goodbye.c_str());
            release_and_join();
            return 1;
        }
        if (!p51_gate_write_marker(settlement_gate_dir + "/release-1")) {
            std::fprintf(stderr, "FAIL: could not release exactly one prior Goodbye settlement\n");
            release_and_join();
            return 1;
        }
        const bool first_goodbye_released = p51_wait_settlement_ack(
            settlement_gate_dir + "/released-1", held_request_ids[1],
            std::chrono::seconds(4));
        const bool other_goodbyes_still_held =
            !std::filesystem::exists(settlement_gate_dir + "/released-2");
        if (!first_goodbye_released || !other_goodbyes_still_held ||
            Clock::now() >= overflow_deadline) {
            std::fprintf(stderr,
                "FAIL: one exact prior Goodbye did not release while 29 credits remained held\n");
            release_and_join();
            return 1;
        }
        const int retry_source_fd = ::fcntl(
            overflow.source_fd, F_DUPFD_CLOEXEC, 0);
        if (retry_source_fd < 0) {
            std::fprintf(stderr, "FAIL: could not duplicate retained 121st source for retry\n");
            release_and_join();
            return 1;
        }
        struct stat retry_source_stat{};
        const bool retry_stat_ok = ::fstat(retry_source_fd, &retry_source_stat) == 0;
        const auto clock = icecc::p50::sidecar::process_monotonic_clock_identity();
        const auto original_absolute_deadline =
            icecc::p50::sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
                overflow_deadline, clock.clock_domain_id, clock.time_namespace_id);
        const icecc::p50::local::P51SourceTransferRequest original_request{
            overflow.armed, original_absolute_deadline};
        const icecc::p50::local::P51SourceTransferRequest retried_request{
            overflow.armed,
            icecc::p50::sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
                overflow_deadline, clock.clock_domain_id, clock.time_namespace_id)};
        const bool exact_retry_request = original_request == retried_request &&
            overflow_control_identity.valid() &&
            overflow_arm == overflow.armed.arm.source && retry_stat_ok &&
            retry_source_stat.st_dev == original_source_stat.st_dev &&
            retry_source_stat.st_ino == original_source_stat.st_ino &&
            retry_source_stat.st_size == original_source_stat.st_size;
        if (!exact_retry_request) {
            ::close(retry_source_fd);
            std::fprintf(stderr,
                "FAIL: same-assignment retry changed its immutable source, ARM, or absolute deadline\n");
            release_and_join();
            return 1;
        }
        const auto retry_result = retry_p51_kind8_same_arm(
            *overflow.wrapper, overflow.wire_id, epoch, overflow.nonce,
            overflow.armed, retry_source_fd, profile_mask, overflow_deadline,
            overflow_control_identity);
        bool retry_committed = retry_result.code ==
                icecc::p50::local::SourceTransferResultCode::Committed &&
            retry_result.error_code == 0 && retry_result.attempts == 1 &&
            retry_result.raw_bytes == overflow.bytes.size() &&
            retry_result.raw_digest == icecc::digest128(overflow.bytes) &&
            retry_result.c_store_guid.bytes ==
                overflow.armed.arm.source.c_store_guid;
        bool first_f_arm_once = false;
        {
            std::ifstream f_log(f_roles[0].log);
            const std::string trace((std::istreambuf_iterator<char>(f_log)),
                                    std::istreambuf_iterator<char>());
            const std::string marker = "P51_TEST_ARM_TRACE request=" +
                std::to_string(overflow.nonce) + " ";
            first_f_arm_once = trace.find(marker) != std::string::npos &&
                trace.find(marker, trace.find(marker) + marker.size()) ==
                    std::string::npos;
        }
        if (!retry_committed || !first_f_arm_once ||
            gates[0]->followup_commits() != 1 ||
            std::filesystem::exists(settlement_gate_dir + "/released-2")) {
            std::fprintf(stderr,
                "FAIL: retry was not one same-ARM committed follow-up while 119 credits remained occupied\n");
            for (const auto& gate : gates) gate->release_held();
            release_and_join();
            return 1;
        }
        overflow.result = retry_result;
        std::fprintf(stderr,
            "P51_CAPACITY_OVERLAP_RETRY_COMMITTED same_assignment=1 same_ARM=1 exact_source=1 exact_deadline=1 c_identity_unchanged=1 followup_commits=%zu\n",
            gates[0]->followup_commits());

        // All remaining held replies and three 30-receipt batches now settle;
        // this must not require another ARM or strand a sibling operation.
        if (!p51_gate_write_marker(settlement_gate_dir + "/release-rest")) {
            std::fprintf(stderr, "FAIL: could not release remaining settlement acknowledgements\n");
            for (const auto& gate : gates) gate->release_held();
            release_and_join();
            return 1;
        }
        for (unsigned ordinal = 2; ordinal <= 30; ++ordinal)
            all_settlements_held &= p51_wait_settlement_ack(
                settlement_gate_dir + "/released-" + std::to_string(ordinal),
                held_request_ids[ordinal], std::chrono::seconds(2));
        for (size_t f = 1; f < gates.size(); ++f) gates[f]->release_held();
        const bool all_siblings_finished = wait_until([&] {
            return std::all_of(jobs.begin(), jobs.end(), [](const Job& job) {
                return job.finished.load(std::memory_order_acquire);
            });
        }, std::chrono::seconds(10));
        const bool all_siblings_committed = std::all_of(
            jobs.begin(), jobs.end(), [](const Job& job) {
                return job.result.code ==
                        icecc::p50::local::SourceTransferResultCode::Committed &&
                    job.result.error_code == 0;
            });
        if (!all_settlements_held || !all_siblings_finished ||
            !all_siblings_committed ||
            gates[0]->followup_commits() != 1) {
            std::fprintf(stderr,
                "FAIL: original 120 siblings did not each settle after the bounded release\n");
            release_and_join();
            return 1;
        }
        std::fprintf(stderr,
            "P51_CAPACITY_OVERLAP_SETTLED original_operations=120 goodbye_holds=30 goodbye_released=30 siblings_committed=120 retry_committed=1\n");

        const auto attach_offset = std::filesystem::file_size(f_roles[0].log);
        CompileJob overflow_compile = attachment_compile_job(
            overflow.wire_id, epoch, overflow.nonce,
            source_arm(overflow.wire_id, epoch, overflow.nonce,
                static_cast<uint32_t>(f_roles[0].endpoint_port),
                static_cast<uint32_t>(f_roles[0].endpoint_port)), nullptr);
        CompileInputIdentity overflow_identity;
        overflow_identity.profile = profile_mask == CACHE_PROFILE_P29V1
            ? CompileInputIdentity::P29V1Profile
            : profile_mask == CACHE_PROFILE_ZSTD_ROUTE
                ? CompileInputIdentity::ZstdRouteProfile
                : CompileInputIdentity::ZstdTuProfile;
        overflow_identity.c_store_guid = retry_result.c_store_guid.bytes;
        overflow_identity.tu_seq = retry_result.tu_seq;
        overflow_identity.raw_bytes = retry_result.raw_bytes;
        overflow_identity.raw_digest = retry_result.raw_digest.bytes;
        overflow_identity.attempt_id = overflow.nonce;
        overflow_identity.request_id = overflow.nonce;
        overflow_compile.setCompileInputIdentity(overflow_identity);
        const bool overflow_compile_sent = overflow.compiler->send_msg(
            CompileFileMsg(&overflow_compile));
        const std::string overflow_attach_marker =
            "P50_INPUT_ATTACH_END job=" + std::to_string(overflow.wire_id) +
            " epoch=" + std::to_string(epoch) + " nonce=" +
            std::to_string(overflow.nonce) + " request=" +
            std::to_string(overflow.nonce) + " elapsed_ms=";
        bool overflow_attached = overflow_compile_sent && wait_attachment_log(
            f_roles[0].log, attach_offset, overflow_attach_marker, 10000);
        if (overflow_attached) {
            const auto suffix = read_file_suffix(f_roles[0].log, attach_offset);
            const size_t marker_at = suffix.find(overflow_attach_marker);
            const size_t line_end = suffix.find('\n', marker_at);
            const std::string_view attach_line(suffix.data() + marker_at,
                (line_end == std::string::npos ? suffix.size() : line_end) - marker_at);
            overflow_attached = attach_line.find(" status=0") != std::string_view::npos;
        }
        const bool overflow_eof = overflow_attached &&
            wait_eof(overflow.compiler.get(), 10000);
        if (!overflow_eof) {
            std::fprintf(stderr,
                "FAIL: request 121 did not attach exact bytes after same-assignment retry\n");
            release_and_join();
            return 1;
        }
        std::fprintf(stderr,
            "P51_MULTILINK_CAPACITY_OVERLAP_PASS topology=C1F4 links=4 per_link=30 initial_operations=120 settlement_overlap=30+90 request121=typed_Busy_then_same_assignment_success one_ARM=1 siblings_progress=1\n");
    }

    if (!capacity_overlap) {
    const auto held_relation = gates[0]->held_relationship();
    REQUIRE(held_relation.has_value(),
            "one relationship is held at its 30-receipt boundary for cross-link progress");
    if (!held_relation) {
        for (const auto& gate : gates) gate->release_held();
        for (auto& transfer : transfers) if (transfer.joinable()) transfer.join();
        return 1;
    }
    bool healthy_links_ready = true;
    if (f_count == 1) {
        healthy_links_ready = gates[0]->wait_for_healthy_links(std::chrono::seconds(45));
    } else {
        for (size_t f = 1; f < gates.size(); ++f)
            healthy_links_ready &= gates[f]->wait_for_all_links(std::chrono::seconds(45));
    }
    const auto healthy_deadline = Clock::now() + std::chrono::seconds(45);
    while (healthy_links_ready && Clock::now() < healthy_deadline) {
        bool all_healthy_finished = true;
        for (const Job& job : jobs) {
            if (job.armed_ready.load(std::memory_order_acquire) &&
                icecc::p50::Id128{job.armed.logical_relationship_id} == *held_relation)
                continue;
            all_healthy_finished &= job.finished.load(std::memory_order_acquire);
        }
        if (all_healthy_finished) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool all_healthy_finished = true;
    bool held_still_pending = true;
    for (const Job& job : jobs) {
        if (!job.armed_ready.load(std::memory_order_acquire)) {
            all_healthy_finished = false;
            held_still_pending = false;
            continue;
        }
        const auto relationship =
            icecc::p50::Id128{job.armed.logical_relationship_id};
        const bool held = relationship == *held_relation;
        if (held) held_still_pending &= !job.finished.load(std::memory_order_acquire);
        else all_healthy_finished &= job.finished.load(std::memory_order_acquire);
    }
    REQUIRE(healthy_links_ready && all_healthy_finished && held_still_pending,
            "healthy C/F link jobs finish while the selected relationship receipts remain held");
    for (const auto& gate : gates) gate->release_held();
    }
    for (auto& transfer : transfers) if (transfer.joinable()) transfer.join();

    std::map<icecc::p50::Id128, std::set<uint64_t>> sequences_by_link;
    std::map<icecc::p50::CStoreGuid, std::set<uint64_t>> sequences_by_c_store;
    std::map<size_t, icecc::p50::CStoreGuid> c_guids;
    std::map<size_t, std::array<uint8_t, 16>> f_guids;
    std::map<size_t, icecc::p50::Id128> relation_by_pair;
    bool exact_results = true;
    for (Job& job : jobs) {
        const auto& result = job.result;
        exact_results &= job.armed_ready.load(std::memory_order_acquire) &&
            result.code == icecc::p50::local::SourceTransferResultCode::Committed &&
            result.error_code == 0 && result.raw_bytes == job.bytes.size() &&
            result.raw_digest == icecc::digest128(job.bytes) &&
            result.c_store_guid != icecc::p50::CStoreGuid{} &&
            job.armed.selected_revision == CACHE_WIRE_REVISION_R2 &&
            job.armed.selected_window == jobs_per_link &&
            job.armed.arm.source.cache_profile == profile_mask &&
            job.armed.relationship_epoch != 0 &&
            job.armed.f_store_generation != 0;
        const auto relationship =
            icecc::p50::Id128{job.armed.logical_relationship_id};
        sequences_by_link[relationship].insert(result.tu_seq);
        sequences_by_c_store[result.c_store_guid].insert(result.tu_seq);
        const auto [c_it, c_inserted] = c_guids.emplace(job.c_index, result.c_store_guid);
        if (!c_inserted) exact_results &= c_it->second == result.c_store_guid;
        const auto [f_it, f_inserted] = f_guids.emplace(job.f_index, job.armed.f_store_guid);
        if (!f_inserted) exact_results &= f_it->second == job.armed.f_store_guid;
        const auto [pair_it, pair_inserted] = relation_by_pair.emplace(
            job.pair_index, relationship);
        if (!pair_inserted) exact_results &= pair_it->second == relationship;
    }
    std::set<icecc::p50::Id128> distinct_relationships;
    for (const auto& [pair, relation] : relation_by_pair) {
        (void)pair;
        distinct_relationships.insert(relation);
    }
    exact_results &= relation_by_pair.size() == pair_count &&
        distinct_relationships.size() == pair_count && sequences_by_link.size() == pair_count;
    for (const auto& [relation, sequences] : sequences_by_link) {
        (void)relation;
        exact_results &= sequences.size() == jobs_per_link;
    }
    if (c_count > 1) {
        std::set<icecc::p50::CStoreGuid> distinct_c_stores;
        for (const auto& [index, guid] : c_guids) {
            (void)index;
            distinct_c_stores.insert(guid);
        }
        exact_results &= c_guids.size() == c_count &&
            distinct_c_stores.size() == c_count;
    }
    if (f_count > 1) {
        std::set<std::array<uint8_t, 16>> distinct_f_stores;
        for (const auto& [index, guid] : f_guids) {
            (void)index;
            distinct_f_stores.insert(guid);
        }
        exact_results &= f_guids.size() == f_count &&
            distinct_f_stores.size() == f_count;
    }
    exact_results &= sequences_by_c_store.size() == c_count;
    for (const auto& [store, sequences] : sequences_by_c_store) {
        (void)store;
        const uint64_t expected = jobs_per_link * f_count;
        exact_results &= sequences.size() == expected && !sequences.empty() &&
            *sequences.begin() == 0 && *sequences.rbegin() == expected - 1;
    }
    REQUIRE(exact_results,
            "links preserve selected profile/store identity, unique 30-TU bundles, and C-wide contiguous allocation");

    bool compile_sent = true;
    for (Job& job : jobs) {
        CompileJob compile_job = attachment_compile_job(
            job.wire_id, epoch, job.nonce,
            source_arm(job.wire_id, epoch, job.nonce,
                static_cast<uint32_t>(f_roles[job.f_index].endpoint_port),
                static_cast<uint32_t>(f_roles[job.f_index].endpoint_port)), nullptr);
        CompileInputIdentity identity;
        identity.profile = profile_mask == CACHE_PROFILE_P29V1
            ? CompileInputIdentity::P29V1Profile
            : profile_mask == CACHE_PROFILE_ZSTD_ROUTE
                ? CompileInputIdentity::ZstdRouteProfile
                : CompileInputIdentity::ZstdTuProfile;
        identity.c_store_guid = job.result.c_store_guid.bytes;
        identity.tu_seq = job.result.tu_seq;
        identity.raw_bytes = job.result.raw_bytes;
        identity.raw_digest = job.result.raw_digest.bytes;
        identity.attempt_id = job.nonce;
        identity.request_id = job.nonce;
        compile_job.setCompileInputIdentity(identity);
        std::error_code error;
        job.attach_log_offset = std::filesystem::file_size(
            f_roles[job.f_index].log, error);
        job.compile_sent = !error && job.compiler &&
            job.compiler->send_msg(CompileFileMsg(&compile_job));
        compile_sent &= job.compile_sent;
    }
    REQUIRE(compile_sent,
            "every original F compiler socket accepts its exact committed CompileFile identity");
    bool attached = true;
    bool bounded = true;
    for (Job& job : jobs) {
        if (!job.compile_sent) { attached = bounded = false; continue; }
        const std::string marker = "P50_INPUT_ATTACH_END job=" +
            std::to_string(job.wire_id) + " epoch=" + std::to_string(epoch) +
            " nonce=" + std::to_string(job.nonce) + " request=" +
            std::to_string(job.nonce) + " elapsed_ms=";
        bool found = wait_attachment_log(f_roles[job.f_index].log,
            job.attach_log_offset, marker, 15000);
        if (found) {
            const auto suffix = read_file_suffix(f_roles[job.f_index].log,
                                                 job.attach_log_offset);
            const size_t start = suffix.find(marker);
            if (start == std::string::npos) {
                found = false;
            } else {
                const size_t end = suffix.find('\n', start);
                const std::string_view line(suffix.data() + start,
                    (end == std::string::npos ? suffix.size() : end) - start);
                found = line.find(" status=0") != std::string_view::npos;
            }
        }
        job.attached = found;
        job.compile_bounded = wait_eof(job.compiler.get(), 15000);
        attached &= job.attached;
        bounded &= job.compile_bounded;
    }
    REQUIRE(attached, "all F roles materialize the exact 30-per-link input attachments");
    REQUIRE(bounded, "all multi-link CompileFile operations settle within bounded deadlines");

    size_t accepted_attempts = 0;
    size_t established_links = 0;
    size_t peak_active_links = 0;
    for (const auto& gate : gates) {
        accepted_attempts += gate->accepted_connections();
        established_links += gate->established_link_states();
        peak_active_links += gate->peak_active_links();
    }

    for (Job& job : jobs) {
        job.wrapper.reset();
        job.compiler.reset();
    }
    gates.clear();
    int daemon_exit_count = 0;
    for (Role& role : c_roles) {
        (void)::kill(role.pid, SIGTERM);
        int status = 0;
        if (wait_child(role.pid, 10000, &status)) {
            role.pid = -1;
            daemon_exit_count += WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    }
    for (Role& role : f_roles) {
        (void)::kill(role.pid, SIGTERM);
        int status = 0;
        if (wait_child(role.pid, 10000, &status)) {
            role.pid = -1;
            daemon_exit_count += WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    }
    REQUIRE(static_cast<unsigned>(daemon_exit_count) == c_count + f_count,
            "all real sidecars terminate cleanly after the multi-link receipt test");
    if (failures == 0) std::filesystem::remove_all(work);
    else std::fprintf(stderr, "retained multi-link work directory %s: %s\n",
                      topology.c_str(), work.c_str());
    if (failures == 0) {
        std::fprintf(stderr,
            "P51_MULTILINK_W30_PASS topology=%s profile=%u links=%zu jobs=%zu "
            "accepted_attempts=%zu established_links=%zu peak_active_links=%zu\n",
            topology.c_str(), profile_mask, pair_count, total_jobs,
            accepted_attempts, established_links, peak_active_links);
    }
    return failures ? 1 : 0;
}

struct P51RestartRole {
    std::string name;
    std::string directory;
    std::string log;
    int scheduler_listener = -1;
    int scheduler_port = 0;
    int endpoint_port = 0;
    pid_t daemon_pid = -1;
    MsgChannel *scheduler = nullptr;
    bool paused = false;
    uint64_t scheduler_epoch = 0;

    bool start(const char *daemon_binary, const char *cache_service,
               uint64_t epoch, unsigned max_jobs)
    {
        scheduler_listener = listen_ephemeral(&scheduler_port);
        endpoint_port = reserve_port();
        if (scheduler_listener < 0 || endpoint_port <= 0) {
            std::fprintf(stderr,
                "P51_RESTART_START name=%s phase=reserve listener=%d endpoint=%d errno=%d\n",
                name.c_str(), scheduler_listener, endpoint_port, errno);
            return false;
        }
        daemon_pid = launch_vertical_daemon(
            daemon_binary, cache_service, directory + "/iceccd.sock",
            directory + "/envs", directory + "/runtime", log,
            scheduler_port, endpoint_port, name.c_str(), max_jobs);
        if (daemon_pid <= 0) {
            std::fprintf(stderr,
                "P51_RESTART_START name=%s phase=launch pid=%ld errno=%d\n",
                name.c_str(), static_cast<long>(daemon_pid), errno);
            return false;
        }
        scheduler = accept_channel(scheduler_listener, 10000);
        if (!scheduler) {
            int status = 0;
            const pid_t waited = ::waitpid(daemon_pid, &status, WNOHANG);
            std::fprintf(stderr,
                "P51_RESTART_START name=%s phase=scheduler-connect pid=%ld waitpid=%ld status=%d log=%s\n",
                name.c_str(), static_cast<long>(daemon_pid),
                static_cast<long>(waited), status, log.c_str());
            std::fprintf(stderr, "%s\n", read_file_suffix(log, 0).c_str());
            return false;
        }
        std::unique_ptr<Msg> initial(scheduler
            ? wait_for_type(scheduler, Msg::LOGIN, 5000) : nullptr);
        const auto *initial_login = dynamic_cast<const LoginMsg*>(initial.get());
        if (!absent(initial_login)) {
            std::fprintf(stderr,
                "P51_RESTART_START name=%s phase=initial-login type=%d log=%s\n",
                name.c_str(), initial ? static_cast<int>(*initial) : -1,
                log.c_str());
            std::fprintf(stderr, "%s\n", read_file_suffix(log, 0).c_str());
            return false;
        }
        const ConfCSMsg activate(epoch, ConfCSMsg::StrictNonce);
        if (!scheduler->send_msg(activate)) {
            std::fprintf(stderr,
                "P51_RESTART_START name=%s phase=activate-send log=%s\n",
                name.c_str(), log.c_str());
            std::fprintf(stderr, "%s\n", read_file_suffix(log, 0).c_str());
            return false;
        }
        const auto ready_deadline = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < ready_deadline) {
            std::unique_ptr<Msg> message(wait_for_type(scheduler, Msg::LOGIN, 500));
            const auto *login = dynamic_cast<const LoginMsg*>(message.get());
            if (present_revision(login, static_cast<uint32_t>(endpoint_port),
                                 CACHE_WIRE_REVISION_R2)) {
                scheduler_epoch = epoch;
                return true;
            }
        }
        std::fprintf(stderr,
            "P51_RESTART_START name=%s phase=ready-timeout endpoint=%d log=%s\n",
            name.c_str(), endpoint_port, log.c_str());
        std::fprintf(stderr, "%s\n", read_file_suffix(log, 0).c_str());
        return false;
    }

    // Close and replace only this fixture's scheduler-side peer. The daemon,
    // cache sidecar, and listener port remain untouched; this models a new
    // scheduler session/epoch and is deliberately not a shared S-process test.
    bool replace_scheduler_epoch(uint64_t epoch)
    {
        delete scheduler;
        scheduler = nullptr;
        scheduler = accept_channel(scheduler_listener, 15000);
        std::unique_ptr<Msg> initial(scheduler
            ? wait_for_type(scheduler, Msg::LOGIN, 5000) : nullptr);
        const auto *initial_login = dynamic_cast<const LoginMsg*>(initial.get());
        if (!absent(initial_login)) {
            std::fprintf(stderr,
                "P51_SYNTH_SCHEDULER name=%s phase=initial-login type=%d\n",
                name.c_str(), initial ? static_cast<int>(*initial) : -1);
            return false;
        }
        const ConfCSMsg activate(epoch, ConfCSMsg::StrictNonce);
        if (!scheduler || !scheduler->send_msg(activate)) {
            std::fprintf(stderr,
                "P51_SYNTH_SCHEDULER name=%s phase=activate-send epoch=%llu\n",
                name.c_str(), static_cast<unsigned long long>(epoch));
            return false;
        }
        const auto ready_deadline = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < ready_deadline) {
            std::unique_ptr<Msg> message(wait_for_type(scheduler, Msg::LOGIN, 500));
            const auto *login = dynamic_cast<const LoginMsg*>(message.get());
            if (present_revision(login, static_cast<uint32_t>(endpoint_port),
                                 CACHE_WIRE_REVISION_R2)) {
                scheduler_epoch = epoch;
                return true;
            }
        }
        std::fprintf(stderr,
            "P51_SYNTH_SCHEDULER name=%s phase=ready-timeout epoch=%llu log=%s\n",
            name.c_str(), static_cast<unsigned long long>(epoch), log.c_str());
        std::fprintf(stderr, "%s\n", read_file_suffix(log, 0).c_str());
        return false;
    }

    pid_t wait_for_restarted_cache_sidecar(const char *cache_service,
                                           pid_t old_sidecar)
    {
        if (old_sidecar <= 1) return -1;
        const auto deadline = Clock::now() + std::chrono::seconds(15);
        while (Clock::now() < deadline) {
            const pid_t current = find_attachment_sidecar(daemon_pid, cache_service);
            if (current > 1 && current != old_sidecar) {
                std::unique_ptr<Msg> message(scheduler ? scheduler->get_msg(1, true)
                                                       : nullptr);
                const auto *login = dynamic_cast<const LoginMsg*>(message.get());
                if (present_revision(login, static_cast<uint32_t>(endpoint_port),
                                     CACHE_WIRE_REVISION_R2))
                    return current;
            }
            ::usleep(10000);
        }
        return -1;
    }

    void stop() noexcept
    {
        if (daemon_pid > 1) {
            if (paused) {
                (void)::kill(daemon_pid, SIGCONT);
                paused = false;
            }
            (void)::kill(daemon_pid, SIGTERM);
            int status = 0;
            if (!wait_child(daemon_pid, 5000, &status)) {
                (void)::kill(daemon_pid, SIGKILL);
                (void)::waitpid(daemon_pid, &status, 0);
            }
            daemon_pid = -1;
        }
        delete scheduler;
        scheduler = nullptr;
        if (scheduler_listener >= 0) {
            ::close(scheduler_listener);
            scheduler_listener = -1;
        }
    }
};

struct P51RestartJob {
    MsgChannel *wrapper = nullptr;
    MsgChannel *compiler = nullptr;
    int source_fd = -1;
    uint32_t wire_id = 0;
    uint64_t epoch = 0;
    uint64_t nonce = 0;
    std::string bytes;

    ~P51RestartJob()
    {
        if (source_fd >= 0) ::close(source_fd);
        delete wrapper;
        delete compiler;
    }
};

struct P51RestartTransferCell {
    using Result = icecc::p50::local::P50SourceTransferResult;

    std::unique_ptr<P51RestartJob> job;
    P51SourceArmedFields armed{};
    std::promise<Result> promise;
    std::future<Result> future;
    std::thread worker;
    Result result{};
    Clock::time_point source_deadline{};
    Clock::time_point finished_at{};
    bool settled = false;
    bool source_deadline_captured = false;

    explicit P51RestartTransferCell(std::unique_ptr<P51RestartJob> value)
        : job(std::move(value)), future(promise.get_future()) {}

    ~P51RestartTransferCell()
    {
        if (worker.joinable()) worker.join();
    }
};

static std::unique_ptr<P51RestartJob> prepare_p51_restart_job(
    P51RestartRole& c, P51RestartRole& f, const std::string& source_root,
    uint32_t wire_id, uint64_t nonce, uint64_t epoch, uint32_t profile_mask)
{
    auto job = std::make_unique<P51RestartJob>();
    job->wire_id = wire_id;
    job->epoch = epoch;
    job->nonce = nonce;
    job->bytes = "int p51_restart_value_" + std::to_string(wire_id) +
                 " = " + std::to_string(wire_id) + ";\n";
    if (!c.scheduler->send_msg(AssignPrepareMsg(epoch, wire_id, nonce, 1)) ||
        !f.scheduler->send_msg(AssignPrepareMsg(epoch, wire_id, nonce, 1)))
        return nullptr;
    std::unique_ptr<Msg> c_ready_msg(wait_for_type(c.scheduler, Msg::ASSIGN_READY, 5000));
    std::unique_ptr<Msg> f_ready_msg(wait_for_type(f.scheduler, Msg::ASSIGN_READY, 5000));
    const auto *c_ready = dynamic_cast<const AssignReadyMsg*>(c_ready_msg.get());
    const auto *f_ready = dynamic_cast<const AssignReadyMsg*>(f_ready_msg.get());
    if (!c_ready || !f_ready || c_ready->wire_id != wire_id ||
        f_ready->wire_id != wire_id || c_ready->epoch() != epoch ||
        f_ready->epoch() != epoch || c_ready->nonce() != nonce ||
        f_ready->nonce() != nonce)
        return nullptr;

    job->compiler = connect_tcp_bounded(f.endpoint_port, 5000);
    job->wrapper = Service::createChannel(c.directory + "/iceccd.sock");
    Environments source_envs;
    source_envs.emplace_back("x86_64", "p51-restart-env");
    GetCSMsg get(source_envs, "restart.cc", CompileJob::Lang_CXX, 1,
                 "x86_64", 0, "", PROTOCOL_VERSION, 0, 0);
    get.cache_protocol = CACHE_WIRE_REVISION_R2;
    get.cache_profile_mask = profile_mask;
    if (!job->compiler || !job->wrapper || !job->wrapper->send_msg(get))
        return nullptr;
    std::unique_ptr<Msg> forwarded_msg(wait_for_type(c.scheduler, Msg::GET_CS, 5000));
    const auto *forwarded = dynamic_cast<const GetCSMsg*>(forwarded_msg.get());
    if (!forwarded || forwarded->client_id == 0 ||
        !c.scheduler->send_msg(UseCSMsg(
            "x86_64", "127.0.0.1", f.endpoint_port, wire_id, true,
            forwarded->client_id, 0, epoch, nonce, f.endpoint_port,
            CACHE_WIRE_REVISION_R2, profile_mask)))
        return nullptr;
    std::unique_ptr<Msg> use_msg(job->wrapper->get_msg_until(
        Clock::now() + std::chrono::seconds(5)));
    const auto *use = dynamic_cast<const UseCSMsg*>(use_msg.get());
    if (!use || use->job_id != wire_id || use->assignmentEpoch() != epoch ||
        use->assignmentNonce() != nonce ||
        use->cache_protocol != CACHE_WIRE_REVISION_R2 ||
        use->cache_profile_mask != profile_mask)
        return nullptr;
    job->source_fd = make_vertical_source_fd(source_root, job->bytes);
    return job->source_fd >= 0 ? std::move(job) : nullptr;
}

static icecc::p50::local::P50SourceTransferResult run_p51_restart_job(
    P51RestartRole& f, P51RestartJob& job,
    uint32_t profile_mask, P51SourceArmedFields *armed = nullptr,
    Clock::time_point *source_deadline = nullptr)
{
    const int source_fd = std::exchange(job.source_fd, -1);
    return execute_p51_kind8(
        *job.wrapper, *job.compiler, job.wire_id,
        job.epoch, job.nonce,
        static_cast<uint32_t>(f.endpoint_port), source_fd,
        profile_mask, armed, nullptr, source_deadline);
}

static bool start_p51_restart_transfers(
    std::vector<std::unique_ptr<P51RestartTransferCell>>& cells,
    P51RestartRole& f, uint32_t profile_mask)
{
    try {
        for (auto& owned : cells) {
            P51RestartTransferCell *cell = owned.get();
            cell->worker = std::thread([cell, &f, profile_mask] {
                P51RestartTransferCell::Result result{};
                try {
                    result = run_p51_restart_job(
                        f, *cell->job, profile_mask, &cell->armed,
                        &cell->source_deadline);
                    cell->source_deadline_captured = true;
                } catch (...) {
                    std::fprintf(stderr,
                        "P51_RESTART_TRANSFER exception job=%u\n",
                        cell->job ? cell->job->wire_id : 0u);
                }
                cell->finished_at = Clock::now();
                cell->promise.set_value(std::move(result));
            });
        }
    } catch (...) {
        return false;
    }
    return true;
}

static bool wait_p51_restart_transfers(
    std::vector<std::unique_ptr<P51RestartTransferCell>>& cells,
    Clock::time_point deadline)
{
    bool all_settled = true;
    for (auto& cell : cells) {
        if (cell->settled) continue;
        if (cell->future.wait_until(deadline) != std::future_status::ready) {
            all_settled = false;
            continue;
        }
        cell->result = cell->future.get();
        cell->settled = true;
    }
    return all_settled;
}

static bool p51_restart_deadlines_respected(
    const std::vector<std::unique_ptr<P51RestartTransferCell>>& cells,
    std::chrono::milliseconds cleanup_grace,
    int64_t *max_completion_delta_ms = nullptr)
{
    bool respected = true;
    int64_t max_delta_ms = std::numeric_limits<int64_t>::min();
    for (const auto& cell : cells) {
        if (!cell->settled || !cell->source_deadline_captured) {
            respected = false;
            continue;
        }
        const int64_t delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            cell->finished_at - cell->source_deadline).count();
        max_delta_ms = std::max(max_delta_ms, delta_ms);
        if (cell->finished_at > cell->source_deadline + cleanup_grace)
            respected = false;
    }
    if (max_completion_delta_ms != nullptr)
        *max_completion_delta_ms = max_delta_ms == std::numeric_limits<int64_t>::min()
            ? 0 : max_delta_ms;
    return respected;
}

static void join_p51_restart_transfers(
    std::vector<std::unique_ptr<P51RestartTransferCell>>& cells)
{
    for (auto& cell : cells)
        if (cell->worker.joinable()) cell->worker.join();
}

static bool attach_p51_restart_job(
    P51RestartRole& f, P51RestartJob& job, uint32_t profile_mask,
    const P51SourceArmedFields& armed,
    const icecc::p50::local::P50SourceTransferResult& result)
{
    const auto expected_digest = icecc::digest128(job.bytes);
    if (result.code != icecc::p50::local::SourceTransferResultCode::Committed ||
        job.compiler == nullptr || result.raw_bytes != job.bytes.size() ||
        result.raw_digest != expected_digest) {
        std::fprintf(stderr,
            "P51_RESTART_ATTACH precondition job=%u result_code=%u compiler=%u raw=%llu/%zu digest_match=%u c_guid_prefix=%02x%02x tu=%llu\n",
            job.wire_id, static_cast<unsigned>(result.code),
            job.compiler != nullptr ? 1u : 0u,
            static_cast<unsigned long long>(result.raw_bytes), job.bytes.size(),
            result.raw_digest == expected_digest ? 1u : 0u,
            static_cast<unsigned>(result.c_store_guid.bytes[0]),
            static_cast<unsigned>(result.c_store_guid.bytes[1]),
            static_cast<unsigned long long>(result.tu_seq));
        return false;
    }
    CompileJob compile_job = attachment_compile_job(
        job.wire_id, job.epoch, job.nonce, armed.arm.source, &result);
    CompileInputIdentity identity;
    identity.profile = profile_mask == CACHE_PROFILE_P29V1
        ? CompileInputIdentity::P29V1Profile
        : profile_mask == CACHE_PROFILE_ZSTD_ROUTE
            ? CompileInputIdentity::ZstdRouteProfile
            : CompileInputIdentity::ZstdTuProfile;
    identity.c_store_guid = result.c_store_guid.bytes;
    identity.tu_seq = result.tu_seq;
    identity.raw_bytes = result.raw_bytes;
    identity.raw_digest = result.raw_digest.bytes;
    identity.attempt_id = job.nonce;
    identity.request_id = job.nonce;
    compile_job.setCompileInputIdentity(identity);
    std::error_code error;
    const uintmax_t offset = std::filesystem::file_size(f.log, error);
    if (error || !job.compiler->send_msg(CompileFileMsg(&compile_job))) {
        std::fprintf(stderr,
            "P51_RESTART_ATTACH send_failed job=%u file_size_error=%s\n",
            job.wire_id, error ? error.message().c_str() : "none");
        return false;
    }
    const std::string marker = "P50_INPUT_ATTACH_END job=" +
        std::to_string(job.wire_id) + " epoch=" + std::to_string(job.epoch) +
        " nonce=" + std::to_string(job.nonce) + " request=" +
        std::to_string(job.nonce) + " elapsed_ms=";
    if (!wait_attachment_log(f.log, offset, marker, 15000)) {
        std::fprintf(stderr,
            "P51_RESTART_ATTACH marker_timeout job=%u log=%s\n",
            job.wire_id, f.log.c_str());
        return false;
    }
    const std::string suffix = read_file_suffix(f.log, offset);
    const size_t marker_pos = suffix.find(marker);
    if (marker_pos == std::string::npos) return false;
    const size_t line_end = suffix.find('\n', marker_pos);
    const std::string_view marker_line(suffix.data() + marker_pos,
        (line_end == std::string::npos ? suffix.size() : line_end) - marker_pos);
    const bool status_ok = marker_line.find(" status=0") != std::string_view::npos;
    const bool eof_ok = wait_eof(job.compiler, 15000);
    if (!status_ok || !eof_ok)
        std::fprintf(stderr,
            "P51_RESTART_ATTACH terminal job=%u status_ok=%u eof_ok=%u marker=%.*s\n",
            job.wire_id, status_ok ? 1u : 0u, eof_ok ? 1u : 0u,
            static_cast<int>(marker_line.size()), marker_line.data());
    return status_ok && eof_ok;
}

static bool reject_p51_stale_f_owner_attach(
    P51RestartRole& f, const P51RestartJob& old_job,
    const P51SourceArmedFields& old_armed,
    const P51SourceArmedFields& current_armed,
    const icecc::p50::R2TxCommit& witness, uint32_t profile_mask);

static int run_p51_process_restart_case(
    const char *daemon_binary, const char *cache_service, passwd *icecc,
    unsigned c_count, unsigned f_count, uint32_t profile_mask,
    unsigned jobs_per_window = 1, bool restart_chain_f_c = false)
{
    if (jobs_per_window == 0 || jobs_per_window > 30 ||
        c_count == 0 || c_count > 4 || f_count == 0 || f_count > 4 ||
        c_count * f_count > 4 ||
        (!restart_chain_f_c &&
         !((c_count == 1 && f_count >= 2) ||
           (f_count == 1 && c_count >= 2))) ||
        (restart_chain_f_c &&
         !(c_count == 2 && f_count == 2 && jobs_per_window == 30))) {
        std::fprintf(stderr,
            "FAIL: unsupported process-restart topology/window C%uF%u W%u\n",
            c_count, f_count, jobs_per_window);
        return 2;
    }
    const bool restart_f = f_count > 1;
    const char *temporary_root = ::getenv("TMPDIR");
    const std::string prefix = temporary_root && *temporary_root ? temporary_root : "/tmp";
    std::string pattern = prefix + "/p51r.XXXXXX";
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    char *created = ::mkdtemp(mutable_pattern.data());
    REQUIRE(created != nullptr, "P51 process-restart private root created");
    if (!created) return 2;
    const std::string work(created);
    REQUIRE(::chown(work.c_str(), icecc->pw_uid, icecc->pw_gid) == 0 &&
                ::chmod(work.c_str(), 0700) == 0,
            "P51 process-restart root has sidecar ownership");
    if (failures) return 2;

    const uint64_t epoch = restart_f ? UINT64_C(0x51f2000000000001)
                                     : UINT64_C(0x51c2000000000001);
    std::vector<P51RestartRole> c_roles;
    std::vector<P51RestartRole> f_roles;
    c_roles.reserve(c_count);
    f_roles.reserve(f_count);
    for (unsigned index = 0; index < c_count; ++index) {
        P51RestartRole role;
        role.name = "p51-c" + std::to_string(index + 1);
        role.directory = work + "/c" + std::to_string(index + 1);
        c_roles.push_back(std::move(role));
    }
    for (unsigned index = 0; index < f_count; ++index) {
        P51RestartRole role;
        role.name = "p51-f" + std::to_string(index + 1);
        role.directory = work + "/f" + std::to_string(index + 1);
        f_roles.push_back(std::move(role));
    }
    auto stop_roles = [&] {
        for (auto& role : c_roles) role.stop();
        for (auto& role : f_roles) role.stop();
    };
    auto all_roles = [&] {
        std::vector<P51RestartRole*> result;
        result.reserve(c_roles.size() + f_roles.size());
        for (auto& role : c_roles) result.push_back(&role);
        for (auto& role : f_roles) result.push_back(&role);
        return result;
    };
    for (P51RestartRole *role : all_roles()) {
        const bool made = ::mkdir(role->directory.c_str(), 0700) == 0 &&
            ::chown(role->directory.c_str(), icecc->pw_uid, icecc->pw_gid) == 0 &&
            ::mkdir((role->directory + "/envs").c_str(), 0700) == 0 &&
            ::chown((role->directory + "/envs").c_str(), icecc->pw_uid, icecc->pw_gid) == 0 &&
            ::mkdir((role->directory + "/runtime").c_str(), 0700) == 0 &&
            ::chown((role->directory + "/runtime").c_str(), icecc->pw_uid, icecc->pw_gid) == 0;
        REQUIRE(made, "all restart roles have private owned runtime directories");
        role->log = role->directory + "/iceccd.log";
    }
    if (failures) return 2;

    const unsigned legacy_role_limit = jobs_per_window == 1 ? 6u : 64u;
    const unsigned topology_role_limit = jobs_per_window * 2u +
        2u * (c_count + f_count - 2u) + 2u;
    const unsigned role_job_limit = std::max(legacy_role_limit,
                                              topology_role_limit);
    bool roles_ready = true;
    for (auto& role : c_roles)
        roles_ready &= role.start(daemon_binary, cache_service, epoch,
                                  role_job_limit);
    for (auto& role : f_roles)
        roles_ready &= role.start(daemon_binary, cache_service, epoch,
                                  role_job_limit);
    REQUIRE(roles_ready,
        "all independent C/F daemons start for the requested process-restart topology");
    if (!roles_ready) {
        stop_roles();
        std::fprintf(stderr, "retained process-restart work directory: %s\n", work.c_str());
        return 1;
    }
    P51RestartRole& c_affected = c_roles.front();
    P51RestartRole& f_affected = f_roles.front();
    const std::string topology = "C" + std::to_string(c_count) +
                                 "F" + std::to_string(f_count);
    const uint32_t wire_base = restart_f ? 0x51f21000 : 0x51c21000;
    auto make_job = [&](P51RestartRole& c, P51RestartRole& f,
                        uint32_t offset) {
        const uint32_t wire = wire_base + offset;
        const uint64_t nonce = (static_cast<uint64_t>(wire) << 32) | offset;
        return prepare_p51_restart_job(c, f, work, wire, nonce, epoch,
                                       profile_mask);
    };
    bool healthy_before = true;
    bool healthy_during = true;
    uint32_t healthy_before_count = 0;
    uint32_t healthy_during_count = 0;
    bool fresh_attached = false;
    bool stop_parent_ok = false;
    bool kill_sidecar_ok = false;
    bool resumed_parent_ok = false;
    pid_t old_sidecar = -1;
    pid_t restarted_sidecar = -1;
    pid_t restart_parent = -1;
    std::vector<std::unique_ptr<P51RestartJob>> healthy_initial_jobs;
    auto run_healthy = [&](P51RestartRole& c, P51RestartRole& f,
                           uint32_t offset, const char *phase) {
        auto job = make_job(c, f, offset);
        P51SourceArmedFields armed{};
        const auto result = job
            ? run_p51_restart_job(f, *job, profile_mask, &armed)
            : icecc::p50::local::P50SourceTransferResult{};
        const bool attached = job && attach_p51_restart_job(
            f, *job, profile_mask, armed, result);
        std::fprintf(stderr,
            "P51_RESTART_HEALTHY topology=%s profile=%u phase=%s c=%s f=%s "
            "committed=%u attached=%u raw=%llu expected=%zu digest_match=%u\n",
            topology.c_str(), profile_mask, phase, c.name.c_str(), f.name.c_str(),
            result.code == icecc::p50::local::SourceTransferResultCode::Committed,
            attached ? 1u : 0u,
            static_cast<unsigned long long>(result.raw_bytes),
            job ? job->bytes.size() : 0,
            job && result.raw_digest == icecc::digest128(job->bytes) ? 1u : 0u);
        if (job) healthy_initial_jobs.push_back(std::move(job));
        return attached;
    };
    uint32_t next_offset = 1;
    if (restart_f) {
        for (size_t index = 1; index < f_roles.size(); ++index) {
            const bool attached = run_healthy(c_affected, f_roles[index],
                                              next_offset++, "before");
            healthy_before &= attached;
            healthy_before_count += attached;
        }
    } else {
        for (size_t index = 1; index < c_roles.size(); ++index) {
            const bool attached = run_healthy(c_roles[index], f_affected,
                                              next_offset++, "before");
            healthy_before &= attached;
            healthy_before_count += attached;
        }
    }
    REQUIRE(healthy_before &&
                healthy_before_count == (restart_f ? f_count - 1 : c_count - 1),
            "every unaffected sibling relationship establishes and attaches before restart");
    if (!healthy_before) {
        healthy_initial_jobs.clear();
        stop_roles();
        std::fprintf(stderr, "retained process-restart work directory: %s\n", work.c_str());
        return 1;
    }

    const uid_t sidecar_uid = icecc->pw_uid;
    auto gate = std::make_unique<P51CommitReceiptGate>(
        f_affected.endpoint_port, sidecar_uid, jobs_per_window);
    REQUIRE(gate->ready(), "restart gate can hold the affected COMMIT window under NET_ADMIN");
    if (!gate->ready()) {
        stop_roles();
        std::fprintf(stderr, "retained process-restart work directory: %s\n", work.c_str());
        return 2;
    }
    std::vector<std::unique_ptr<P51RestartTransferCell>> affected_cells;
    affected_cells.reserve(jobs_per_window);
    bool affected_prepared = true;
    for (unsigned index = 0; index < jobs_per_window; ++index) {
        const uint32_t offset = 100 + index;
        const uint32_t wire = wire_base + offset;
        const uint64_t nonce = (static_cast<uint64_t>(wire) << 32) | offset;
        auto job = prepare_p51_restart_job(
            c_affected, f_affected, work, wire, nonce, epoch, profile_mask);
        affected_prepared &= job != nullptr;
        if (!job) break;
        affected_cells.emplace_back(
            std::make_unique<P51RestartTransferCell>(std::move(job)));
    }
    REQUIRE(affected_prepared && affected_cells.size() == jobs_per_window,
            "affected original compiler assignments and P51 ARMs cover the restart window");
    if (!affected_prepared || affected_cells.size() != jobs_per_window) {
        gate.reset(); stop_roles();
        std::fprintf(stderr, "retained process-restart work directory: %s\n", work.c_str());
        return 1;
    }
    const bool callers_started = start_p51_restart_transfers(
        affected_cells, f_affected, profile_mask);
    REQUIRE(callers_started,
            "all affected original source callers enter one bounded R2 window");
    if (!callers_started) {
        gate->discard_held_commits();
        gate.reset();
        stop_roles();
        (void)wait_p51_restart_transfers(
            affected_cells, Clock::now() + std::chrono::seconds(5));
        join_p51_restart_transfers(affected_cells);
        std::fprintf(stderr, "retained process-restart work directory: %s\n", work.c_str());
        return 1;
    }
    const bool observed_commit = gate->wait_for_commits(
        std::chrono::seconds(jobs_per_window == 1 ? 20 : 30));
    REQUIRE(observed_commit,
            "affected relationship reaches the exact held F-committed/C-unobserved COMMIT window");
    if (!observed_commit) {
        gate->discard_held_commits();
        gate.reset();
        stop_roles();
        (void)wait_p51_restart_transfers(
            affected_cells, Clock::now() + std::chrono::seconds(5));
        join_p51_restart_transfers(affected_cells);
        std::fprintf(stderr, "retained process-restart work directory: %s\n", work.c_str());
        return 1;
    }
    REQUIRE(gate->observed_commits() == jobs_per_window,
            "restart gate retains every exact affected COMMIT before store replacement");
    const size_t held_old_commits = gate->observed_commits();
    const auto held_old_witnesses = gate->commit_witnesses();
    REQUIRE(held_old_witnesses.size() == jobs_per_window,
            "held F window exposes exact commit witnesses for the pre-restart owner");
    restart_parent = restart_f ? f_affected.daemon_pid : c_affected.daemon_pid;
    old_sidecar = find_attachment_sidecar(restart_parent, cache_service);
    stop_parent_ok = restart_parent > 1 && ::kill(restart_parent, SIGSTOP) == 0;
    if (stop_parent_ok) {
        if (restart_f) f_affected.paused = true;
        else c_affected.paused = true;
    }
    REQUIRE(old_sidecar > 1 && stop_parent_ok,
            "affected daemon and exact old sidecar are identified before replacement");
    kill_sidecar_ok = old_sidecar > 1 && ::kill(old_sidecar, SIGKILL) == 0;
    REQUIRE(kill_sidecar_ok,
            "only the pre-restart affected cache-service PID is killed after F commit");
    // A frame buffered by F is not evidence delivered to C. Drop it at the
    // proxy so the test does not confuse local positive evidence with restart.
    gate->discard_held_commits();
    gate.reset();
    if (restart_f) {
        for (size_t index = 1; index < f_roles.size(); ++index) {
            const bool attached = run_healthy(
                c_affected, f_roles[index],
                200 + static_cast<uint32_t>(index - 1), "during-restart");
            healthy_during &= attached;
            healthy_during_count += attached;
        }
    } else {
        for (size_t index = 1; index < c_roles.size(); ++index) {
            const bool attached = run_healthy(
                c_roles[index], f_affected,
                200 + static_cast<uint32_t>(index - 1), "during-restart");
            healthy_during &= attached;
            healthy_during_count += attached;
        }
    }
    const bool healthy_parent_stopped =
        restart_f ? f_affected.paused : c_affected.paused;
    REQUIRE(healthy_parent_stopped &&
                healthy_during && healthy_during_count ==
                    (restart_f ? f_count - 1 : c_count - 1),
            "every pre-existing healthy sibling commits and attaches while affected daemon is stopped");

    resumed_parent_ok = stop_parent_ok &&
        ::kill(restart_parent, SIGCONT) == 0;
    if (resumed_parent_ok) {
        if (restart_f) f_affected.paused = false;
        else c_affected.paused = false;
    }
    REQUIRE(resumed_parent_ok,
            "affected daemon resumes its cache-service replacement supervisor");
    restarted_sidecar = resumed_parent_ok
        ? (restart_f ? f_affected.wait_for_restarted_cache_sidecar(
                            cache_service, old_sidecar)
                     : c_affected.wait_for_restarted_cache_sidecar(
                            cache_service, old_sidecar)) : -1;
    REQUIRE(restarted_sidecar > 1,
            "the affected daemon publishes a fresh sidecar process/incarnation");

    const bool affected_settled = wait_p51_restart_transfers(
        affected_cells, Clock::now() + std::chrono::seconds(35));
    REQUIRE(affected_settled,
            "all original W30 callers settle before the outer cleanup watchdog");
    if (!affected_settled) {
        std::fprintf(stderr,
            "P51_PROCESS_RESTART affected W30 callers did not settle by bounded wait\n");
        for (P51RestartRole *role : all_roles())
            if (role->daemon_pid > 1) (void)::kill(role->daemon_pid, SIGKILL);
        const bool settled_after_close = wait_p51_restart_transfers(
            affected_cells, Clock::now() + std::chrono::seconds(5));
        join_p51_restart_transfers(affected_cells);
        REQUIRE(settled_after_close,
                "closing all topology daemons releases every original transfer worker");
        stop_roles();
        std::fprintf(stderr, "retained process-restart work directory: %s\n", work.c_str());
        return 1;
    }
    join_p51_restart_transfers(affected_cells);
    constexpr auto kSourceDeadlineCleanupGrace = std::chrono::seconds(2);
    int64_t affected_max_deadline_delta_ms = 0;
    const bool affected_deadlines_respected = p51_restart_deadlines_respected(
        affected_cells, kSourceDeadlineCleanupGrace,
        &affected_max_deadline_delta_ms);
    std::fprintf(stderr,
        "P51_RESTART_DEADLINES topology=%s profile=%u phase=old callers=%zu max_completion_delta_ms=%lld grace_ms=%lld respected=%u\n",
        topology.c_str(), profile_mask, affected_cells.size(),
        static_cast<long long>(affected_max_deadline_delta_ms),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            kSourceDeadlineCleanupGrace).count()),
        affected_deadlines_respected ? 1u : 0u);
    REQUIRE(affected_deadlines_respected,
            "every original source caller completes by its original 30-second deadline plus 2-second cleanup grace");
    size_t affected_noncommitted = 0;
    for (const auto& cell : affected_cells)
        affected_noncommitted += cell->result.code !=
            icecc::p50::local::SourceTransferResultCode::Committed;
    REQUIRE(affected_noncommitted == jobs_per_window,
            "discarded old-incarnation COMMITs never become positive C receipts for any original caller");
    std::fprintf(stderr,
        "P51_RESTART_OLD_WINDOW topology=%s profile=%u callers=%zu held_commits=%zu noncommitted=%zu\n",
        topology.c_str(), profile_mask, affected_cells.size(),
        held_old_commits, affected_noncommitted);
    const P51SourceArmedFields old_armed = affected_cells.front()->armed;
    std::unique_ptr<P51RestartJob> stale_f_job;
    P51SourceArmedFields stale_f_armed{};
    icecc::p50::R2TxCommit stale_f_witness{};
    if (restart_chain_f_c && !affected_cells.empty() &&
        held_old_witnesses.size() == jobs_per_window) {
        const auto matching = std::find_if(held_old_witnesses.begin(),
            held_old_witnesses.end(), [&](const auto& witness) {
                return std::any_of(affected_cells.begin(), affected_cells.end(),
                    [&](const auto& cell) {
                        return cell->job && icecc::digest128(cell->job->bytes) ==
                            witness.inner.raw_digest;
                    });
            });
        if (matching != held_old_witnesses.end()) {
            const auto cell = std::find_if(affected_cells.begin(),
                affected_cells.end(), [&](const auto& candidate) {
                    return candidate->job && icecc::digest128(candidate->job->bytes) ==
                        matching->inner.raw_digest;
                });
            if (cell != affected_cells.end()) {
                stale_f_job = std::make_unique<P51RestartJob>();
                stale_f_job->wire_id = (*cell)->job->wire_id;
                stale_f_job->epoch = (*cell)->job->epoch;
                stale_f_job->nonce = (*cell)->job->nonce;
                stale_f_job->bytes = (*cell)->job->bytes;
                stale_f_armed = (*cell)->armed;
                stale_f_witness = *matching;
            }
        }
        REQUIRE(stale_f_job != nullptr,
                "a held old-F commit maps to its exact original compiler assignment");
    }
    affected_cells.clear();

    P51RestartRole& refreshed_c = c_affected;
    P51RestartRole& refreshed_f = f_affected;
    auto fresh_gate = std::make_unique<P51CommitReceiptGate>(
        refreshed_f.endpoint_port, sidecar_uid, jobs_per_window);
    REQUIRE(fresh_gate->ready(),
            "fresh replacement relationship has an independent bounded receipt gate");
    std::vector<std::unique_ptr<P51RestartTransferCell>> fresh_cells;
    fresh_cells.reserve(jobs_per_window);
    bool fresh_prepared = fresh_gate->ready();
    for (unsigned index = 0; index < jobs_per_window && fresh_prepared; ++index) {
        const uint32_t offset = 300 + index;
        const uint32_t wire = wire_base + offset;
        const uint64_t nonce = (static_cast<uint64_t>(wire) << 32) | offset;
        auto job = prepare_p51_restart_job(
            refreshed_c, refreshed_f, work, wire, nonce, epoch, profile_mask);
        fresh_prepared &= job != nullptr;
        if (!job) break;
        fresh_cells.emplace_back(
            std::make_unique<P51RestartTransferCell>(std::move(job)));
    }
    REQUIRE(fresh_prepared && fresh_cells.size() == jobs_per_window,
            "fresh replacement identity admits 30 distinct original compiler assignments");
    const bool fresh_started = fresh_prepared && start_p51_restart_transfers(
        fresh_cells, refreshed_f, profile_mask);
    REQUIRE(fresh_started,
            "fresh replacement jobs enter the full bounded W30 transfer window");
    const bool fresh_window_observed = fresh_started && fresh_gate->wait_for_commits(
        std::chrono::seconds(jobs_per_window == 1 ? 20 : 30));
    REQUIRE(fresh_window_observed,
            "fresh replacement F commits all W30 before any C receipt is released");
    uint64_t fresh_last_ordinal = 0;
    if (fresh_window_observed) {
        const auto fresh_witnesses = fresh_gate->commit_witnesses();
        for (const auto& witness : fresh_witnesses)
            fresh_last_ordinal = std::max(fresh_last_ordinal,
                                           witness.relationship_ordinal);
    }
    size_t fresh_pending_before_receipt = 0;
    if (fresh_window_observed) {
        for (const auto& cell : fresh_cells)
            fresh_pending_before_receipt += cell->future.wait_for(
                std::chrono::milliseconds(0)) != std::future_status::ready;
    }
    REQUIRE(fresh_pending_before_receipt == jobs_per_window,
            "fresh W30 source callers remain pending until cumulative receipt delivery");
    if (fresh_window_observed) fresh_gate->release_commits();
    else fresh_gate->discard_held_commits();
    bool fresh_settled = fresh_started && wait_p51_restart_transfers(
        fresh_cells, Clock::now() + std::chrono::seconds(35));
    REQUIRE(fresh_settled,
            "fresh replacement W30 callers settle before the outer cleanup watchdog");
    if (!fresh_settled) {
        for (P51RestartRole *role : all_roles())
            if (role->daemon_pid > 1) (void)::kill(role->daemon_pid, SIGKILL);
        fresh_settled = wait_p51_restart_transfers(
            fresh_cells, Clock::now() + std::chrono::seconds(5));
    }
    REQUIRE(fresh_settled,
            "closing the restarted roles releases every fresh transfer worker");
    join_p51_restart_transfers(fresh_cells);
    int64_t fresh_max_deadline_delta_ms = 0;
    const bool fresh_deadlines_respected = p51_restart_deadlines_respected(
        fresh_cells, kSourceDeadlineCleanupGrace,
        &fresh_max_deadline_delta_ms);
    std::fprintf(stderr,
        "P51_RESTART_DEADLINES topology=%s profile=%u phase=fresh callers=%zu max_completion_delta_ms=%lld grace_ms=%lld respected=%u\n",
        topology.c_str(), profile_mask, fresh_cells.size(),
        static_cast<long long>(fresh_max_deadline_delta_ms),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            kSourceDeadlineCleanupGrace).count()),
        fresh_deadlines_respected ? 1u : 0u);
    REQUIRE(fresh_deadlines_respected,
            "every fresh source caller completes by its original 30-second deadline plus 2-second cleanup grace");
    P51SourceArmedFields fresh_armed = fresh_cells.empty()
        ? P51SourceArmedFields{} : fresh_cells.front()->armed;
    size_t fresh_committed = 0;
    size_t fresh_attached_count = 0;
    for (auto& cell : fresh_cells) {
        const auto& result = cell->result;
        const bool committed = cell->settled && result.code ==
            icecc::p50::local::SourceTransferResultCode::Committed;
        fresh_committed += committed;
        if (committed && attach_p51_restart_job(
                refreshed_f, *cell->job, profile_mask, cell->armed, result))
            fresh_attached_count++;
    }
    if (!restart_chain_f_c) fresh_gate.reset();
    fresh_attached = fresh_prepared && fresh_settled &&
        fresh_committed == jobs_per_window &&
        fresh_attached_count == jobs_per_window;
    std::fprintf(stderr,
        "P51_RESTART_FRESH_WINDOW topology=%s profile=%u callers=%zu commits=%zu attached=%zu held_commits=%zu\n",
        topology.c_str(), profile_mask, fresh_cells.size(),
        fresh_committed, fresh_attached_count,
        fresh_window_observed ? static_cast<size_t>(jobs_per_window) : 0u);
    const auto& fresh_result = fresh_cells.empty()
        ? icecc::p50::local::P50SourceTransferResult{}
        : fresh_cells.front()->result;
    std::fprintf(stderr,
        "P51_RESTART_FRESH job=%u code=%u error=%u attempts=%u raw=%llu expected=%zu digest_match=%u f_guid_prefix=%02x%02x f_gen=%llu c_guid_prefix=%02x%02x tu=%llu\n",
        fresh_cells.empty() ? 0u : fresh_cells.front()->job->wire_id,
        static_cast<unsigned>(fresh_result.code),
        static_cast<unsigned>(fresh_result.error_code),
        static_cast<unsigned>(fresh_result.attempts),
        static_cast<unsigned long long>(fresh_result.raw_bytes),
        fresh_cells.empty() ? 0u : fresh_cells.front()->job->bytes.size(),
        !fresh_cells.empty() && fresh_result.raw_digest ==
            icecc::digest128(fresh_cells.front()->job->bytes) ? 1u : 0u,
        static_cast<unsigned>(fresh_armed.f_store_guid[0]),
        static_cast<unsigned>(fresh_armed.f_store_guid[1]),
        static_cast<unsigned long long>(fresh_armed.f_store_generation),
        static_cast<unsigned>(fresh_result.c_store_guid.bytes[0]),
        static_cast<unsigned>(fresh_result.c_store_guid.bytes[1]),
        static_cast<unsigned long long>(fresh_result.tu_seq));
    REQUIRE(fresh_attached,
            "fresh W30 assignments commit and exact CompileFile inputs attach on the replacement incarnation");
    const bool fresh_identity = restart_f
        ? old_armed.f_store_guid != fresh_armed.f_store_guid ||
              old_armed.f_store_generation != fresh_armed.f_store_generation
        : old_armed.arm.source.c_store_guid != fresh_armed.arm.source.c_store_guid ||
              old_armed.arm.source.c_store_generation != fresh_armed.arm.source.c_store_generation;
    REQUIRE(fresh_identity && fresh_attached,
            "replacement transfer uses a fresh F store identity or fresh C store identity");

    if (restart_chain_f_c && fresh_gate && !fresh_cells.empty() &&
        fresh_attached && fresh_identity) {
        const bool stale_old_f_rejected = stale_f_job && !fresh_cells.empty() &&
            reject_p51_stale_f_owner_attach(
                refreshed_f, *stale_f_job, stale_f_armed,
                fresh_cells.front()->armed, stale_f_witness, profile_mask);
        REQUIRE(stale_old_f_rejected,
                "a CompileFile carrying a held old-F commit cannot attach after F store replacement");

        // C2/F2 is independent of the C1/F1 relationship whose sidecar is
        // restarted below. Prove its progress while C1 is stopped in the
        // second leg as well as before the stop.
        const bool healthy_c_sibling_before = run_healthy(
            c_roles[1], f_roles[1], next_offset++, "before-C-restart");
        REQUIRE(healthy_c_sibling_before,
                "unaffected C2/F2 sibling is live before restarting C1's cache owner");

        const P51SourceArmedFields c_before = fresh_cells.front()->armed;
        auto *c_gate = fresh_gate.get();
        const uint64_t c_first_ordinal = fresh_last_ordinal == UINT64_MAX
            ? 0 : fresh_last_ordinal + 1;
        const bool c_gate_rearmed = c_gate && c_first_ordinal != 0 &&
            p51_gate_wait_rearm(*c_gate, jobs_per_window, c_first_ordinal,
                                work + "/c-reset-gate-abort");
        REQUIRE(c_gate_rearmed,
                "the live post-F receipt stream rearms for the next C1/F1 ordinal window");
        std::vector<std::unique_ptr<P51RestartTransferCell>> c_reset_cells;
        c_reset_cells.reserve(jobs_per_window);
        bool c_reset_prepared = c_gate_rearmed;
        for (unsigned index = 0; index < jobs_per_window && c_reset_prepared; ++index) {
            auto job = make_job(c_affected, refreshed_f, 600 + index);
            c_reset_prepared &= job != nullptr;
            if (!job) break;
            c_reset_cells.emplace_back(
                std::make_unique<P51RestartTransferCell>(std::move(job)));
        }
        REQUIRE(c_reset_prepared && c_reset_cells.size() == jobs_per_window,
                "same C1/F1 relationship admits a second exact W30 before C restart");
        const bool c_reset_started = c_reset_prepared &&
            start_p51_restart_transfers(c_reset_cells, refreshed_f, profile_mask);
        const bool c_window = c_reset_started && c_gate->wait_for_commits(
            std::chrono::seconds(30));
        const auto c_witnesses = c_window ? c_gate->commit_witnesses()
            : std::vector<icecc::p50::R2TxCommit>{};
        REQUIRE(c_window && c_witnesses.size() == jobs_per_window,
                "C-restart leg holds exact F-positive, C-unobserved W30 receipts");

        const pid_t c_parent = c_affected.daemon_pid;
        const pid_t c_sidecar_before = find_attachment_sidecar(c_parent, cache_service);
        const bool c_parent_stopped = c_parent > 1 &&
            ::kill(c_parent, SIGSTOP) == 0;
        if (c_parent_stopped) c_affected.paused = true;
        REQUIRE(c_sidecar_before > 1 && c_parent_stopped,
                "C leg identifies and stops only the affected C daemon before child replacement");
        const bool c_sidecar_killed = c_sidecar_before > 1 &&
            ::kill(c_sidecar_before, SIGKILL) == 0;
        REQUIRE(c_sidecar_killed,
                "C leg kills the exact old C cache-sidecar after held F commits");
        c_gate->discard_held_commits();

        const bool healthy_c_sibling_during = c_parent_stopped && run_healthy(
            c_roles[1], f_roles[1], next_offset++, "during-C-restart");
        REQUIRE(healthy_c_sibling_during,
                "unaffected C2/F2 completes and attaches while affected C1 parent remains stopped");
        const bool c_parent_resumed = c_parent_stopped &&
            ::kill(c_parent, SIGCONT) == 0;
        if (c_parent_resumed) c_affected.paused = false;
        const pid_t c_sidecar_after = c_parent_resumed
            ? c_affected.wait_for_restarted_cache_sidecar(
                cache_service, c_sidecar_before) : -1;
        REQUIRE(c_sidecar_after > 1,
                "C daemon publishes the replacement sidecar after the second chain leg");
        const bool c_old_settled = wait_p51_restart_transfers(
            c_reset_cells, Clock::now() + std::chrono::seconds(35));
        REQUIRE(c_old_settled,
                "C-owner callers settle within their original bounded deadlines after C restart");
        if (!c_old_settled) {
            for (P51RestartRole *role : all_roles())
                if (role->daemon_pid > 1) (void)::kill(role->daemon_pid, SIGKILL);
            (void)wait_p51_restart_transfers(
                c_reset_cells, Clock::now() + std::chrono::seconds(5));
        }
        join_p51_restart_transfers(c_reset_cells);
        int64_t c_old_max_deadline_delta_ms = 0;
        const bool c_old_deadlines_respected = p51_restart_deadlines_respected(
            c_reset_cells, kSourceDeadlineCleanupGrace,
            &c_old_max_deadline_delta_ms);
        REQUIRE(c_old_deadlines_respected,
                "C-restart callers remain bounded by their original absolute source deadlines");
        size_t c_old_positive_results = 0;
        for (const auto& cell : c_reset_cells) {
            if (cell->settled && cell->result.code ==
                    icecc::p50::local::SourceTransferResultCode::Committed) {
                ++c_old_positive_results;
                REQUIRE(cell->result.raw_bytes == cell->job->bytes.size() &&
                            cell->result.raw_digest == icecc::digest128(cell->job->bytes) &&
                            cell->result.tu_seq != UINT64_MAX &&
                            cell->result.c_store_guid.bytes ==
                                cell->armed.arm.source.c_store_guid,
                        "any C-restart caller that remains positive preserves its exact digest, TU, and original C identity");
            }
        }
        size_t old_c_input_attachments = 0;
        size_t old_c_input_witnesses = 0;
        for (const auto& witness : c_witnesses) {
            const auto cell = std::find_if(c_reset_cells.begin(), c_reset_cells.end(),
                [&](const auto& candidate) {
                    return candidate->job &&
                        icecc::digest128(candidate->job->bytes) == witness.inner.raw_digest;
                });
            if (cell == c_reset_cells.end() || witness.inner.tu_seq.value == UINT64_MAX)
                continue;
            icecc::p50::local::P50SourceTransferResult retained;
            retained.code = icecc::p50::local::SourceTransferResultCode::Committed;
            retained.attempts = 1;
            retained.tu_seq = witness.inner.tu_seq.value;
            retained.raw_bytes = (*cell)->job->bytes.size();
            retained.raw_digest = witness.inner.raw_digest;
            retained.c_store_guid.bytes = (*cell)->armed.arm.source.c_store_guid;
            if (retained.valid()) {
                ++old_c_input_witnesses;
                if (attach_p51_restart_job(
                        refreshed_f, *(*cell)->job, profile_mask,
                        (*cell)->armed, retained))
                    ++old_c_input_attachments;
            }
        }
        REQUIRE(old_c_input_witnesses == jobs_per_window &&
                    old_c_input_attachments == jobs_per_window,
                "C-sidecar replacement preserves exact attachment of every live F-positive old-C input");
        fresh_gate.reset();
        const bool identities_cycled = !c_reset_cells.empty() &&
            !fresh_cells.empty() && c_sidecar_after != c_sidecar_before &&
            c_parent == c_affected.daemon_pid &&
            refreshed_f.daemon_pid == f_affected.daemon_pid;
        REQUIRE(identities_cycled,
                "C sidecar identity changes while C/F daemon processes and F owner survive");

        auto post_c_gate = std::make_unique<P51CommitReceiptGate>(
            refreshed_f.endpoint_port, sidecar_uid, jobs_per_window);
        REQUIRE(post_c_gate->ready(),
                "post-C receipt gate protects the fresh C-owner W30 window");
        std::vector<std::unique_ptr<P51RestartTransferCell>> post_c_cells;
        post_c_cells.reserve(jobs_per_window);
        bool post_c_prepared = post_c_gate->ready();
        for (unsigned index = 0; index < jobs_per_window && post_c_prepared; ++index) {
            auto job = make_job(c_affected, refreshed_f, 700 + index);
            post_c_prepared &= job != nullptr;
            if (!job) break;
            post_c_cells.emplace_back(
                std::make_unique<P51RestartTransferCell>(std::move(job)));
        }
        REQUIRE(post_c_prepared && post_c_cells.size() == jobs_per_window,
                "fresh C identity admits a complete post-chain W30 assignment set");
        const bool post_c_started = post_c_prepared && start_p51_restart_transfers(
            post_c_cells, refreshed_f, profile_mask);
        const bool post_c_held = post_c_started && post_c_gate->wait_for_commits(
            std::chrono::seconds(30));
        REQUIRE(post_c_held && post_c_gate->observed_commits() == jobs_per_window,
                "post-chain F emits the exact fresh W30 commit window");
        if (post_c_held) post_c_gate->release_commits();
        else post_c_gate->discard_held_commits();
        const bool post_c_settled = post_c_started && wait_p51_restart_transfers(
            post_c_cells, Clock::now() + std::chrono::seconds(35));
        REQUIRE(post_c_settled,
                "post-C fresh W30 callers settle before the watchdog");
        if (!post_c_settled)
            for (P51RestartRole *role : all_roles())
                if (role->daemon_pid > 1) (void)::kill(role->daemon_pid, SIGKILL);
        join_p51_restart_transfers(post_c_cells);
        int64_t post_c_max_deadline_delta_ms = 0;
        const bool post_c_deadlines_respected = p51_restart_deadlines_respected(
            post_c_cells, kSourceDeadlineCleanupGrace,
            &post_c_max_deadline_delta_ms);
        REQUIRE(post_c_deadlines_respected,
                "fresh post-C callers remain bounded by their original absolute source deadlines");
        size_t post_c_committed = 0;
        size_t post_c_attached = 0;
        for (auto& cell : post_c_cells) {
            if (!cell->settled || cell->result.code !=
                    icecc::p50::local::SourceTransferResultCode::Committed)
                continue;
            ++post_c_committed;
            post_c_attached += attach_p51_restart_job(
                refreshed_f, *cell->job, profile_mask, cell->armed, cell->result);
        }
        const bool c_rotated_f_preserved = !post_c_cells.empty() &&
            c_before.arm.source.c_store_guid !=
                post_c_cells.front()->armed.arm.source.c_store_guid &&
            c_before.arm.source.c_store_generation !=
                post_c_cells.front()->armed.arm.source.c_store_generation &&
            c_before.f_store_guid == post_c_cells.front()->armed.f_store_guid &&
            c_before.f_store_generation == post_c_cells.front()->armed.f_store_generation;
        REQUIRE(c_rotated_f_preserved,
                "post-C assignments use the new C identity while retaining the restarted F identity");
        REQUIRE(post_c_committed == jobs_per_window &&
                    post_c_attached == jobs_per_window,
                "all fresh post-chain C/F jobs attach exact committed source bytes");
        std::fprintf(stderr,
            "P51_PROCESS_RESTART_CHAIN_F_C profile=%u f_old_assignment_rejected=%u c_parent_stopped=%u healthy_c2_f2=%u c_f_commits=%zu c_old_caller_committed=%zu/%zu c_old_attach=%zu c_old_deadline_ms=%lld post_c=%zu/%zu post_c_deadline_ms=%lld c_rotated_f_preserved=%u\n",
            profile_mask, stale_old_f_rejected ? 1u : 0u,
            c_parent_stopped ? 1u : 0u, healthy_c_sibling_during ? 1u : 0u,
            c_witnesses.size(), c_old_positive_results,
            static_cast<size_t>(jobs_per_window), old_c_input_attachments,
            static_cast<long long>(c_old_max_deadline_delta_ms),
            post_c_committed, post_c_attached,
            static_cast<long long>(post_c_max_deadline_delta_ms),
            c_rotated_f_preserved ? 1u : 0u);
        post_c_gate.reset();
        post_c_cells.clear();
        c_reset_cells.clear();
    } else if (restart_chain_f_c) {
        REQUIRE(false,
                "the ordered C-restart leg requires a settled fresh F identity and live gate");
    }

    healthy_initial_jobs.clear(); fresh_cells.clear();
    stop_roles();
    std::fprintf(stderr, "retained process-restart work directory: %s\n", work.c_str());
    std::fprintf(stderr, "P51_PROCESS_RESTART%s topology=%s affected=%s profile=%u jobs=%u fresh_attached=%u healthy_attached=%u healthy_siblings=%u/%u target_parent_stopped=%u\n",
        jobs_per_window == 30 ? "_W30" : "",
        topology.c_str(), restart_f ? "F-cache" : "C-cache",
        profile_mask, jobs_per_window, fresh_attached ? 1u : 0u,
        healthy_during ? 1u : 0u, healthy_during_count,
        restart_f ? f_count - 1 : c_count - 1,
        healthy_parent_stopped ? 1u : 0u);
    REQUIRE(healthy_during_count == (restart_f ? f_count - 1 : c_count - 1),
            "healthy progress was observed for every unaffected topology sibling");
    return failures ? 1 : 0;
}

static bool reject_p51_stale_epoch_attach(
    P51RestartRole& f, const P51RestartJob& old_job,
    const P51SourceArmedFields& old_armed,
    const icecc::p50::R2TxCommit& witness, uint32_t profile_mask)
{
    const std::string marker = "P50_INPUT_ATTACH_BEGIN job=" +
        std::to_string(old_job.wire_id) + " epoch=" +
        std::to_string(old_job.epoch) + " nonce=" +
        std::to_string(old_job.nonce);
    std::error_code error;
    const uintmax_t offset = std::filesystem::file_size(f.log, error);
    if (error) return false;
    std::unique_ptr<MsgChannel> stale(connect_tcp_bounded(f.endpoint_port, 5000));
    if (!stale) return false;

    CompileJob compile_job = attachment_compile_job(
        old_job.wire_id, old_job.epoch, old_job.nonce,
        old_armed.arm.source, nullptr);
    CompileInputIdentity identity;
    identity.profile = profile_mask == CACHE_PROFILE_P29V1
        ? CompileInputIdentity::P29V1Profile
        : profile_mask == CACHE_PROFILE_ZSTD_ROUTE
            ? CompileInputIdentity::ZstdRouteProfile
            : CompileInputIdentity::ZstdTuProfile;
    identity.c_store_guid = old_armed.arm.source.c_store_guid;
    identity.tu_seq = witness.inner.tu_seq.value;
    identity.raw_bytes = old_job.bytes.size();
    identity.raw_digest = witness.inner.raw_digest.bytes;
    identity.attempt_id = old_job.nonce;
    identity.request_id = old_job.nonce;
    compile_job.setCompileInputIdentity(identity);
    if (!stale->send_msg(CompileFileMsg(&compile_job))) return false;
    const bool closed = wait_eof(stale.get(), 5000);
    const std::string suffix = read_file_suffix(f.log, offset);
    const bool attach_started = suffix.find(marker) != std::string::npos;
    const std::string rejected = "rejecting unprepared/revoked assignment claim " +
        std::to_string(old_job.wire_id);
    const bool epoch_assignment_rejected = suffix.find(rejected) != std::string::npos;
    std::fprintf(stderr,
        "P51_SYNTH_SCHEDULER_STALE_ATTACH job=%u old_epoch=%llu new_epoch=%llu closed=%u attach_started=%u assignment_reject=%u witness_tu=%llu\n",
        old_job.wire_id, static_cast<unsigned long long>(old_job.epoch),
        static_cast<unsigned long long>(f.scheduler_epoch), closed ? 1u : 0u,
        attach_started ? 1u : 0u, epoch_assignment_rejected ? 1u : 0u,
        static_cast<unsigned long long>(witness.inner.tu_seq.value));
    return closed && !attach_started && epoch_assignment_rejected;
}

static bool reject_p51_stale_f_owner_attach(
    P51RestartRole& f, const P51RestartJob& old_job,
    const P51SourceArmedFields& old_armed,
    const P51SourceArmedFields& current_armed,
    const icecc::p50::R2TxCommit& witness, uint32_t profile_mask)
{
    const std::string marker = "P50_INPUT_ATTACH_BEGIN job=" +
        std::to_string(old_job.wire_id) + " epoch=" +
        std::to_string(old_job.epoch) + " nonce=" +
        std::to_string(old_job.nonce);
    std::error_code error;
    const uintmax_t offset = std::filesystem::file_size(f.log, error);
    if (error) return false;
    std::unique_ptr<MsgChannel> stale(connect_tcp_bounded(f.endpoint_port, 5000));
    if (!stale) return false;

    CompileJob compile_job = attachment_compile_job(
        old_job.wire_id, old_job.epoch, old_job.nonce,
        old_armed.arm.source, nullptr);
    CompileInputIdentity identity;
    identity.profile = profile_mask == CACHE_PROFILE_P29V1
        ? CompileInputIdentity::P29V1Profile
        : profile_mask == CACHE_PROFILE_ZSTD_ROUTE
            ? CompileInputIdentity::ZstdRouteProfile
            : CompileInputIdentity::ZstdTuProfile;
    identity.c_store_guid = old_armed.arm.source.c_store_guid;
    identity.tu_seq = witness.inner.tu_seq.value;
    identity.raw_bytes = old_job.bytes.size();
    identity.raw_digest = witness.inner.raw_digest.bytes;
    identity.attempt_id = old_job.nonce;
    identity.request_id = old_job.nonce;
    compile_job.setCompileInputIdentity(identity);
    if (!stale->send_msg(CompileFileMsg(&compile_job))) return false;
    const bool closed = wait_eof(stale.get(), 5000);
    const std::string suffix = read_file_suffix(f.log, offset);
    const bool old_attach_started = suffix.find(marker) != std::string::npos;
    const std::string owner_rejected =
        "P50 CompileFile did not match one live source owner for job " +
        std::to_string(old_job.wire_id) + "\n";
    const std::string revoked_assignment =
        "rejecting unprepared/revoked assignment claim " +
        std::to_string(old_job.wire_id) + "\n";
    const bool retired_old_assignment_rejected =
        suffix.find(owner_rejected) != std::string::npos ||
        suffix.find(revoked_assignment) != std::string::npos;
    const std::string attach_end = "P50_INPUT_ATTACH_END job=" +
        std::to_string(old_job.wire_id) + " epoch=" +
        std::to_string(old_job.epoch) + " nonce=" +
        std::to_string(old_job.nonce) + " request=" +
        std::to_string(old_job.nonce) + " elapsed_ms=";
    const size_t attach_end_at = suffix.find(attach_end);
    const size_t attach_end_line_end = attach_end_at == std::string::npos
        ? attach_end_at : suffix.find('\n', attach_end_at);
    const std::string_view attach_end_line =
        attach_end_at == std::string::npos ? std::string_view{} :
        std::string_view(suffix.data() + attach_end_at,
            (attach_end_line_end == std::string::npos ? suffix.size() :
             attach_end_line_end) - attach_end_at);
    const bool any_successful_attach =
        attach_end_line.find(" status=0") != std::string_view::npos;
    std::fprintf(stderr,
        "P51_STALE_F_ASSIGNMENT_ATTACH job=%u old_f_gen=%llu new_f_gen=%llu closed=%u attach_started=%u retired_assignment_rejected=%u success=%u witness_tu=%llu\n",
        old_job.wire_id,
        static_cast<unsigned long long>(old_armed.f_store_generation),
        static_cast<unsigned long long>(current_armed.f_store_generation),
        closed ? 1u : 0u,
        old_attach_started ? 1u : 0u, retired_old_assignment_rejected ? 1u : 0u,
        any_successful_attach ? 1u : 0u,
        static_cast<unsigned long long>(witness.inner.tu_seq.value));
    return closed && !old_attach_started && retired_old_assignment_rejected &&
        !any_successful_attach;
}

static int run_p51_synthetic_scheduler_epoch_w30(
    const char *daemon_binary, const char *cache_service, passwd *icecc,
    uint32_t profile_mask)
{
    const char *temporary_root = ::getenv("TMPDIR");
    const std::string prefix = temporary_root && *temporary_root ? temporary_root : "/tmp";
    std::string pattern = prefix + "/p51s.XXXXXX";
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    char *created = ::mkdtemp(mutable_pattern.data());
    REQUIRE(created != nullptr, "synthetic scheduler fixture root created");
    if (!created) return 2;
    const std::string work(created);
    REQUIRE(::chown(work.c_str(), icecc->pw_uid, icecc->pw_gid) == 0 &&
                ::chmod(work.c_str(), 0700) == 0,
            "synthetic scheduler fixture root has sidecar ownership");
    if (failures) return 2;

    constexpr uint64_t kOldEpoch = UINT64_C(0x51e0000000000001);
    constexpr uint64_t kNewEpoch = UINT64_C(0x51e0000000000002);
    constexpr uint32_t kWireBase = 0x51e21000;
    P51RestartRole affected_c{"synthetic-c1", work + "/c1", "", -1, 0, 0,
                              -1, nullptr, false, 0};
    P51RestartRole healthy_c{"synthetic-c2", work + "/c2", "", -1, 0, 0,
                             -1, nullptr, false, 0};
    P51RestartRole affected_f{"synthetic-f1", work + "/f1", "", -1, 0, 0,
                              -1, nullptr, false, 0};
    P51RestartRole healthy_f{"synthetic-f2", work + "/f2", "", -1, 0, 0,
                             -1, nullptr, false, 0};
    for (P51RestartRole *role : {&affected_c, &healthy_c, &affected_f, &healthy_f}) {
        const bool made = ::mkdir(role->directory.c_str(), 0700) == 0 &&
            ::chown(role->directory.c_str(), icecc->pw_uid, icecc->pw_gid) == 0 &&
            ::mkdir((role->directory + "/envs").c_str(), 0700) == 0 &&
            ::chown((role->directory + "/envs").c_str(), icecc->pw_uid, icecc->pw_gid) == 0 &&
            ::mkdir((role->directory + "/runtime").c_str(), 0700) == 0 &&
            ::chown((role->directory + "/runtime").c_str(), icecc->pw_uid, icecc->pw_gid) == 0;
        REQUIRE(made, "all synthetic scheduler roles have private runtime directories");
        role->log = role->directory + "/iceccd.log";
    }
    if (failures) return 2;

    const bool roles_ready = affected_c.start(daemon_binary, cache_service, kOldEpoch, 64) &&
        affected_f.start(daemon_binary, cache_service, kOldEpoch, 64) &&
        healthy_c.start(daemon_binary, cache_service, kOldEpoch, 64) &&
        healthy_f.start(daemon_binary, cache_service, kOldEpoch, 64);
    REQUIRE(roles_ready,
        "four independent daemon/cache roles start under the original scheduler epoch");
    if (!roles_ready) {
        affected_c.stop(); affected_f.stop(); healthy_c.stop(); healthy_f.stop();
        std::fprintf(stderr, "retained synthetic scheduler work directory: %s\n", work.c_str());
        return 1;
    }
    const pid_t c_pid_before = affected_c.daemon_pid;
    const pid_t f_pid_before = affected_f.daemon_pid;
    const int old_c_port = affected_c.endpoint_port;
    const int old_f_port = affected_f.endpoint_port;
    const uint32_t sidecar_uid = icecc->pw_uid;
    auto make_job = [&](P51RestartRole& c, P51RestartRole& f,
                        uint32_t offset, uint64_t epoch) {
        const uint32_t wire = kWireBase + offset;
        const uint64_t nonce = (static_cast<uint64_t>(wire) << 32) | offset;
        return prepare_p51_restart_job(c, f, work, wire, nonce, epoch, profile_mask);
    };

    auto healthy_initial = make_job(healthy_c, healthy_f, 1, kOldEpoch);
    P51SourceArmedFields healthy_initial_armed{};
    const auto healthy_initial_result = healthy_initial
        ? run_p51_restart_job(healthy_f, *healthy_initial, profile_mask,
                              &healthy_initial_armed)
        : icecc::p50::local::P50SourceTransferResult{};
    const bool healthy_before = healthy_initial && attach_p51_restart_job(
        healthy_f, *healthy_initial, profile_mask, healthy_initial_armed,
        healthy_initial_result);
    REQUIRE(healthy_before,
        "independent C2/F2 scheduler pair is established before epoch replacement");
    if (!healthy_before) {
        healthy_initial.reset();
        affected_c.stop(); affected_f.stop(); healthy_c.stop(); healthy_f.stop();
        std::fprintf(stderr, "retained synthetic scheduler work directory: %s\n", work.c_str());
        return 1;
    }

    auto old_gate = std::make_unique<P51CommitReceiptGate>(
        affected_f.endpoint_port, sidecar_uid, 30);
    REQUIRE(old_gate->ready(), "synthetic epoch gate can hold the old W30 receipts");
    std::vector<std::unique_ptr<P51RestartTransferCell>> old_cells;
    old_cells.reserve(30);
    bool old_prepared = old_gate->ready();
    for (unsigned index = 0; index < 30 && old_prepared; ++index) {
        auto job = make_job(affected_c, affected_f, 2 + index, kOldEpoch);
        old_prepared &= job != nullptr;
        if (!job) break;
        old_cells.emplace_back(std::make_unique<P51RestartTransferCell>(std::move(job)));
    }
    REQUIRE(old_prepared && old_cells.size() == 30,
        "old scheduler epoch admits 30 distinct affected compiler assignments");
    const bool old_started = old_prepared && start_p51_restart_transfers(
        old_cells, affected_f, profile_mask);
    REQUIRE(old_started, "old epoch enters a complete pending W30 source window");
    const bool old_window = old_started && old_gate->wait_for_commits(std::chrono::seconds(30));
    REQUIRE(old_window && old_gate->observed_commits() == 30,
        "old epoch reaches 30 F-committed but C-unobserved source receipts");
    const auto old_witnesses = old_window ? old_gate->commit_witnesses()
                                          : std::vector<icecc::p50::R2TxCommit>{};
    const bool old_witnesses_complete = old_witnesses.size() == 30;
    REQUIRE(old_witnesses_complete,
        "held old-epoch receipts expose the complete exact commit witness set");
    if (!old_window || !old_witnesses_complete) {
        old_gate->discard_held_commits(); old_gate.reset();
        affected_c.stop(); affected_f.stop(); healthy_c.stop(); healthy_f.stop();
        (void)wait_p51_restart_transfers(old_cells, Clock::now() + std::chrono::seconds(5));
        join_p51_restart_transfers(old_cells);
        std::fprintf(stderr, "retained synthetic scheduler work directory: %s\n", work.c_str());
        return 1;
    }
    const auto& old_witness = old_witnesses.front();
    // Drop the two affected scheduler sessions but keep both daemon/cache
    // processes alive. Their reconnects are accepted later under epoch S'.
    delete affected_c.scheduler; affected_c.scheduler = nullptr;
    delete affected_f.scheduler; affected_f.scheduler = nullptr;
    old_gate->release_commits();

    auto healthy_during_job = make_job(healthy_c, healthy_f, 33, kOldEpoch);
    P51SourceArmedFields healthy_during_armed{};
    const auto healthy_during_result = healthy_during_job
        ? run_p51_restart_job(healthy_f, *healthy_during_job, profile_mask,
                              &healthy_during_armed)
        : icecc::p50::local::P50SourceTransferResult{};
    const bool healthy_during = healthy_during_job && attach_p51_restart_job(
        healthy_f, *healthy_during_job, profile_mask, healthy_during_armed,
        healthy_during_result);
    REQUIRE(healthy_during,
        "unaffected independent scheduler pair transfers and attaches while affected sessions are down");

    const bool c_reconnected = affected_c.replace_scheduler_epoch(kNewEpoch);
    const bool f_reconnected = affected_f.replace_scheduler_epoch(kNewEpoch);
    REQUIRE(c_reconnected && f_reconnected && affected_c.scheduler_epoch == kNewEpoch &&
                affected_f.scheduler_epoch == kNewEpoch,
        "same C/F daemons reconnect to synthetic scheduler peers under a new assignment epoch");
    const bool old_settled = wait_p51_restart_transfers(
        old_cells, Clock::now() + std::chrono::seconds(20));
    REQUIRE(old_settled, "all old-epoch W30 original callers settle after scheduler loss");
    if (!old_settled) {
        affected_c.stop(); affected_f.stop(); healthy_c.stop(); healthy_f.stop();
        (void)wait_p51_restart_transfers(
            old_cells, Clock::now() + std::chrono::seconds(5));
        join_p51_restart_transfers(old_cells);
        std::fprintf(stderr, "retained synthetic scheduler work directory: %s\n", work.c_str());
        return 1;
    }
    join_p51_restart_transfers(old_cells);
    const P51SourceArmedFields old_armed = old_cells.front()->armed;
    const auto old_witness_cell = std::find_if(old_cells.begin(), old_cells.end(),
        [&](const auto& cell) {
            return icecc::digest128(cell->job->bytes) == old_witness.inner.raw_digest;
        });
    const bool witness_matches_job = old_witness_cell != old_cells.end();
    REQUIRE(witness_matches_job,
        "held receipt witness maps to the exact original compiler input");
    const P51SourceArmedFields witness_armed = witness_matches_job
        ? (*old_witness_cell)->armed : old_armed;
    const P51RestartJob& witness_job = witness_matches_job
        ? *(*old_witness_cell)->job : *old_cells.front()->job;
    const bool daemon_identity_stable = affected_c.daemon_pid == c_pid_before &&
        affected_f.daemon_pid == f_pid_before && affected_c.endpoint_port == old_c_port &&
        affected_f.endpoint_port == old_f_port;
    REQUIRE(daemon_identity_stable,
        "scheduler epoch replacement leaves daemon processes and public endpoints unchanged");

    int64_t old_delta_ms = 0;
    const bool old_deadlines = p51_restart_deadlines_respected(
        old_cells, std::chrono::seconds(2), &old_delta_ms);
    REQUIRE(old_deadlines,
        "old scheduler-epoch callers complete within their original absolute deadlines");
    size_t old_exact_receipts = 0;
    for (const auto& cell : old_cells) {
        if (!cell->settled || cell->result.code !=
                icecc::p50::local::SourceTransferResultCode::Committed ||
            cell->result.raw_bytes != cell->job->bytes.size() ||
            cell->result.raw_digest != icecc::digest128(cell->job->bytes) ||
            cell->result.c_store_guid.bytes != cell->armed.arm.source.c_store_guid)
            continue;
        const bool receipt_seen = std::any_of(
            old_witnesses.begin(), old_witnesses.end(), [&](const auto& witness) {
                return witness.inner.tu_seq.value == cell->result.tu_seq &&
                       witness.inner.raw_digest == cell->result.raw_digest;
            });
        old_exact_receipts += receipt_seen;
    }
    REQUIRE(old_exact_receipts == 30,
        "every old result is an exact match for a held F commit witness, never an invented receipt");
    std::fprintf(stderr,
        "P51_SYNTH_SCHEDULER_OLD_WINDOW profile=%u held=30 exact_results=%zu max_deadline_delta_ms=%lld\n",
        profile_mask, old_exact_receipts, static_cast<long long>(old_delta_ms));

    const bool stale_rejected = c_reconnected && f_reconnected &&
        witness_matches_job &&
        reject_p51_stale_epoch_attach(affected_f, witness_job,
                                      witness_armed, old_witness, profile_mask);
    REQUIRE(stale_rejected,
        "explicit old-epoch CompileFile attachment is rejected after the new scheduler epoch");

    const uint64_t last_old_ordinal = old_witnesses.empty() ? 0 :
        std::max_element(old_witnesses.begin(), old_witnesses.end(),
            [](const auto& left, const auto& right) {
                return left.relationship_ordinal < right.relationship_ordinal;
            })->relationship_ordinal;
    const bool fresh_ordinal_available = last_old_ordinal != UINT64_MAX;
    REQUIRE(fresh_ordinal_available,
        "same-store scheduler replacement retains a non-exhausted relationship ordinal");
    const bool fresh_gate_ready = fresh_ordinal_available &&
        old_gate->rearm(30, last_old_ordinal + 1);
    REQUIRE(fresh_gate_ready,
        "the same persistent-link receipt gate rearms for the new ordinal interval");
    std::vector<std::unique_ptr<P51RestartTransferCell>> fresh_cells;
    fresh_cells.reserve(30);
    bool fresh_prepared = fresh_gate_ready;
    for (unsigned index = 0; index < 30 && fresh_prepared; ++index) {
        auto job = make_job(affected_c, affected_f, 34 + index, kNewEpoch);
        fresh_prepared &= job != nullptr;
        if (!job) break;
        fresh_cells.emplace_back(std::make_unique<P51RestartTransferCell>(std::move(job)));
    }
    REQUIRE(fresh_prepared && fresh_cells.size() == 30,
        "new scheduler epoch admits a fresh set of 30 compiler assignments");
    const bool fresh_started = fresh_prepared && start_p51_restart_transfers(
        fresh_cells, affected_f, profile_mask);
    const bool fresh_window = fresh_started && old_gate->wait_for_commits(
        std::chrono::seconds(30));
    REQUIRE(fresh_window && old_gate->observed_commits() == 30,
        "new scheduler epoch reaches a fresh 30-commit W30 window before receipt release");
    size_t fresh_pending = 0;
    if (fresh_window) {
        for (const auto& cell : fresh_cells)
            fresh_pending += cell->future.wait_for(std::chrono::milliseconds(0)) !=
                std::future_status::ready;
    }
    REQUIRE(fresh_pending == 30,
        "fresh new-epoch callers remain pending until their own receipt gate opens");
    if (fresh_window) old_gate->release_commits();
    else old_gate->discard_held_commits();
    const bool fresh_settled = fresh_started && wait_p51_restart_transfers(
        fresh_cells, Clock::now() + std::chrono::seconds(35));
    REQUIRE(fresh_settled, "fresh epoch callers settle within the bounded fixture watchdog");
    if (!fresh_settled) {
        affected_c.stop(); affected_f.stop(); healthy_c.stop(); healthy_f.stop();
        (void)wait_p51_restart_transfers(
            fresh_cells, Clock::now() + std::chrono::seconds(5));
    }
    join_p51_restart_transfers(fresh_cells);
    int64_t fresh_delta_ms = 0;
    const bool fresh_deadlines = p51_restart_deadlines_respected(
        fresh_cells, std::chrono::seconds(2), &fresh_delta_ms);
    REQUIRE(fresh_deadlines,
        "fresh scheduler-epoch callers complete within their original absolute deadlines");
    size_t fresh_committed = 0;
    size_t fresh_attached = 0;
    for (const auto& cell : fresh_cells) {
        if (cell->settled && cell->result.code ==
                icecc::p50::local::SourceTransferResultCode::Committed) {
            ++fresh_committed;
            fresh_attached += attach_p51_restart_job(
                affected_f, *cell->job, profile_mask, cell->armed, cell->result);
        }
    }
    REQUIRE(fresh_committed == 30 && fresh_attached == 30,
        "fresh scheduler-epoch W30 inputs commit and attach exact bytes for all 30 jobs");
    const bool fresh_identity_present = !fresh_cells.empty();
    const bool cache_identities_unchanged = fresh_identity_present &&
        old_armed.f_store_guid == fresh_cells.front()->armed.f_store_guid &&
        old_armed.f_store_generation == fresh_cells.front()->armed.f_store_generation &&
        old_armed.arm.source.c_store_guid ==
            fresh_cells.front()->armed.arm.source.c_store_guid &&
        old_armed.arm.source.c_store_generation ==
            fresh_cells.front()->armed.arm.source.c_store_generation;
    REQUIRE(cache_identities_unchanged,
        "synthetic scheduler replacement changes epoch but not either cache-store identity");
    if (failures == 0)
        std::fprintf(stderr,
            "P51_SYNTH_SCHEDULER_EPOCH_W30_PASS profile=%u old=30/exact-results fresh=%zu/committed/%zu/attached healthy_sibling=%u daemon_identity_stable=%u old_deadline_delta_ms=%lld fresh_deadline_delta_ms=%lld old_epoch=%llu new_epoch=%llu\n",
            profile_mask, fresh_committed, fresh_attached, healthy_during ? 1u : 0u,
            daemon_identity_stable ? 1u : 0u, static_cast<long long>(old_delta_ms),
            static_cast<long long>(fresh_delta_ms),
            static_cast<unsigned long long>(kOldEpoch),
            static_cast<unsigned long long>(kNewEpoch));

    old_gate.reset();
    healthy_initial.reset(); healthy_during_job.reset();
    old_cells.clear(); fresh_cells.clear();
    affected_c.stop(); affected_f.stop(); healthy_c.stop(); healthy_f.stop();
    if (failures == 0) std::filesystem::remove_all(work);
    else std::fprintf(stderr, "retained synthetic scheduler work directory: %s\n", work.c_str());
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

static uint32_t source_mode_for_profile(uint32_t profile_mask)
{
    return profile_mask == CACHE_PROFILE_P29V1 ? P50_SOURCE_MODE_P29V1
        : profile_mask == CACHE_PROFILE_ZSTD_ROUTE ? P50_SOURCE_MODE_ZSTD_ROUTE
        : P50_SOURCE_MODE_ZSTD_TU;
}

static constexpr uint32_t kExpiredArmWireJob = 0x7a710701;
static constexpr uint64_t kExpiredArmWireNonce = UINT64_C(0x7a71070100000001);
static constexpr uint32_t kFreshArmWireJob = 0x7a710702;
static constexpr uint64_t kFreshArmWireNonce = UINT64_C(0x7a71070200000001);
static constexpr uint64_t kExpiredArmWireBudgetMsec = 2000;

int main(int argc, char **argv)
{
    const bool receipt_gate_mode = argc == 7 &&
        (std::strcmp(argv[1], "--p51-commit-receipt-gate") == 0 ||
         std::strcmp(argv[1], "--p51-commit-receipt-gate-once") == 0);
    const bool receipt_gate_once = receipt_gate_mode &&
        std::strcmp(argv[1], "--p51-commit-receipt-gate-once") == 0;
    const bool p51_expired_arm_wire_case =
        ::getenv("ICECC_TEST_P51_EXPIRED_ARM_WIRE") != nullptr;
    const bool p51_cancel_before_start =
        ::getenv("ICECC_TEST_P51_CANCEL_BEFORE_START") != nullptr;
    const bool p51_cancel_after_deadline =
        ::getenv("ICECC_TEST_P51_CANCEL_AFTER_DEADLINE") != nullptr;
    const bool p51_cancel_retained_committed =
        ::getenv("ICECC_TEST_P51_CANCEL_RETAINED_COMMITTED") != nullptr;
    const unsigned p51_cancel_scenarios =
        static_cast<unsigned>(p51_cancel_before_start) +
        static_cast<unsigned>(p51_cancel_after_deadline) +
        static_cast<unsigned>(p51_cancel_retained_committed);
    const P51CancelScenario p51_cancel_scenario = p51_cancel_before_start
        ? P51CancelScenario::BeforePublication
        : p51_cancel_after_deadline ? P51CancelScenario::AfterSourceDeadline
        : p51_cancel_retained_committed ? P51CancelScenario::RetainedCommitted
                                        : P51CancelScenario::None;
    const char *other_p51_modes[] = {
        "ICECC_TEST_P51_CANCEL_REPLACEMENT",
        "ICECC_TEST_P51_MULTILINK",
        "ICECC_TEST_P51_VERTICAL",
        "ICECC_TEST_P51_VERTICAL_W30",
        "ICECC_TEST_P51_RESTART_F_C1F2",
        "ICECC_TEST_P51_RESTART_C_C2F1",
        "ICECC_TEST_P51_RESTART_W30_F_C1F2",
        "ICECC_TEST_P51_RESTART_W30_C_C2F1",
        "ICECC_TEST_P51_RESTART_W30_TOPOLOGY",
        "ICECC_TEST_P51_SYNTH_SCHEDULER_W30",
        "ICECC_TEST_P51_LOST_RECEIPTS",
        "ICECC_TEST_P51_RESTART_CHAIN_F_C_W30",
    };
    bool conflicting_p51_mode = false;
    for (const char *name : other_p51_modes)
        conflicting_p51_mode = conflicting_p51_mode ||
            ::getenv(name) != nullptr;
    if (p51_cancel_scenarios > 1) {
        std::fprintf(stderr,
            "FAIL: select one P51 exact-cancellation scenario\n");
        return 2;
    }
    if (receipt_gate_mode && p51_cancel_scenario != P51CancelScenario::None) {
        std::fprintf(stderr,
            "FAIL: receipt gate cannot be combined with a P51 cancellation scenario\n");
        return 2;
    }
    if (p51_expired_arm_wire_case &&
        (receipt_gate_mode || conflicting_p51_mode ||
         p51_cancel_scenario != P51CancelScenario::None)) {
        std::fprintf(stderr,
            "FAIL: P51 expired-ARM wire gate cannot be combined with another mode\n");
        return 2;
    }
    if (!receipt_gate_mode && argc != 3) {
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
    if (receipt_gate_mode) {
        char *end = nullptr;
        errno = 0;
        const long endpoint_port = std::strtol(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' ||
            endpoint_port <= 0 || endpoint_port > 65535)
            return 2;
        errno = 0;
        const unsigned long sidecar_uid = std::strtoul(argv[3], &end, 10);
        if (errno != 0 || end == argv[3] || *end != '\0' ||
            sidecar_uid != static_cast<unsigned long>(icecc->pw_uid))
            return 2;
        errno = 0;
        const unsigned long expected = std::strtoul(argv[4], &end, 10);
        if (errno != 0 || end == argv[4] || *end != '\0' || expected != 30)
            return 2;
        errno = 0;
        const unsigned long long first = std::strtoull(argv[5], &end, 10);
        if (errno != 0 || end == argv[5] || *end != '\0') return 2;
        return run_p51_commit_receipt_gate(
            static_cast<int>(endpoint_port), static_cast<uid_t>(sidecar_uid),
            static_cast<size_t>(expected), static_cast<uint64_t>(first), argv[6],
            receipt_gate_once);
    }

    if (p51_cancel_scenario != P51CancelScenario::None) {
        if (conflicting_p51_mode) {
            std::fprintf(stderr,
                "FAIL: select only one P51 positive-daemon test mode\n");
            return 2;
        }
        const uint32_t profile_mask = selected_vertical_profile();
        if (profile_mask == 0) {
            std::fprintf(stderr,
                "FAIL: ICECC_TEST_P51_PROFILE must be P29V1, ZSTD_TU, or ZSTD_ROUTE\n");
            return 2;
        }
        return run_p51_vertical(argv[1], argv[2], icecc, 1, profile_mask,
                                false, p51_cancel_scenario);
    }

    const bool restart_f_c1f2 =
        ::getenv("ICECC_TEST_P51_RESTART_F_C1F2") != nullptr;
    const bool restart_c_c2f1 =
        ::getenv("ICECC_TEST_P51_RESTART_C_C2F1") != nullptr;
    const bool restart_w30_f_c1f2 =
        ::getenv("ICECC_TEST_P51_RESTART_W30_F_C1F2") != nullptr;
    const bool restart_w30_c_c2f1 =
        ::getenv("ICECC_TEST_P51_RESTART_W30_C_C2F1") != nullptr;
    const char *restart_w30_topology =
        ::getenv("ICECC_TEST_P51_RESTART_W30_TOPOLOGY");
    const bool restart_w30_topology_selected =
        restart_w30_topology != nullptr;
    const bool restart_chain_f_c_w30 =
        ::getenv("ICECC_TEST_P51_RESTART_CHAIN_F_C_W30") != nullptr;
    const bool synthetic_scheduler_w30 =
        ::getenv("ICECC_TEST_P51_SYNTH_SCHEDULER_W30") != nullptr;
    const char *lost_receipts_value =
        ::getenv("ICECC_TEST_P51_LOST_RECEIPTS");
    const bool lost_receipts = lost_receipts_value != nullptr;
    const unsigned restart_selectors = static_cast<unsigned>(restart_f_c1f2) +
        static_cast<unsigned>(restart_c_c2f1) +
        static_cast<unsigned>(restart_w30_f_c1f2) +
        static_cast<unsigned>(restart_w30_c_c2f1) +
        static_cast<unsigned>(restart_w30_topology_selected) +
        static_cast<unsigned>(restart_chain_f_c_w30) +
        static_cast<unsigned>(synthetic_scheduler_w30) +
        static_cast<unsigned>(lost_receipts);
    if (restart_selectors != 0) {
        const uint32_t profile_mask = selected_vertical_profile();
        if (profile_mask == 0) {
            std::fprintf(stderr,
                "FAIL: ICECC_TEST_P51_PROFILE must be P29V1, ZSTD_TU, or ZSTD_ROUTE\n");
            return 2;
        }
        if (restart_selectors != 1) {
            std::fprintf(stderr, "FAIL: select one P51 restart/epoch fixture\n");
            return 2;
        }
        if (synthetic_scheduler_w30)
            return run_p51_synthetic_scheduler_epoch_w30(
                argv[1], argv[2], icecc, profile_mask);
        if (lost_receipts) {
            char *end = nullptr;
            errno = 0;
            const unsigned long count = std::strtoul(
                lost_receipts_value, &end, 10);
            if (errno != 0 || end == lost_receipts_value || *end != '\0' ||
                (count != 1 && count != 2 && count != 30)) {
                std::fprintf(stderr,
                    "FAIL: ICECC_TEST_P51_LOST_RECEIPTS must be 1, 2, or 30\n");
                return 2;
            }
            return run_p51_vertical(argv[1], argv[2], icecc,
                                    static_cast<unsigned>(count),
                                    profile_mask, true);
        }
        if (restart_chain_f_c_w30)
            return run_p51_process_restart_case(
                argv[1], argv[2], icecc, 2, 2, profile_mask, 30, true);
        unsigned c_count = 0;
        unsigned f_count = 0;
        unsigned jobs_per_window = 1;
        if (restart_w30_topology_selected) {
            char trailing = '\0';
            if (std::sscanf(restart_w30_topology, "C%uF%u%c",
                            &c_count, &f_count, &trailing) != 2 ||
                c_count == 0 || c_count > 4 || f_count == 0 || f_count > 4 ||
                c_count * f_count > 4 ||
                !((c_count == 1 && f_count >= 2) ||
                  (f_count == 1 && c_count >= 2))) {
                std::fprintf(stderr,
                    "FAIL: ICECC_TEST_P51_RESTART_W30_TOPOLOGY must be C1F2..C1F4 or C2F1..C4F1\n");
                return 2;
            }
            jobs_per_window = 30;
        } else {
            const bool restart_f = restart_f_c1f2 || restart_w30_f_c1f2;
            c_count = restart_f ? 1u : 2u;
            f_count = restart_f ? 2u : 1u;
            jobs_per_window =
                restart_w30_f_c1f2 || restart_w30_c_c2f1 ? 30u : 1u;
        }
        return run_p51_process_restart_case(
            argv[1], argv[2], icecc, c_count, f_count, profile_mask,
            jobs_per_window);
    }

    if (const char *topology = ::getenv("ICECC_TEST_P51_MULTILINK");
        topology != nullptr) {
        const uint32_t profile_mask = selected_vertical_profile();
        if (profile_mask == 0) {
            std::fprintf(stderr,
                "FAIL: ICECC_TEST_P51_PROFILE must be P29V1, ZSTD_TU, or ZSTD_ROUTE\n");
            return 2;
        }
        unsigned c_count = 0;
        unsigned f_count = 0;
        char trailing = '\0';
        if (std::sscanf(topology, "C%uF%u%c", &c_count, &f_count, &trailing) != 2 ||
            c_count < 1 || c_count > 4 || f_count < 1 || f_count > 4 ||
            c_count * f_count <= 1 || c_count * f_count > 4) {
            std::fprintf(stderr,
                "FAIL: ICECC_TEST_P51_MULTILINK must be C1F2..C1F4 or C2F1..C4F1\n");
            return 2;
        }
        return run_p51_multilink_topology(
            argv[1], argv[2], icecc, c_count, f_count, profile_mask);
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
    if (p51_expired_arm_wire_case) {
        const uint32_t profile = selected_vertical_profile();
        if (profile == 0) {
            std::fprintf(stderr,
                "FAIL: ICECC_TEST_P51_PROFILE must be P29V1, ZSTD_TU, or ZSTD_ROUTE\n");
            return 2;
        }
        const std::string request_id = std::to_string(kExpiredArmWireNonce);
        const std::string budget = std::to_string(kExpiredArmWireBudgetMsec);
        ::setenv("ICECC_TEST_P51_PAUSE_AFTER_GOODBYE_REQUEST",
                 request_id.c_str(), 1);
        ::setenv("ICECC_TEST_P50_SOURCE_BUDGET_MSEC", budget.c_str(), 1);
    }
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
    const bool p51_r2_positive_case =
        p51_cancel_case || p51_expired_arm_wire_case;
    REQUIRE(present_revision(
                positive, static_cast<uint32_t>(daemon_port),
                p51_r2_positive_case ? CACHE_WIRE_REVISION_R2
                                     : CACHE_WIRE_REVISION_R1),
            "real READY/authenticated sidecar publishes exact positive advertisement");
    delete positive_message;

    if (p51_expired_arm_wire_case) {
        const uint32_t profile = selected_vertical_profile();
        auto prepare = [&](uint32_t wire_id, uint64_t nonce) {
            const bool sent = scheduler && scheduler->send_msg(
                AssignPrepareMsg(epoch, wire_id, nonce, 1));
            Msg *reply = sent
                ? wait_for_type(scheduler, Msg::ASSIGN_READY, 5000) : nullptr;
            const auto *ready = dynamic_cast<const AssignReadyMsg *>(reply);
            const bool exact = ready && ready->wire_id == wire_id &&
                ready->epoch() == epoch && ready->nonce() == nonce;
            delete reply;
            return exact;
        };
        auto make_arm = [&](uint32_t wire_id, uint64_t nonce) {
            P50SourceArmFields source = source_arm(
                wire_id, epoch, nonce, static_cast<uint32_t>(daemon_port),
                static_cast<uint32_t>(daemon_port));
            source.cache_protocol = CACHE_WIRE_REVISION_R2;
            source.cache_profile = profile;
            source.source_mode = source_mode_for_profile(profile);
            source.c_control_attempt = kExpiredArmWireNonce;
            return P51SourceArmFields{source, 30};
        };

        const bool first_ready = prepare(kExpiredArmWireJob,
                                         kExpiredArmWireNonce);
        MsgChannel *first_wrapper = first_ready
            ? connect_tcp_bounded(daemon_port, 5000) : nullptr;
        const auto arm_sent_at = Clock::now();
        const P51SourceArmMsg expired_request{
            make_arm(kExpiredArmWireJob, kExpiredArmWireNonce)};
        const bool first_sent = first_wrapper &&
            first_wrapper->send_msg(expired_request);
        int stopped_status = 0;
        bool daemon_stopped = false;
        const auto stop_deadline = Clock::now() + std::chrono::seconds(5);
        while (first_sent && Clock::now() < stop_deadline) {
            const pid_t waited = ::waitpid(
                daemon_pid, &stopped_status, WNOHANG | WUNTRACED);
            if (waited == daemon_pid) {
                daemon_stopped = WIFSTOPPED(stopped_status);
                break;
            }
            if (waited < 0 && errno != EINTR) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto stopped_at = Clock::now();
        const bool stopped_before_deadline = daemon_stopped &&
            stopped_at - arm_sent_at <
                std::chrono::milliseconds(kExpiredArmWireBudgetMsec);
        REQUIRE(first_ready && first_sent && daemon_stopped &&
                    stopped_before_deadline,
                "daemon reached post-Goodbye pause for the exact successful reservation before its deadline");

        if (daemon_stopped) {
            std::this_thread::sleep_until(
                stopped_at + std::chrono::milliseconds(
                    kExpiredArmWireBudgetMsec + 250));
            const bool resumed = ::kill(daemon_pid, SIGCONT) == 0;
            daemon_stopped = !resumed;
            REQUIRE(resumed,
                    "stopped daemon resumed only after the unchanged original ARM deadline elapsed");
        }

        Msg *expired_response = first_wrapper
            ? wait_for_any_type(first_wrapper, 5000) : nullptr;
        const bool terminal_end = expired_response &&
            *expired_response == Msg::END;
        delete expired_response;
        bool eof_after_end = false;
        if (terminal_end && first_wrapper != nullptr) {
            bool unexpected_trailing_message = false;
            const auto response_deadline = Clock::now() + std::chrono::seconds(5);
            while (Clock::now() < response_deadline) {
                Msg *trailing = first_wrapper->get_msg(1, true);
                if (trailing != nullptr) {
                    unexpected_trailing_message = true;
                    delete trailing;
                    break;
                }
                if (first_wrapper->at_eof()) {
                    eof_after_end = true;
                    break;
                }
            }
            eof_after_end = eof_after_end && !unexpected_trailing_message;
        }
        REQUIRE(terminal_end && eof_after_end,
                "expired exact R2 reservation produced End then EOF, with no wire P51_SOURCE_ARMED");
        delete first_wrapper;

        const bool fresh_ready = prepare(kFreshArmWireJob, kFreshArmWireNonce);
        MsgChannel *fresh_wrapper = fresh_ready
            ? connect_tcp_bounded(daemon_port, 5000) : nullptr;
        const P51SourceArmMsg fresh_request{
            make_arm(kFreshArmWireJob, kFreshArmWireNonce)};
        const bool fresh_sent = fresh_wrapper &&
            fresh_wrapper->send_msg(fresh_request);
        Msg *fresh_message = fresh_sent
            ? wait_for_type(fresh_wrapper, Msg::P51_SOURCE_ARMED, 5000) : nullptr;
        const auto *fresh_armed =
            dynamic_cast<const P51SourceArmedMsg *>(fresh_message);
        const bool fresh_success = fresh_armed &&
            fresh_armed->acknowledges(fresh_request) &&
            fresh_armed->selected_window == 30 &&
            fresh_armed->f_store_generation != 0;
        delete fresh_message;
        REQUIRE(fresh_ready && fresh_sent && fresh_success,
                "fresh exact ARM succeeds on the same healthy daemon after late ARM rejection");
        delete fresh_wrapper;

        if (daemon_stopped) (void)::kill(daemon_pid, SIGCONT);
        (void)::kill(daemon_pid, SIGTERM);
        int status = 0;
        bool reaped = wait_child(daemon_pid, 10000, &status);
        if (!reaped) {
            (void)::kill(daemon_pid, SIGKILL);
            (void)::waitpid(daemon_pid, &status, 0);
        }
        REQUIRE(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "wire deadline fixture daemon exits cleanly under bounded cleanup");
        delete scheduler;
        ::close(scheduler_listener);
        if (failures == 0) std::filesystem::remove_all(work);
        else std::fprintf(stderr,
                          "retained failing work directory: %s\n", work.c_str());
        return failures ? 1 : 0;
    }

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
