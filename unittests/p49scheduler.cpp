/* Real-scheduler Protocol-49 assignment lifecycle gate.
   Fake daemons use the production channel and message classes while an actual
   scheduler owns selection, exposure, cancellation, and id reuse. */

#include "comm.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using Clock = std::chrono::steady_clock;
static int failures = 0;

#define REQUIRE(cond, text) do {                                      \
    if (cond) { std::fprintf(stderr, "ok       - %s\n", text); }      \
    else { std::fprintf(stderr, "FAILED   - %s\n", text); ++failures; } \
} while (0)

static int bind_port(int requested, int *actual)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(requested));
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        close(fd);
        return -1;
    }
    *actual = ntohs(address.sin_port);
    return fd;
}

static int bind_scheduler_port(int type, int requested, int *actual)
{
    const int fd = socket(AF_INET, type, 0);
    if (fd < 0) return -1;
    if (type == SOCK_STREAM) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(requested));
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        close(fd);
        return -1;
    }
    *actual = ntohs(address.sin_port);
    return fd;
}

static int reserve_port_pair()
{
    /* A scheduler binds wildcard TCP on port/port+1 and wildcard UDP on
       port.  Reserving only loopback TCP can falsely pass while another
       scheduler owns UDP, producing a late bind failure after fork.  Hold all
       three exact sockets through selection and release them together just
       before the child starts. */
    for (int attempt = 0; attempt != 100; ++attempt) {
        int port = 0;
        int first = bind_scheduler_port(SOCK_STREAM, 0, &port);
        if (first < 0 || port >= 65535) {
            if (first >= 0) close(first);
            continue;
        }
        int ignored = 0;
        int second = bind_scheduler_port(SOCK_STREAM, port + 1, &ignored);
        int broadcast = bind_scheduler_port(SOCK_DGRAM, port, &ignored);
        if (second >= 0 && broadcast >= 0) {
            close(broadcast);
            close(second);
            close(first);
            return port;
        }
        if (broadcast >= 0) close(broadcast);
        if (second >= 0) close(second);
        close(first);
    }
    return 0;
}

static int tcp_connect(int port, int receive_buffer = 0,
                       const char *source_address = nullptr)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (receive_buffer > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                   &receive_buffer, sizeof(receive_buffer));
    }
    if (source_address != nullptr) {
        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        if (inet_pton(AF_INET, source_address, &local.sin_addr) != 1 ||
            bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
            close(fd);
            return -1;
        }
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool read_exact(int fd, void *buffer, size_t size)
{
    char *out = static_cast<char *>(buffer);
    while (size) {
        const ssize_t count = read(fd, out, size);
        if (count <= 0) return false;
        out += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

static bool write_exact(int fd, const void *buffer, size_t size)
{
    const char *in = static_cast<const char *>(buffer);
    while (size) {
        const ssize_t count = write(fd, in, size);
        if (count <= 0) return false;
        in += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

/* Translate only the symmetric three-step version handshake, then relay all
   production frames unchanged.  This creates a genuine negotiated-P48
   scheduler link while the harness itself remains built from current code. */
static pid_t start_p48_proxy(int scheduler_port, int *proxy_port)
{
    int listener = bind_port(0, proxy_port);
    if (listener < 0 || listen(listener, 1) != 0) {
        if (listener >= 0) close(listener);
        return -1;
    }
    pid_t child = fork();
    if (child != 0) {
        close(listener);
        return child;
    }
    int downstream = accept(listener, nullptr, nullptr);
    int upstream = -1;
    const auto connect_deadline = Clock::now() + std::chrono::seconds(5);
    while (upstream < 0 && Clock::now() < connect_deadline) {
        upstream = tcp_connect(scheduler_port);
        if (upstream < 0) usleep(20 * 1000);
    }
    close(listener);
    unsigned char down_version[4] {};
    unsigned char up_version[4] {};
    const unsigned char p48[4] { 48, 0, 0, 0 };
    if (downstream < 0 || upstream < 0
            || !read_exact(downstream, down_version, sizeof(down_version))
            || !read_exact(upstream, up_version, sizeof(up_version))
            || !write_exact(downstream, p48, sizeof(p48))
            || !write_exact(upstream, p48, sizeof(p48))
            || !read_exact(downstream, down_version, sizeof(down_version))
            || !read_exact(upstream, up_version, sizeof(up_version))
            || !write_exact(downstream, up_version, sizeof(up_version))
            || !write_exact(upstream, down_version, sizeof(down_version))) {
        _exit(2);
    }
    for (;;) {
        pollfd pfds[2] {{ downstream, POLLIN, 0 }, { upstream, POLLIN, 0 }};
        if (poll(pfds, 2, -1) <= 0) continue;
        char bytes[8192];
        for (int i = 0; i != 2; ++i) {
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const int source = i == 0 ? downstream : upstream;
            const int destination = i == 0 ? upstream : downstream;
            const ssize_t count = read(source, bytes, sizeof(bytes));
            if (count <= 0
                    || !write_exact(destination, bytes,
                                    static_cast<size_t>(count))) {
                close(downstream);
                close(upstream);
                _exit(0);
            }
        }
    }
}

static MsgChannel *connect_scheduler(int port, int timeout_msec = 5000,
                                     int receive_buffer = 0,
                                     const char *source_address = nullptr)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        int fd = tcp_connect(port, receive_buffer, source_address);
        if (fd >= 0) {
            sockaddr_in peer {};
            peer.sin_family = AF_INET;
            MsgChannel *channel = Service::createChannel(
                fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
            if (channel) return channel;
        }
        usleep(20 * 1000);
    }
    return nullptr;
}

static Msg *next_message(MsgChannel *channel, int timeout_msec)
{
    if (!channel) return nullptr;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        if (Msg *msg = channel->get_msg(0, true)) return msg;
        if (channel->at_eof()) return nullptr;
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count());
        if (left <= 0) break;
        pollfd pfd { channel->fd, POLLIN, 0 };
        poll(&pfd, 1, left > 20 ? 20 : left);
    }
    return nullptr;
}

static Msg *wait_type(MsgChannel *channel, Msg::Value wanted, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count());
        if (left <= 0) break;
        Msg *msg = next_message(channel, left > 100 ? 100 : left);
        if (msg) {
            if (*msg == wanted) return msg;
            delete msg;
        }
        if (channel->at_eof()) return nullptr;
    }
    return nullptr;
}

static bool no_type(MsgChannel *channel, Msg::Value unwanted, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count());
        if (left <= 0) break;
        Msg *msg = next_message(channel, left > 50 ? 50 : left);
        if (msg) {
            const bool found = *msg == unwanted;
            delete msg;
            if (found) return false;
        }
        if (channel->at_eof()) break;
    }
    return true;
}

static bool no_assignment_reply(MsgChannel *channel, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count());
        if (left <= 0) break;
        Msg *msg = next_message(channel, left > 50 ? 50 : left);
        if (msg) {
            const bool assignment = *msg == Msg::USE_CS || *msg == Msg::NO_CS;
            delete msg;
            if (assignment) return false;
        }
        if (channel->at_eof()) break;
    }
    return true;
}

static bool wait_eof(MsgChannel *channel, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        Msg *msg = next_message(channel, 100);
        delete msg;
        if (channel->at_eof()) return true;
    }
    return channel && channel->at_eof();
}

static pid_t start_scheduler(const std::string &binary, int port,
                             const char *mode, const std::string &log_path,
                             unsigned int job_domain = 1,
                             unsigned int max_outstanding = 32,
                             bool shrink_send_buffer = false,
                             bool force_ready_nested_teardown = false)
{
    pid_t child = fork();
    if (child != 0) return child;
    signal(SIGPIPE, SIG_DFL);
    setenv("ICECC_TESTS", "1", 1);
    if (force_ready_nested_teardown) {
        setenv("ICECC_TEST_P49_READY_NESTED_TEARDOWN", "1", 1);
    }
    if (shrink_send_buffer) {
        const std::string marker = "/scheduler/";
        const std::string::size_type split = binary.rfind(marker);
        if (split != std::string::npos) {
            const std::string shim = binary.substr(0, split)
                + "/unittests/sndbuf_shim.so";
            setenv("LD_PRELOAD", shim.c_str(), 1);
            setenv("ICECC_TEST_SNDBUF", "4096", 1);
            setenv("ICECC_TEST_STRIP_USER_TIMEOUT", "1", 1);
        }
    }
    char domain_text[32];
    std::snprintf(domain_text, sizeof(domain_text), "%u", job_domain);
    setenv("ICECC_TEST_JOB_ID_DOMAIN", domain_text, 1);
    FILE *log = std::fopen(log_path.c_str(), "w");
    if (log) {
        dup2(fileno(log), STDOUT_FILENO);
        dup2(fileno(log), STDERR_FILENO);
    }
    char port_text[16];
    std::snprintf(port_text, sizeof(port_text), "%d", port);
    char netname[64];
    std::snprintf(netname, sizeof(netname), "p49-test-%ld-%d",
                  static_cast<long>(getpid()), port);
    char outstanding_text[32];
    std::snprintf(outstanding_text, sizeof(outstanding_text), "%u",
                  max_outstanding);
    if (mode) {
        execl(binary.c_str(), binary.c_str(), "-p", port_text, "-n", netname,
              "-r", "--assignment-fence-mode", mode,
              "--max-outstanding-dispatches", outstanding_text,
              "-vvv", (char *)nullptr);
    } else {
        execl(binary.c_str(), binary.c_str(), "-p", port_text, "-n", netname,
              "-r", "--max-outstanding-dispatches", outstanding_text,
              "-vvv", (char *)nullptr);
    }
    _exit(127);
}

static pid_t start_cache_routing_scheduler(const std::string &binary, int port,
                                           const std::string &log_path,
                                           const char *mode = "advisory")
{
    pid_t child = fork();
    if (child != 0) return child;
    signal(SIGPIPE, SIG_DFL);
    setenv("ICECC_TESTS", "1", 1);
    setenv("ICECC_TEST_JOB_ID_DOMAIN", "64", 1);
    setenv("ICECC_P50_PROFILE", "P29V1", 1);
    FILE *log = std::fopen(log_path.c_str(), "w");
    if (log) {
        dup2(fileno(log), STDOUT_FILENO);
        dup2(fileno(log), STDERR_FILENO);
    }
    char port_text[16];
    std::snprintf(port_text, sizeof(port_text), "%d", port);
    char netname[64];
    std::snprintf(netname, sizeof(netname), "p50-route-test-%ld-%d",
                  static_cast<long>(getpid()), port);
    if (mode) {
        execl(binary.c_str(), binary.c_str(), "-p", port_text, "-n", netname,
              "-r", "--assignment-fence-mode", mode,
              "--max-outstanding-dispatches", "32", "-a", "least_busy",
              "-vvv", (char *)nullptr);
    } else {
        execl(binary.c_str(), binary.c_str(), "-p", port_text, "-n", netname,
              "-r", "--max-outstanding-dispatches", "32", "-a", "least_busy",
              "-vvv", (char *)nullptr);
    }
    _exit(127);
}

static bool stop_scheduler(pid_t child)
{
    if (child <= 0)
        return false;
    kill(child, SIGTERM);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    int status = 0;
    while (Clock::now() < deadline) {
        if (waitpid(child, &status, WNOHANG) == child) {
            return WIFEXITED(status) || WIFSIGNALED(status);
        }
        usleep(20 * 1000);
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    return false;
}

static MsgChannel *login_host(int port, const char *name, bool worker,
                              int worker_port,
                              ConfCSMsg **configuration,
                              MsgChannel *connected = nullptr,
                              unsigned int worker_slots = 1,
                              int receive_buffer = 0,
                              uint32_t cache_endpoint_port = 0,
                              uint32_t cache_protocol = 0,
                              uint32_t cache_profiles = 0)
{
    *configuration = nullptr;
    MsgChannel *channel = connected
        ? connected : connect_scheduler(port, 5000, receive_buffer);
    if (!channel) return nullptr;
    LoginMsg login(worker ? static_cast<unsigned int>(worker_port) : 0,
                   name, "x86_64", 0);
    login.envs.push_back(std::make_pair(std::string("x86_64"),
                                        std::string("p49-test-env")));
    login.max_kids = worker ? worker_slots : 0;
    login.noremote = !worker;
    login.chroot_possible = worker;
    login.setCacheAdvertisement(cache_endpoint_port, cache_protocol,
                                cache_profiles);
    if (!channel->send_msg(login)) {
        delete channel;
        return nullptr;
    }
    Msg *msg = wait_type(channel, Msg::CS_CONF, 3000);
    *configuration = dynamic_cast<ConfCSMsg *>(msg);
    if (!*configuration) {
        delete msg;
        delete channel;
        return nullptr;
    }
    if (worker) {
        StatsMsg stats;
        channel->send_msg(stats);
    }
    return channel;
}

static bool request_job(MsgChannel *submitter, uint32_t client_id,
                        unsigned int count = 1)
{
    GetCSMsg request(
        Environments { std::make_pair(std::string("x86_64"),
                                      std::string("p49-test-env")) },
        "p49-test.cpp", CompileJob::Lang_CXX, 1, "x86_64", 0,
        std::string(), 0, 0, 0);
    request.client_id = client_id;
    request.count = count;
    return submitter->send_msg(request);
}

static bool request_remote_required_job(MsgChannel *submitter,
                                        uint32_t client_id)
{
    GetCSMsg request(
        Environments { std::make_pair(std::string("x86_64"),
                                      std::string("p49-test-env")) },
        "remote-required.cpp", CompileJob::Lang_CXX, 1, "x86_64", 0,
        std::string(), 50, 0, 0);
    request.client_id = client_id;
    request.remote_required = 1;
    return submitter->send_msg(request);
}

static bool request_cache_job(MsgChannel *submitter, uint32_t client_id,
                              const std::string &affinity_host = std::string(),
                              uint32_t affinity_port = 0,
                              uint32_t affinity_profiles = 0,
                              const std::string &retry_avoid_host = std::string(),
                              uint32_t retry_avoid_port = 0)
{
    GetCSMsg request(
        Environments { std::make_pair(std::string("x86_64"),
                                      std::string("p49-test-env")) },
        "p50-route-test.cpp", CompileJob::Lang_CXX, 1, "x86_64", 0,
        std::string(), 0, 0, 0);
    request.client_id = client_id;
    request.cache_protocol = CACHE_WIRE_REVISION;
    request.cache_profile_mask = CACHE_ADVERTISABLE_PROFILE_MASK;
    request.cache_affinity_profile_mask = affinity_profiles;
    request.cache_affinity_port = affinity_port;
    request.cache_affinity_host = affinity_host;
    request.cache_retry_avoid_port = retry_avoid_port;
    request.cache_retry_avoid_host = retry_avoid_host;
    return submitter->send_msg(request);
}

/* Forces pick_server's early "user wants to test/prefer one specific
   daemon" path (scheduler.cpp): CompileServer::matches() accepts either the
   Login nodename or the numeric peer address, so the nodename passed to
   login_host works here. */
static bool request_job_preferring(MsgChannel *submitter, uint32_t client_id,
                                   const std::string &preferred_host,
                                   const std::string &retry_avoid_host = std::string(),
                                   uint32_t retry_avoid_port = 0)
{
    GetCSMsg request(
        Environments { std::make_pair(std::string("x86_64"),
                                      std::string("p49-test-env")) },
        "p49-test.cpp", CompileJob::Lang_CXX, 1, "x86_64", 0,
        preferred_host, 0, 0, 0);
    request.client_id = client_id;
    request.cache_protocol = CACHE_WIRE_REVISION;
    request.cache_profile_mask = CACHE_ADVERTISABLE_PROFILE_MASK;
    request.cache_retry_avoid_host = retry_avoid_host;
    request.cache_retry_avoid_port = retry_avoid_port;
    return submitter->send_msg(request);
}

static bool set_worker_load(MsgChannel *worker, uint32_t load)
{
    StatsMsg stats;
    stats.load = load;
    stats.loadAvg1 = 0;
    stats.loadAvg5 = 0;
    stats.loadAvg10 = 0;
    stats.freeMem = 0;
    return worker && worker->send_msg(stats);
}

static std::string retry_decision_record(const UseCSMsg &use, int failed_port,
                                          bool alternative)
{
    return "P50_RETRY_DECISION job=" + std::to_string(use.job_id) +
        " epoch=" + std::to_string(use.assignmentEpoch()) +
        " nonce=" + std::to_string(use.assignmentNonce()) +
        " failed=127.0.0.1:" + std::to_string(failed_port) +
        " selected=" + use.hostname + ":" + std::to_string(use.port) +
        " profile=" + std::to_string(use.cache_profile_mask) +
        " compatible_alternative=" + (alternative ? "1" : "0");
}

static bool file_contains(const std::string &path, const std::string &needle)
{
    std::ifstream input(path);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str().find(needle) != std::string::npos;
}

static size_t file_occurrence_count(const std::string &path,
                                    const std::string &needle)
{
    if (needle.empty()) return 0;
    std::ifstream input(path);
    std::ostringstream stream;
    stream << input.rdbuf();
    const std::string text = stream.str();
    size_t count = 0;
    std::string::size_type position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

static bool wait_file_contains(const std::string &path,
                               const std::string &needle,
                               int timeout_msec)
{
    const auto deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        if (file_contains(path, needle)) return true;
        usleep(20 * 1000);
    }
    return file_contains(path, needle);
}

/* Reservation sockets must be released before exec: the production scheduler
   does not accept inherited listeners. Retry only a confirmed startup bind
   collision, never a scenario failure or an unexplained readiness timeout. */
static pid_t start_cache_routing_scheduler_retry(
    const std::string &binary, const std::string &directory,
    int *selected_port, std::string *selected_log)
{
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    for (unsigned attempt = 0; attempt != 4 && Clock::now() < deadline; ++attempt) {
        const int port = reserve_port_pair();
        if (port == 0) return -1;
        const std::string suffix = attempt == 0 ? std::string()
            : ".attempt" + std::to_string(attempt + 1);
        const std::string log = directory + "/pre-exposure-worker-loss" + suffix + ".log";
        int injected = -1;
        if (attempt == 0 && std::getenv("ICECC_TEST_P49_OCCUPY_RESERVED_PORT")) {
            int ignored = 0;
            injected = bind_scheduler_port(SOCK_STREAM, port, &ignored);
            if (injected >= 0 && listen(injected, 1) != 0) {
                close(injected);
                injected = -1;
            }
            if (injected < 0)
                return -1;
        }
        const pid_t child = start_cache_routing_scheduler(binary, port, log, "strict-nonce");
        if (child <= 0) {
            if (injected >= 0) close(injected);
            return -1;
        }
        bool collision = false;
        bool ready = false;
        while (Clock::now() < deadline) {
            collision = file_contains(log, "bind()(Error: Address already in use)");
            if (collision) break;
            if (file_contains(log, "scheduler ready, algorithm:")) {
                ready = true;
                break;
            }
            usleep(20 * 1000);
        }
        if (injected >= 0) close(injected);
        if (collision) {
            if (!stop_scheduler(child))
                return -1;
            continue;
        }
        if (!ready) {
            stop_scheduler(child);
            return -1;
        }
        *selected_port = port;
        *selected_log = log;
        return child;
    }
    return -1;
}

static JobDoneMsg job_done_for(const UseCSMsg &use, int exitcode,
                               unsigned int flags);

static void run_remote_required_waits_for_worker(
    const std::string &binary, const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/remote-required.log";
    pid_t scheduler = start_scheduler(binary, port, nullptr, log);
    REQUIRE(port != 0 && scheduler > 0,
            "remote-required scheduler process launched");

    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(
        port, "remote-required-submit", false, 0, &submitter_conf);
    delete submitter_conf;
    REQUIRE(submitter && request_remote_required_job(submitter, 9971),
            "remote-required assignment requested without a worker");
    REQUIRE(no_assignment_reply(submitter, 500),
            "remote-required job remains queued instead of selecting submitter");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(
        port, "remote-required-worker", true, worker_port, &worker_conf);
    delete worker_conf;
    UseCSMsg *use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(worker && use,
            "queued remote-required job selects the newly available worker");
    if (worker && use) {
        worker->send_msg(JobBeginMsg(use->job_id, 0));
        worker->send_msg(job_done_for(*use, 0, JobDoneMsg::FROM_SERVER));
    }

    delete use;
    delete submitter;
    delete worker;
    if (worker_listener >= 0) close(worker_listener);
    REQUIRE(stop_scheduler(scheduler),
            "remote-required scheduler stopped cleanly");
}

static std::string control_text(int port, const char *command)
{
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    int fd = -1;
    while (fd < 0 && Clock::now() < deadline) {
        fd = tcp_connect(port + 1);
        if (fd < 0) usleep(20 * 1000);
    }
    if (fd < 0) return std::string();
    char buffer[8192];
    pollfd pfd { fd, POLLIN, 0 };
    if (poll(&pfd, 1, 2000) <= 0 || read(fd, buffer, sizeof(buffer)) <= 0) {
        close(fd);
        return std::string();
    }
    const std::string request = std::string(command) + "\n";
    if (write(fd, request.data(), request.size())
            != static_cast<ssize_t>(request.size())) {
        close(fd);
        return std::string();
    }
    std::string result;
    while (Clock::now() < deadline) {
        pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, 100) <= 0) continue;
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count <= 0) break;
        result.append(buffer, static_cast<size_t>(count));
        if (result.find("200 done") != std::string::npos) break;
    }
    close(fd);
    return result.find("200 done") != std::string::npos
        ? result : std::string();
}

static AssignPrepareMsg *wait_prepare(MsgChannel *worker)
{
    return dynamic_cast<AssignPrepareMsg *>(
        wait_type(worker, Msg::ASSIGN_PREPARE, 3000));
}

static JobDoneMsg job_done_for(const UseCSMsg &use, int exitcode,
                               unsigned int flags)
{
    return JobDoneMsg(use.job_id, exitcode, flags, 0,
                      use.assignmentEpoch(), use.assignmentNonce(),
                      use.cGuid(), use.tuSeq());
}

static void run_precompile_worker_terminal(const std::string &binary,
                                           const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/precompile-worker-terminal.log";
    pid_t scheduler = start_scheduler(
        binary, port, "enforcing-compat", log);
    REQUIRE(port != 0 && scheduler > 0,
            "pre-compile worker-terminal scheduler process launched");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(port, "precompile-worker", true,
                                    worker_port, &worker_conf);
    delete worker_conf;
    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(port, "precompile-submit", false, 0,
                                       &submitter_conf);
    delete submitter_conf;
    REQUIRE(worker && submitter && request_job(submitter, 9901),
            "pre-compile failure assignment requested");

    AssignPrepareMsg *first = wait_prepare(worker);
    if (first) {
        worker->send_msg(AssignReadyMsg(first->epoch(), first->wire_id,
                                        first->nonce()));
    }
    UseCSMsg *first_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(first && first_use && first_use->job_id == first->wire_id &&
                first_use->cGuid() != 0,
            "pre-compile failure has an exposed fenced assignment");

    if (first_use) {
        worker->send_msg(JobDoneMsg(
            first_use->job_id, 149, JobDoneMsg::FROM_SERVER, 0,
            first_use->assignmentEpoch(), first_use->assignmentNonce(), 0, 0));
    }
    usleep(100 * 1000);
    REQUIRE(submitter && request_job(submitter, 9902),
            "assignment requested after exact pre-compile worker failure");
    AssignPrepareMsg *second = wait_prepare(worker);
    REQUIRE(first && second && second->wire_id == first->wire_id &&
                second->nonce() != first->nonce(),
            "pre-compile worker failure preserves F and releases the job id");

    if (second) {
        worker->send_msg(AssignReadyMsg(second->epoch(), second->wire_id,
                                        second->nonce()));
    }
    UseCSMsg *second_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(second && second_use && second_use->job_id == second->wire_id,
            "post-failure assignment reaches the preserved worker");
    if (second_use) {
        worker->send_msg(JobBeginMsg(second_use->job_id, 0));
        worker->send_msg(JobDoneMsg(
            second_use->job_id, 149, JobDoneMsg::FROM_SERVER, 0,
            second_use->assignmentEpoch(), second_use->assignmentNonce(), 0, 0));
    }
    REQUIRE(wait_eof(worker, 3000),
            "a begun compile still rejects an absent compile identity");

    delete second_use;
    delete second;
    delete first_use;
    delete first;
    delete submitter;
    delete worker;
    if (worker_listener >= 0) close(worker_listener);
    REQUIRE(stop_scheduler(scheduler),
            "pre-compile worker-terminal scheduler stopped cleanly");
}

static void run_post_begin_submitter_withdrawal(const std::string &binary,
                                                const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/post-begin-withdrawal.log";
    pid_t scheduler = start_scheduler(
        binary, port, "enforcing-compat", log, 16, 1);
    REQUIRE(port != 0 && scheduler > 0,
            "post-Begin withdrawal scheduler process launched");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(port, "post-begin-worker", true,
                                    worker_port, &worker_conf);
    delete worker_conf;
    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(port, "post-begin-submit", false, 0,
                                       &submitter_conf);
    delete submitter_conf;
    REQUIRE(worker && submitter && request_job(submitter, 9951),
            "post-Begin withdrawal assignment requested");

    AssignPrepareMsg *first = wait_prepare(worker);
    if (first) {
        worker->send_msg(AssignReadyMsg(first->epoch(), first->wire_id,
                                        first->nonce()));
    }
    UseCSMsg *first_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(first && first_use && first_use->job_id == first->wire_id,
            "post-Begin withdrawal receives the fenced assignment");
    if (first_use) {
        worker->send_msg(JobBeginMsg(first_use->job_id, 0));
        usleep(100 * 1000);
        submitter->send_msg(job_done_for(*first_use, 106,
                                         JobDoneMsg::FROM_SUBMITTER));
        submitter->send_msg(job_done_for(*first_use, 106,
                                         JobDoneMsg::FROM_SUBMITTER));
    }

    ConfCSMsg *probe_conf = nullptr;
    MsgChannel *probe = login_host(port, "post-begin-probe", false, 0,
                                   &probe_conf);
    delete probe_conf;
    REQUIRE(probe && request_job(probe, 9952),
            "capacity probe submitted while begun F work is outstanding");
    AssignPrepareMsg *preloaded = wait_prepare(worker);
    REQUIRE(first && preloaded && preloaded->wire_id != first->wire_id,
            "post-Begin withdrawal retains the old id while one preload is admitted");

    ConfCSMsg *second_probe_conf = nullptr;
    MsgChannel *second_probe = login_host(
        port, "post-begin-second-probe", false, 0, &second_probe_conf);
    delete second_probe_conf;
    REQUIRE(second_probe && request_job(second_probe, 9953),
            "second capacity probe submitted beyond the preload slot");
    REQUIRE(no_type(worker, Msg::ASSIGN_PREPARE, 500),
            "submitter withdrawal after Begin does not release live F capacity");
    REQUIRE(first_use && !file_contains(
                log, "END " + std::to_string(first_use->job_id)
                     + " status=106"),
            "submitter withdrawal after Begin emits no premature END");

    if (first_use) {
        worker->send_msg(job_done_for(*first_use, 0,
                                      JobDoneMsg::FROM_SERVER));
    }
    AssignPrepareMsg *after_terminal = wait_prepare(worker);
    REQUIRE(first && preloaded && after_terminal
                && after_terminal->wire_id != preloaded->wire_id
                && after_terminal->nonce() != first->nonce(),
            "exact worker terminal releases retained capacity exactly once");
    if (preloaded) {
        worker->send_msg(AssignReadyMsg(preloaded->epoch(), preloaded->wire_id,
                                        preloaded->nonce()));
    }
    UseCSMsg *preloaded_use = dynamic_cast<UseCSMsg *>(
        wait_type(probe, Msg::USE_CS, 3000));
    REQUIRE(preloaded && preloaded_use
                && preloaded_use->job_id == preloaded->wire_id,
            "retained worker exposes the queued preload after its terminal");
    if (preloaded_use) {
        worker->send_msg(JobBeginMsg(preloaded_use->job_id, 0));
        worker->send_msg(job_done_for(*preloaded_use, 0,
                                      JobDoneMsg::FROM_SERVER));
    }
    if (after_terminal) {
        worker->send_msg(AssignReadyMsg(
            after_terminal->epoch(), after_terminal->wire_id,
            after_terminal->nonce()));
    }
    UseCSMsg *after_terminal_use = dynamic_cast<UseCSMsg *>(
        wait_type(second_probe, Msg::USE_CS, 3000));
    REQUIRE(after_terminal && after_terminal_use
                && after_terminal_use->job_id == after_terminal->wire_id,
            "released worker capacity serves the second queued probe");
    if (after_terminal_use) {
        worker->send_msg(JobBeginMsg(after_terminal_use->job_id, 0));
        worker->send_msg(job_done_for(*after_terminal_use, 0,
                                      JobDoneMsg::FROM_SERVER));
    }

    delete after_terminal_use;
    delete after_terminal;
    delete preloaded_use;
    delete preloaded;
    delete first_use;
    delete first;
    delete second_probe;
    delete probe;
    delete submitter;
    delete worker;
    if (worker_listener >= 0) close(worker_listener);
    REQUIRE(stop_scheduler(scheduler),
            "post-Begin withdrawal scheduler stopped cleanly");
}

static void run_enforcing(const std::string &binary,
                          const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/enforcing-scheduler.log";
    pid_t scheduler = start_scheduler(
        binary, port, "enforcing-compat", log);
    REQUIRE(port != 0 && scheduler > 0,
            "EnforcingCompat scheduler process launched");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(port, "p49-worker", true, worker_port,
                                    &worker_conf);
    REQUIRE(worker && worker_conf
                && worker_conf->fence_mode == ConfCSMsg::EnforcingCompat
                && worker_conf->epoch() != 0,
            "EnforcingCompat policy and nonzero scheduler epoch negotiated");
    const uint64_t epoch = worker_conf ? worker_conf->epoch() : 0;
    delete worker_conf;

    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(port, "p49-submit-a", false, 0,
                                       &submitter_conf);
    delete submitter_conf;
    REQUIRE(submitter && request_job(submitter, 1001),
            "first EnforcingCompat assignment requested");

    AssignPrepareMsg *first = wait_prepare(worker);
    REQUIRE(first && first->epoch() == epoch && first->wire_id != 0
                && first->nonce() != 0,
            "scheduler creates the complete identity before exposure");
    REQUIRE(no_type(submitter, Msg::USE_CS, 250),
            "UseCS is withheld until matching READY");
    if (first) {
        worker->send_msg(AssignReadyMsg(first->epoch(), first->wire_id,
                                        first->nonce() ^ UINT64_C(1)));
    }
    REQUIRE(no_type(submitter, Msg::USE_CS, 250),
            "stale READY cannot expose an assignment");
    if (first) {
        worker->send_msg(AssignReadyMsg(first->epoch(), first->wire_id,
                                        first->nonce()));
    }
    UseCSMsg *first_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(first && first_use && first_use->job_id == first->wire_id,
            "matching READY exposes the frozen UseCS projection");
    if (first) {
        worker->send_msg(AssignReadyMsg(first->epoch(), first->wire_id,
                                        first->nonce()));
    }
    REQUIRE(no_type(submitter, Msg::USE_CS, 250),
            "duplicate READY cannot expose a second UseCS");

    if (first_use) {
        submitter->send_msg(job_done_for(*first_use, 1,
                                         JobDoneMsg::FROM_SUBMITTER));
    }
    RevokeBeforeStartMsg *first_revoke = dynamic_cast<RevokeBeforeStartMsg *>(
        wait_type(worker, Msg::REVOKE_BEFORE_START, 3000));
    REQUIRE(first && first_revoke
                && first_revoke->epoch() == first->epoch()
                && first_revoke->wire_id == first->wire_id
                && first_revoke->nonce() == first->nonce(),
            "pre-start cancellation queues an ordered exact revoke");

    if (first_revoke) {
        worker->send_msg(RevokeResultMsg(first_revoke->epoch(),
                                         first_revoke->wire_id,
                                         first_revoke->nonce() ^ UINT64_C(2),
                                         RevokeResultMsg::Revoked));
    }
    ConfCSMsg *probe_conf = nullptr;
    MsgChannel *probe = login_host(port, "p49-probe", false, 0, &probe_conf);
    delete probe_conf;
    REQUIRE(probe && request_job(probe, 1002),
            "allocator-retention probe submitted before terminal");
    REQUIRE(no_type(worker, Msg::ASSIGN_PREPARE, 350),
            "stale terminal does not release the live reservation");
    delete probe;
    probe = nullptr;

    if (first_revoke) {
        worker->send_msg(JobBeginMsg(first_revoke->wire_id, 0));
        worker->send_msg(RevokeResultMsg(first_revoke->epoch(),
                                         first_revoke->wire_id,
                                         first_revoke->nonce(),
                                         RevokeResultMsg::Revoked));
    }
    ConfCSMsg *revoked_probe_conf = nullptr;
    MsgChannel *revoked_probe = login_host(
        port, "p49-revoked-probe", false, 0, &revoked_probe_conf);
    delete revoked_probe_conf;
    REQUIRE(revoked_probe && request_job(revoked_probe, 1003),
            "allocator probe submitted after contradictory Revoked");
    REQUIRE(no_type(worker, Msg::ASSIGN_PREPARE, 350),
            "Revoked after JobBegin cannot release claimed ownership");
    delete revoked_probe;
    if (first_revoke) {
        worker->send_msg(RevokeResultMsg(first_revoke->epoch(),
                                         first_revoke->wire_id,
                                         first_revoke->nonce(),
                                         RevokeResultMsg::ClaimedOrLater));
    }
    ConfCSMsg *claimed_probe_conf = nullptr;
    MsgChannel *claimed_probe = login_host(
        port, "p49-claimed-probe", false, 0, &claimed_probe_conf);
    delete claimed_probe_conf;
    REQUIRE(claimed_probe && request_job(claimed_probe, 1004),
            "allocator probe submitted after claim outcome");
    REQUIRE(no_type(worker, Msg::ASSIGN_PREPARE, 350),
            "claim outcome retains ownership until ordinary completion");
    delete claimed_probe;
    if (first_revoke && first_use) {
        worker->send_msg(job_done_for(*first_use, 0,
                                      JobDoneMsg::FROM_SERVER));
    }

    ConfCSMsg *reuse_conf = nullptr;
    MsgChannel *reuse = login_host(port, "p49-submit-b", false, 0, &reuse_conf);
    delete reuse_conf;
    REQUIRE(reuse && request_job(reuse, 1005),
            "assignment requested after ordinary completion");
    AssignPrepareMsg *second = wait_prepare(worker);
    REQUIRE(first && second && second->wire_id == first->wire_id
                && second->nonce() != first->nonce(),
            "ordinary completion permits reuse with a fresh immutable nonce");
    if (first) {
        worker->send_msg(AssignReadyMsg(first->epoch(), first->wire_id,
                                        first->nonce()));
        worker->send_msg(RevokeResultMsg(first->epoch(), first->wire_id,
                                         first->nonce(),
                                         RevokeResultMsg::Revoked));
    }
    REQUIRE(no_type(reuse, Msg::USE_CS, 250),
            "delayed old READY and terminal cannot affect reused wire id");
    if (second) {
        worker->send_msg(AssignReadyMsg(second->epoch(), second->wire_id,
                                        second->nonce()));
    }
    UseCSMsg *second_use = dynamic_cast<UseCSMsg *>(
        wait_type(reuse, Msg::USE_CS, 3000));
    REQUIRE(second && second_use && second_use->job_id == second->wire_id,
            "current full-triple READY wins after stale controls");

    delete reuse;
    reuse = nullptr;
    RevokeBeforeStartMsg *second_revoke = dynamic_cast<RevokeBeforeStartMsg *>(
        wait_type(worker, Msg::REVOKE_BEFORE_START, 3000));
    REQUIRE(second && second_revoke
                && second_revoke->nonce() == second->nonce(),
            "submitter teardown retains ownership and requests revoke");
    if (second) {
        worker->send_msg(AssignReadyMsg(second->epoch(), second->wire_id,
                                        second->nonce()));
    }
    if (second_revoke && second_use) {
        worker->send_msg(RevokeResultMsg(second_revoke->epoch(),
                                         second_revoke->wire_id,
                                         second_revoke->nonce(),
                                         RevokeResultMsg::ClaimedOrLater));
        worker->send_msg(JobBeginMsg(second_revoke->wire_id, 0));
        worker->send_msg(job_done_for(*second_use, 0,
                                      JobDoneMsg::FROM_SERVER));
    }
    usleep(100 * 1000);

    ConfCSMsg *ordered_conf = nullptr;
    MsgChannel *ordered = login_host(port, "p49-submit-c", false, 0,
                                     &ordered_conf);
    delete ordered_conf;
    REQUIRE(ordered && request_job(ordered, 1004),
            "post-CLAIMED_OR_LATER completion releases ownership exactly once");
    usleep(100 * 1000);
    delete ordered;
    ordered = nullptr;
    AssignPrepareMsg *third = wait_prepare(worker);
    RevokeBeforeStartMsg *third_revoke = dynamic_cast<RevokeBeforeStartMsg *>(
        wait_type(worker, Msg::REVOKE_BEFORE_START, 3000));
    REQUIRE(third && third_revoke
                && third->wire_id == third_revoke->wire_id
                && third->nonce() == third_revoke->nonce(),
            "PREPARE then REVOKE remain ordered when submitter vanishes early");
    if (third) {
        worker->send_msg(AssignReadyMsg(third->epoch(), third->wire_id,
                                        third->nonce()));
    }
    if (third_revoke) {
        worker->send_msg(RevokeResultMsg(third_revoke->epoch(),
                                         third_revoke->wire_id,
                                         third_revoke->nonce(),
                                         RevokeResultMsg::Revoked));
    }
    usleep(100 * 1000);

    ConfCSMsg *loss_conf = nullptr;
    MsgChannel *loss_submitter = login_host(port, "p49-submit-loss", false, 0,
                                            &loss_conf);
    delete loss_conf;
    REQUIRE(loss_submitter && request_job(loss_submitter, 1005),
            "assignment requested for worker-link loss");
    AssignPrepareMsg *loss_prepare = wait_prepare(worker);
    REQUIRE(loss_prepare != nullptr,
            "worker-link loss begins with an owned prepared assignment");
    delete worker;
    worker = nullptr;
    delete loss_submitter;
    usleep(150 * 1000);

    ConfCSMsg *replacement_conf = nullptr;
    MsgChannel *replacement = login_host(port, "p49-worker-replacement", true,
                                         worker_port, &replacement_conf);
    REQUIRE(replacement && replacement_conf
                && replacement_conf->fence_mode == ConfCSMsg::EnforcingCompat
                && replacement_conf->epoch() == epoch,
            "replacement worker joins the same scheduler epoch");
    delete replacement_conf;
    ConfCSMsg *after_loss_conf = nullptr;
    MsgChannel *after_loss = login_host(port, "p49-submit-after-loss", false, 0,
                                        &after_loss_conf);
    delete after_loss_conf;
    REQUIRE(after_loss && request_job(after_loss, 1006),
            "request submitted after worker-link terminal boundary");
    AssignPrepareMsg *after_loss_prepare = wait_prepare(replacement);
    REQUIRE(loss_prepare && after_loss_prepare
                && after_loss_prepare->wire_id == loss_prepare->wire_id
                && after_loss_prepare->nonce() != loss_prepare->nonce(),
            "worker-link loss releases id and replacement gets fresh identity");
    delete after_loss;
    delete replacement;

    delete after_loss_prepare;
    delete loss_prepare;
    delete third;
    delete third_revoke;
    delete second_use;
    delete second_revoke;
    delete second;
    delete first_use;
    delete first_revoke;
    delete first;
    delete submitter;
    delete worker;
    if (worker_listener >= 0) close(worker_listener);
    REQUIRE(stop_scheduler(scheduler),
            "EnforcingCompat scheduler stopped cleanly");
}

static void run_prepare_credit(const std::string &binary,
                               const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/prepare-credit-scheduler.log";
    pid_t scheduler = start_scheduler(
        binary, port, "enforcing-compat", log, 16, 1);
    REQUIRE(port != 0 && scheduler > 0,
            "prepare-credit scheduler process launched");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(port, "credit-worker", true, worker_port,
                                    &worker_conf, nullptr, 2);
    REQUIRE(worker && worker_conf
                && worker_conf->fence_mode == ConfCSMsg::EnforcingCompat,
            "credit worker negotiated enforcing compatibility mode");
    delete worker_conf;

    ConfCSMsg *first_conf = nullptr;
    MsgChannel *first_submitter = login_host(
        port, "credit-submit-a", false, 0, &first_conf);
    delete first_conf;
    REQUIRE(first_submitter && request_job(first_submitter, 4101),
            "first credit assignment requested");
    AssignPrepareMsg *first = wait_prepare(worker);
    REQUIRE(first != nullptr,
            "PREPARE is admitted while a worker slot is available");

    REQUIRE(first_submitter && request_job(first_submitter, 4102),
            "same submitter requests a second assignment before READY");
    REQUIRE(no_type(worker, Msg::ASSIGN_PREPARE, 350),
            "accepted PREPARE immediately consumes dispatch credit");

    if (first) {
        first_submitter->send_msg(JobDoneMsg(
            first->wire_id, 1, JobDoneMsg::FROM_SUBMITTER, 0,
            first->epoch(), first->nonce(), first->epoch(), 0));
    }
    RevokeBeforeStartMsg *revoke = dynamic_cast<RevokeBeforeStartMsg *>(
        wait_type(worker, Msg::REVOKE_BEFORE_START, 3000));
    REQUIRE(first && revoke && revoke->wire_id == first->wire_id
                && revoke->nonce() == first->nonce(),
            "pre-READY cancellation retains ownership and orders REVOKE");
    if (first) {
        worker->send_msg(AssignReadyMsg(first->epoch(), first->wire_id,
                                        first->nonce()));
    }
    REQUIRE(no_type(first_submitter, Msg::USE_CS, 300),
            "READY after cancellation cannot publish UseCS");
    if (revoke) {
        worker->send_msg(RevokeResultMsg(
            revoke->epoch(), revoke->wire_id, revoke->nonce(),
            RevokeResultMsg::Revoked));
    }
    AssignPrepareMsg *second = wait_prepare(worker);
    REQUIRE(second && (!first || second->wire_id != first->wire_id),
            "cancellation returns credit while the old id remains owned");
    REQUIRE(first && wait_file_contains(
                log, "END " + std::to_string(first->wire_id)
                     + " status=255", 2000),
            "accepted Revoked release emits collector terminal evidence");

    delete first_submitter;
    first_submitter = nullptr;
    RevokeBeforeStartMsg *second_revoke =
        dynamic_cast<RevokeBeforeStartMsg *>(
            wait_type(worker, Msg::REVOKE_BEFORE_START, 3000));
    if (second_revoke) {
        worker->send_msg(RevokeResultMsg(
            second_revoke->epoch(), second_revoke->wire_id,
            second_revoke->nonce(), RevokeResultMsg::Revoked));
    }
    delete second_revoke;
    delete second;
    delete revoke;
    delete first;
    delete first_submitter;
    delete worker;
    if (worker_listener >= 0) close(worker_listener);
    REQUIRE(stop_scheduler(scheduler),
            "prepare-credit scheduler stopped cleanly");
}

static void run_ready_nested_teardown(const std::string &binary,
                                      const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/ready-nested-teardown.log";
    pid_t scheduler = start_scheduler(binary, port, "enforcing-compat", log,
                                      16, 16, false, true);
    REQUIRE(port != 0 && scheduler > 0,
            "READY nested-teardown scheduler process launched");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(port, "nested-worker", true, worker_port,
                                    &worker_conf);
    delete worker_conf;
    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(port, "nested-submit", false, 0,
                                       &submitter_conf);
    delete submitter_conf;
    REQUIRE(worker && submitter && request_job(submitter, 4401),
            "nested-teardown assignment requested");
    AssignPrepareMsg *prepare = wait_prepare(worker);
    REQUIRE(prepare != nullptr,
            "nested-teardown assignment reaches PREPARE");
    if (prepare) {
        worker->send_msg(AssignReadyMsg(prepare->epoch(), prepare->wire_id,
                                        prepare->nonce()));
    }
    REQUIRE(wait_eof(worker, 8000),
            "failed ordered revoke tears down the same worker");
    REQUIRE(wait_eof(submitter, 1000),
            "forced UseCS failure tears down the submitter");

    ConfCSMsg *replacement_conf = nullptr;
    MsgChannel *replacement = login_host(port, "nested-replacement", true,
                                         worker_port, &replacement_conf);
    delete replacement_conf;
    ConfCSMsg *recovery_conf = nullptr;
    MsgChannel *recovery = login_host(port, "nested-recovery", false, 0,
                                      &recovery_conf);
    delete recovery_conf;
    REQUIRE(replacement && recovery && request_job(recovery, 4402),
            "scheduler remains live after nested worker deletion");
    AssignPrepareMsg *recovered = wait_prepare(replacement);
    REQUIRE(recovered != nullptr,
            "post-teardown assignment reaches the replacement worker");
    REQUIRE(file_contains(log,
                          "READY teardown deleted current worker; stopping drain"),
            "READY handler returns current-channel deletion to its drain caller");

    delete recovery;
    RevokeBeforeStartMsg *revoke = dynamic_cast<RevokeBeforeStartMsg *>(
        wait_type(replacement, Msg::REVOKE_BEFORE_START, 3000));
    if (revoke) {
        replacement->send_msg(RevokeResultMsg(
            revoke->epoch(), revoke->wire_id, revoke->nonce(),
            RevokeResultMsg::Revoked));
    }
    delete revoke;
    delete recovered;
    delete replacement;
    delete submitter;
    delete worker;
    delete prepare;
    if (worker_listener >= 0) close(worker_listener);
    REQUIRE(stop_scheduler(scheduler),
            "READY nested-teardown scheduler stopped cleanly");
}

static void run_advisory(const std::string &binary,
                         const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/advisory-scheduler.log";
    pid_t scheduler = start_scheduler(binary, port, "advisory", log, 16, 16);
    REQUIRE(port != 0 && scheduler > 0, "Advisory scheduler launched");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(port, "advisory-worker", true,
                                    worker_port, &worker_conf);
    REQUIRE(worker && worker_conf
                && worker_conf->fence_mode == ConfCSMsg::Advisory
                && worker_conf->epoch() != 0,
            "Advisory policy is immutable for the scheduler epoch");
    delete worker_conf;

    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(port, "advisory-submit", false, 0,
                                       &submitter_conf);
    delete submitter_conf;
    REQUIRE(submitter && request_job(submitter, 4201),
            "Advisory assignment requested");
    AssignPrepareMsg *prepare = wait_prepare(worker);
    UseCSMsg *use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(prepare && use && use->job_id == prepare->wire_id,
            "Advisory sends PREPARE without READY-gating UseCS");
    if (use) {
        worker->send_msg(JobBeginMsg(use->job_id, 0));
        worker->send_msg(job_done_for(*use, 0, JobDoneMsg::FROM_SERVER));
    }
    delete use;
    delete prepare;
    delete submitter;
    delete worker;
    if (worker_listener >= 0) close(worker_listener);
    REQUIRE(stop_scheduler(scheduler), "Advisory scheduler stopped cleanly");
}

static void run_prepare_backlog(const std::string &binary,
                                const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/prepare-backlog-scheduler.log";
    pid_t scheduler = start_scheduler(binary, port, "advisory", log,
                                      2048, 1024, true);
    REQUIRE(port != 0 && scheduler > 0,
            "deferred-PREPARE scheduler launched");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(port, "backlog-worker", true, worker_port,
                                    &worker_conf, nullptr, 1024, 256);
    delete worker_conf;

    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(port, "backlog-submit", false, 0,
                                       &submitter_conf);
    delete submitter_conf;
    REQUIRE(worker && submitter && request_job(submitter, 4301, 1024),
            "large Advisory batch admitted against a non-draining worker");

    unsigned int use_count = 0;
    const auto deadline = Clock::now() + std::chrono::milliseconds(1500);
    while (Clock::now() < deadline) {
        Msg *msg = next_message(submitter, 50);
        if (msg) {
            if (*msg == Msg::USE_CS) ++use_count;
            delete msg;
        }
    }
    REQUIRE(use_count > 0 && use_count < 1024,
            "new PREPARE admission stops while worker output is deferred");

    delete submitter;
    delete worker;
    if (worker_listener >= 0) close(worker_listener);
    REQUIRE(stop_scheduler(scheduler),
            "deferred-PREPARE scheduler stopped cleanly");
    REQUIRE(file_contains(log, "has deferred assignment-control output"),
            "scheduler records the deferred-PREPARE eligibility boundary");
}

static void run_strict_nonce(const std::string &binary,
                             const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/strict-nonce.log";
    pid_t scheduler = start_scheduler(binary, port, "strict-nonce", log);
    REQUIRE(port != 0 && scheduler > 0,
            "explicit STRICT_NONCE scheduler launched");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    int cache_port = 0;
    int cache_sentinel = bind_port(0, &cache_port);
    if (cache_sentinel >= 0) listen(cache_sentinel, 4);
    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(port, "strict-worker", true, worker_port,
                                    &worker_conf, nullptr, 1, 0,
                                    static_cast<uint32_t>(cache_port),
                                    CACHE_WIRE_REVISION,
                                    CACHE_PROFILE_ZSTD_TU);
    REQUIRE(worker && worker_conf
                && worker_conf->fence_mode == ConfCSMsg::StrictNonce
                && worker_conf->epoch() != 0,
            "P50 worker receives explicitly selected STRICT_NONCE");
    const uint64_t epoch = worker_conf ? worker_conf->epoch() : 0;
    delete worker_conf;

    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(port, "strict-submit", false, 0,
                                       &submitter_conf);
    REQUIRE(submitter && submitter_conf
                && submitter_conf->fence_mode == ConfCSMsg::StrictNonce
                && submitter_conf->epoch() == epoch,
            "P50 submitter receives the same explicit strict epoch");
    delete submitter_conf;
    REQUIRE(submitter && request_job(submitter, 4901),
            "all-P50 strict assignment requested");
    Msg *prepare_wire = wait_type(worker, Msg::ASSIGN_PREPARE, 3000);
    AssignPrepareMsg *prepare = dynamic_cast<AssignPrepareMsg *>(prepare_wire);
    REQUIRE(prepare && prepare->epoch() == epoch && prepare->wire_id != 0
                && prepare->nonce() != 0,
            "strict scheduler prepares a complete nonzero identity");
    REQUIRE(no_type(submitter, Msg::USE_CS, 300),
            "strict scheduler does not expose UseCS before READY");
    if (prepare) {
        worker->send_msg(AssignReadyMsg(
            prepare->epoch(), prepare->wire_id, prepare->nonce()));
    }
    UseCSMsg *use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(use && prepare && use->job_id == prepare->wire_id
                && use->assignmentEpoch() == prepare->epoch()
                && use->assignmentNonce() == prepare->nonce(),
            "strict UseCS carries the already-authorized full tuple");
    if (use) {
        worker->send_msg(JobBeginMsg(use->job_id, 0));
        worker->send_msg(job_done_for(*use, 0, JobDoneMsg::FROM_SERVER));
    }
    delete use;
    delete prepare_wire;
    delete submitter;
    delete worker;
    if (worker_listener >= 0) close(worker_listener);
    if (cache_sentinel >= 0) close(cache_sentinel);

    /* Link versions only exclude an incompatible path; they never choose or
       weaken the operator's global mode.  The old worker receives a LEGACY
       projection but cannot receive strict remote work. */
    int proxy_port = 0;
    pid_t proxy = start_p48_proxy(port, &proxy_port);
    MsgChannel *p48_channel = connect_scheduler(proxy_port);
    int old_worker_port = 0;
    int old_worker_listener = bind_port(0, &old_worker_port);
    if (old_worker_listener >= 0) listen(old_worker_listener, 16);
    worker_conf = nullptr;
    MsgChannel *old_worker = login_host(
        port, "strict-p48-worker", true, old_worker_port, &worker_conf,
        p48_channel);
    REQUIRE(old_worker && worker_conf
                && worker_conf->fence_mode == ConfCSMsg::Legacy
                && worker_conf->epoch() == 0,
            "strict scheduler projects Legacy configuration to P48 worker");
    delete worker_conf;
    submitter_conf = nullptr;
    MsgChannel *modern_submitter = login_host(
        port, "strict-modern-submit", false, 0, &submitter_conf);
    REQUIRE(modern_submitter && submitter_conf
                && submitter_conf->fence_mode == ConfCSMsg::StrictNonce,
            "global operator mode remains strict for compatible peer");
    delete submitter_conf;
    REQUIRE(modern_submitter && request_job(modern_submitter, 4902),
            "mixed strict remote assignment requested");
    REQUIRE(no_type(modern_submitter, Msg::USE_CS, 600)
                && no_type(modern_submitter, Msg::NO_CS, 200),
            "mixed strict remote path is refused rather than downgraded");
    REQUIRE(no_type(old_worker, Msg::ASSIGN_PREPARE, 300),
            "P48 worker receives no strict PREPARE");

    delete modern_submitter;
    delete old_worker;
    if (old_worker_listener >= 0) close(old_worker_listener);
    REQUIRE(stop_scheduler(proxy), "protocol-48 strict relay stopped cleanly");
    REQUIRE(stop_scheduler(scheduler), "STRICT_NONCE scheduler stopped cleanly");
}

static void run_pre_exposure_worker_loss_redispatch(
    const std::string &binary, const std::string &directory)
{
    int port = 0;
    std::string log;
    pid_t scheduler = start_cache_routing_scheduler_retry(binary, directory, &port, &log);
    REQUIRE(port != 0 && scheduler > 0,
            "pre-exposure worker-loss scheduler launched");
    if (port == 0 || scheduler <= 0)
        return;

    int worker_a_port = 0;
    int worker_a_listener = bind_port(0, &worker_a_port);
    if (worker_a_listener >= 0) listen(worker_a_listener, 16);
    int cache_a_port = 0;
    int cache_a_sentinel = bind_port(0, &cache_a_port);
    if (cache_a_sentinel >= 0) listen(cache_a_sentinel, 4);
    int worker_b_port = 0;
    int worker_b_listener = bind_port(0, &worker_b_port);
    if (worker_b_listener >= 0) listen(worker_b_listener, 16);
    int cache_b_port = 0;
    int cache_b_sentinel = bind_port(0, &cache_b_port);
    if (cache_b_sentinel >= 0) listen(cache_b_sentinel, 4);
    int local_port = 0;
    int local_listener = bind_port(0, &local_port);
    if (local_listener >= 0) listen(local_listener, 16);
    int local_cache_port = 0;
    int local_cache_sentinel = bind_port(0, &local_cache_port);
    if (local_cache_sentinel >= 0) listen(local_cache_sentinel, 4);

    ConfCSMsg *configuration = nullptr;
    MsgChannel *worker_a = login_host(
        port, "pre-exposure-a", true, worker_a_port, &configuration,
        nullptr, 1, 0, static_cast<uint32_t>(cache_a_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    delete configuration;
    configuration = nullptr;
    MsgChannel *worker_b = login_host(
        port, "pre-exposure-b", true, worker_b_port, &configuration,
        nullptr, 1, 0, static_cast<uint32_t>(cache_b_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    delete configuration;
    configuration = nullptr;
    MsgChannel *occupier = login_host(
        port, "pre-exposure-occupier", false, 0, &configuration);
    delete configuration;
    configuration = nullptr;
    /* The submitting C is deliberately also a one-slot compile server.  A
       redispatch that escapes either the preferred-host shortcut or ordinary
       eligibility filter would otherwise turn into a local NoCS decision. */
    MsgChannel *submitter = login_host(
        port, "pre-exposure-submit", true, local_port, &configuration,
        nullptr, 1, 0, static_cast<uint32_t>(local_cache_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    delete configuration;
    REQUIRE(worker_a && worker_b && occupier && submitter,
            "two strict workers, one submitter, and a mixed-role C joined");

    REQUIRE(occupier && request_job_preferring(
                occupier, 6250, "pre-exposure-b"),
            "setup request occupies B's only real slot");
    AssignPrepareMsg *busy_prepare = wait_prepare(worker_b);
    if (busy_prepare) {
        worker_b->send_msg(AssignReadyMsg(
            busy_prepare->epoch(), busy_prepare->wire_id,
            busy_prepare->nonce()));
    }
    UseCSMsg *busy_use = dynamic_cast<UseCSMsg *>(
        wait_type(occupier, Msg::USE_CS, 3000));
    REQUIRE(busy_prepare && busy_use &&
                busy_use->port == static_cast<uint32_t>(worker_b_port),
            "setup request holds B while the target selects A");
    if (busy_use)
        worker_b->send_msg(JobBeginMsg(busy_use->job_id, 0));

    REQUIRE(submitter && request_job_preferring(
                submitter, 6251, "127.0.0.1"),
            "target strict request takes first matching A while B is full");
    AssignPrepareMsg *first_prepare = wait_prepare(worker_a);
    REQUIRE(first_prepare && first_prepare->wire_id != 0 &&
                first_prepare->nonce() != 0,
            "target request is prepared on A");
    Msg *premature_decision = next_message(submitter, 300);
    REQUIRE(!premature_decision,
            "target has no assignment decision before A's READY");
    delete premature_decision;
    const uint32_t target_job = first_prepare ? first_prepare->wire_id : 0;
    const uint64_t first_epoch = first_prepare ? first_prepare->epoch() : 0;
    const uint64_t first_nonce = first_prepare ? first_prepare->nonce() : 0;

    delete worker_a;
    worker_a = nullptr;
    REQUIRE(wait_file_contains(log, "remove daemon pre-exposure-a", 3000),
            "scheduler observes A loss before assignment exposure");
    premature_decision = next_message(submitter, 500);
    REQUIRE(!premature_decision,
            "preferred-host redispatch cannot fall through to its local C");
    delete premature_decision;

    if (busy_use)
        worker_b->send_msg(job_done_for(
            *busy_use, 0, JobDoneMsg::FROM_SERVER));
    AssignPrepareMsg *second_prepare = wait_prepare(worker_b);
    REQUIRE(second_prepare && second_prepare->wire_id == target_job &&
                second_prepare->epoch() == first_epoch &&
                second_prepare->nonce() != first_nonce,
            "worker loss redispatches the same request to B with a fresh nonce");

    if (first_prepare) {
        worker_b->send_msg(AssignReadyMsg(
            first_prepare->epoch(), first_prepare->wire_id,
            first_prepare->nonce()));
    }
    REQUIRE(no_type(submitter, Msg::USE_CS, 300),
            "stale READY for A's retired nonce cannot expose the request");
    if (second_prepare) {
        worker_b->send_msg(AssignReadyMsg(
            second_prepare->epoch(), second_prepare->wire_id,
            second_prepare->nonce()));
    }
    UseCSMsg *target_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(target_use && second_prepare &&
                target_use->job_id == second_prepare->wire_id &&
                target_use->assignmentNonce() == second_prepare->nonce() &&
                target_use->port == static_cast<uint32_t>(worker_b_port),
            "only B's fresh prepared assignment is exposed");
    if (target_use) {
        worker_b->send_msg(JobBeginMsg(target_use->job_id, 0));
        worker_b->send_msg(job_done_for(
            *target_use, 0, JobDoneMsg::FROM_SERVER));
    }
    const std::string target_terminal = target_use
        ? "END " + std::to_string(target_use->job_id) + " status=0"
        : std::string();
    REQUIRE(target_use && wait_file_contains(log, target_terminal, 3000) &&
                file_occurrence_count(log, target_terminal) == 1,
            "redispatched request reaches exactly one successful terminal");
    REQUIRE(file_contains(log, "redispatch unexposed assignment"),
            "scheduler records the pre-exposure redispatch transition");

    REQUIRE(occupier && request_job_preferring(
                occupier, 6252, "pre-exposure-submit"),
            "first setup job occupies mixed-role C's real slot");
    AssignPrepareMsg *local_busy1_prepare = wait_prepare(submitter);
    if (local_busy1_prepare) {
        submitter->send_msg(AssignReadyMsg(
            local_busy1_prepare->epoch(), local_busy1_prepare->wire_id,
            local_busy1_prepare->nonce()));
    }
    UseCSMsg *local_busy1 = dynamic_cast<UseCSMsg *>(
        wait_type(occupier, Msg::USE_CS, 3000));
    REQUIRE(local_busy1_prepare && local_busy1 &&
                local_busy1->port == static_cast<uint32_t>(local_port),
            "mixed-role C's real slot is held");
    if (local_busy1)
        submitter->send_msg(JobBeginMsg(local_busy1->job_id, 0));

    REQUIRE(occupier && request_job_preferring(
                occupier, 6253, "pre-exposure-submit"),
            "second setup job occupies mixed-role C's preload allowance");
    AssignPrepareMsg *local_busy2_prepare = wait_prepare(submitter);
    if (local_busy2_prepare) {
        submitter->send_msg(AssignReadyMsg(
            local_busy2_prepare->epoch(), local_busy2_prepare->wire_id,
            local_busy2_prepare->nonce()));
    }
    UseCSMsg *local_busy2 = dynamic_cast<UseCSMsg *>(
        wait_type(occupier, Msg::USE_CS, 3000));
    REQUIRE(local_busy2_prepare && local_busy2 &&
                local_busy2->port == static_cast<uint32_t>(local_port),
            "mixed-role C is saturated at maxJobs plus preload");
    if (local_busy2)
        submitter->send_msg(JobBeginMsg(local_busy2->job_id, 0));

    REQUIRE(submitter && request_cache_job(submitter, 6254),
            "second target request selects the sole surviving B");
    AssignPrepareMsg *sole_prepare = wait_prepare(worker_b);
    REQUIRE(sole_prepare && sole_prepare->wire_id != 0 &&
                sole_prepare->nonce() != 0,
            "second target is prepared before sole-worker loss");
    REQUIRE(no_type(submitter, Msg::USE_CS, 300),
            "second target remains unexposed before B's READY");
    const uint32_t sole_job = sole_prepare ? sole_prepare->wire_id : 0;
    const uint64_t sole_epoch = sole_prepare ? sole_prepare->epoch() : 0;
    const uint64_t sole_nonce = sole_prepare ? sole_prepare->nonce() : 0;

    delete worker_b;
    worker_b = nullptr;
    REQUIRE(wait_file_contains(log, "remove daemon pre-exposure-b", 3000),
            "scheduler observes sole B loss before assignment exposure");
    if (local_busy1)
        submitter->send_msg(job_done_for(
            *local_busy1, 0, JobDoneMsg::FROM_SERVER));
    if (local_busy2)
        submitter->send_msg(job_done_for(
            *local_busy2, 0, JobDoneMsg::FROM_SERVER));
    premature_decision = next_message(submitter, 700);
    REQUIRE(!premature_decision,
            "ordinary redispatch filter rejects newly free local C");
    delete premature_decision;

    int worker_c_port = 0;
    int worker_c_listener = bind_port(0, &worker_c_port);
    if (worker_c_listener >= 0) listen(worker_c_listener, 16);
    int cache_c_port = 0;
    int cache_c_sentinel = bind_port(0, &cache_c_port);
    if (cache_c_sentinel >= 0) listen(cache_c_sentinel, 4);
    configuration = nullptr;
    MsgChannel *worker_c = login_host(
        port, "pre-exposure-c", true, worker_c_port, &configuration,
        nullptr, 1, 0, static_cast<uint32_t>(cache_c_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    delete configuration;
    AssignPrepareMsg *replacement_prepare = wait_prepare(worker_c);
    REQUIRE(worker_c && replacement_prepare &&
                replacement_prepare->wire_id == sole_job &&
                replacement_prepare->epoch() == sole_epoch &&
                replacement_prepare->nonce() != sole_nonce,
            "a later replacement F receives the same request with a fresh nonce");
    if (replacement_prepare) {
        worker_c->send_msg(AssignReadyMsg(
            replacement_prepare->epoch(), replacement_prepare->wire_id,
            replacement_prepare->nonce()));
    }
    UseCSMsg *replacement_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(replacement_use && replacement_prepare &&
                replacement_use->job_id == replacement_prepare->wire_id &&
                replacement_use->assignmentNonce() ==
                    replacement_prepare->nonce() &&
                replacement_use->port == static_cast<uint32_t>(worker_c_port),
            "replacement F is exposed without an intervening local decision");
    if (replacement_use) {
        worker_c->send_msg(JobBeginMsg(replacement_use->job_id, 0));
        worker_c->send_msg(job_done_for(
            *replacement_use, 0, JobDoneMsg::FROM_SERVER));
    }
    const std::string replacement_terminal = replacement_use
        ? "END " + std::to_string(replacement_use->job_id) + " status=0"
        : std::string();
    REQUIRE(replacement_use && wait_file_contains(
                log, replacement_terminal, 3000) &&
                file_occurrence_count(log, replacement_terminal) == 1,
            "sole-worker-loss redispatch reaches exactly one successful terminal");

    REQUIRE(submitter && request_job_preferring(
                submitter, 6255, "pre-exposure-c"),
            "negative control assigns a fresh request to replacement C");
    AssignPrepareMsg *exposed_prepare = wait_prepare(worker_c);
    if (exposed_prepare) {
        worker_c->send_msg(AssignReadyMsg(
            exposed_prepare->epoch(), exposed_prepare->wire_id,
            exposed_prepare->nonce()));
    }
    UseCSMsg *exposed_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(exposed_prepare && exposed_use &&
                exposed_use->job_id == exposed_prepare->wire_id &&
                exposed_use->assignmentNonce() == exposed_prepare->nonce(),
            "negative-control assignment is exposed before worker loss");
    const uint32_t exposed_job = exposed_use ? exposed_use->job_id : 0;

    delete worker_c;
    worker_c = nullptr;
    REQUIRE(wait_file_contains(log, "remove daemon pre-exposure-c", 3000),
            "scheduler observes exposed worker loss");

    int worker_d_port = 0;
    int worker_d_listener = bind_port(0, &worker_d_port);
    if (worker_d_listener >= 0) listen(worker_d_listener, 16);
    int cache_d_port = 0;
    int cache_d_sentinel = bind_port(0, &cache_d_port);
    if (cache_d_sentinel >= 0) listen(cache_d_sentinel, 4);
    configuration = nullptr;
    MsgChannel *worker_d = login_host(
        port, "pre-exposure-d", true, worker_d_port, &configuration,
        nullptr, 1, 0, static_cast<uint32_t>(cache_d_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    delete configuration;
    AssignPrepareMsg *duplicate_prepare = wait_prepare(worker_d);
    REQUIRE(worker_d && !duplicate_prepare && exposed_job != 0 &&
                file_occurrence_count(
                    log, "redispatch unexposed assignment " +
                         std::to_string(exposed_job)) == 0,
            "already-exposed assignment is never scheduler-redispatched");

    delete duplicate_prepare;
    delete exposed_use;
    delete exposed_prepare;
    delete replacement_use;
    delete replacement_prepare;
    delete sole_prepare;
    delete target_use;
    delete second_prepare;
    delete first_prepare;
    delete local_busy2;
    delete local_busy2_prepare;
    delete local_busy1;
    delete local_busy1_prepare;
    delete busy_use;
    delete busy_prepare;
    delete submitter;
    delete occupier;
    delete worker_d;
    if (worker_a_listener >= 0) close(worker_a_listener);
    if (worker_b_listener >= 0) close(worker_b_listener);
    if (worker_c_listener >= 0) close(worker_c_listener);
    if (worker_d_listener >= 0) close(worker_d_listener);
    if (local_listener >= 0) close(local_listener);
    if (local_cache_sentinel >= 0) close(local_cache_sentinel);
    if (cache_a_sentinel >= 0) close(cache_a_sentinel);
    if (cache_b_sentinel >= 0) close(cache_b_sentinel);
    if (cache_c_sentinel >= 0) close(cache_c_sentinel);
    if (cache_d_sentinel >= 0) close(cache_d_sentinel);
    REQUIRE(stop_scheduler(scheduler),
            "pre-exposure worker-loss scheduler stopped cleanly");
}

static void run_disabled(const std::string &binary, const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/disabled-scheduler.log";
    pid_t scheduler = start_scheduler(binary, port, nullptr, log);
    REQUIRE(port != 0 && scheduler > 0, "default-disabled scheduler launched");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(port, "legacy-worker", true, worker_port,
                                    &worker_conf);
    REQUIRE(worker && worker_conf && worker_conf->epoch() != 0
                && worker_conf->fence_mode == ConfCSMsg::Legacy,
            "default P49 inheritance discriminator is Legacy");
    delete worker_conf;

    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(port, "legacy-submit", false, 0,
                                       &submitter_conf);
    delete submitter_conf;
    REQUIRE(submitter && request_job(submitter, 2001),
            "default-disabled assignment requested");
    UseCSMsg *use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(use != nullptr, "default-disabled path sends legacy UseCS directly");
    REQUIRE(no_type(worker, Msg::ASSIGN_PREPARE, 300),
            "default-disabled path sends no PREPARE and waits for no READY");

    if (use) {
        worker->send_msg(JobBeginMsg(use->job_id, 0));
        worker->send_msg(job_done_for(*use, 0, JobDoneMsg::FROM_SERVER));
    }
    delete use;
    delete submitter;
    delete worker;
    if (worker_listener >= 0) close(worker_listener);
    REQUIRE(stop_scheduler(scheduler), "default-disabled scheduler stopped cleanly");
}

static void run_cache_advertisement(const std::string &binary,
                                    const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/cache-advertisement-scheduler.log";
    pid_t scheduler = start_scheduler(binary, port, nullptr, log);
    REQUIRE(port != 0 && scheduler > 0,
            "cache-advertisement scheduler process launched");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    int cache_port = 0;
    int cache_sentinel = bind_port(0, &cache_port);
    if (cache_sentinel >= 0) listen(cache_sentinel, 4);

    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(
        port, "cache-ad-worker", true, worker_port, &worker_conf,
        nullptr, 1, 0, static_cast<uint32_t>(cache_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU);
    REQUIRE(worker && worker_conf && cache_sentinel >= 0,
            "fake ready F logs in with a production cache advertisement");
    delete worker_conf;

    const std::string endpoint = "cache=127.0.0.1:"
        + std::to_string(cache_port)
        + " cache_wire=v1 cache_protocol=1 cache_profiles=zstd_tu";
    std::string list = control_text(port, "listcs");
    REQUIRE(list.find("cache-ad-worker") != std::string::npos
                && list.find(endpoint) != std::string::npos,
            "scheduler retains and exposes the exact endpoint metadata");
    REQUIRE(file_contains(log, endpoint),
            "scheduler login trace exposes qualified CacheWire metadata");

    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(port, "cache-ad-submit", false, 0,
                                       &submitter_conf);
    delete submitter_conf;
    REQUIRE(submitter && request_job(submitter, 5001),
            "ordinary assignment requested with advertised cache unavailable");
    UseCSMsg *use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(use && use->port == static_cast<uint32_t>(worker_port),
            "advertisement leaves ordinary UseCS selection unchanged");
    /* S2: this scheduler runs in the DEFAULT (Legacy) fence mode, so this
       job's assignment identity is never set (epoch/nonce stay 0 -- see
       ASSIGNMENT_LEGACY in scheduler.cpp).  BigOracle steer: a present
       cache triple additionally requires that complete nonzero identity,
       so even a perfectly valid, faithfully-retained worker snapshot must
       still project wholly absent here.  The genuinely-bound case (a mode
       that DOES assign identity) is covered by
       run_cache_handoff_identity_bound below. */
    REQUIRE(use && !use->hasCacheAdvertisement()
                && use->cache_protocol == 0 && use->cache_profile_mask == 0,
            "S2: LEGACY fence mode projects a wholly-absent cache-handoff "
            "tail even when the selected F's retained snapshot is valid");
    pollfd cache_probe { cache_sentinel, POLLIN, 0 };
    REQUIRE(poll(&cache_probe, 1, 300) == 0,
            "scheduler and submitter never connect to the inert cache endpoint");
    if (use) {
        worker->send_msg(JobBeginMsg(use->job_id, 0));
        worker->send_msg(job_done_for(*use, 0, JobDoneMsg::FROM_SERVER));
    }
    delete use;

    LoginMsg absent(static_cast<uint32_t>(worker_port), "cache-ad-worker",
                    "x86_64", 0);
    absent.envs.push_back(std::make_pair(std::string("x86_64"),
                                         std::string("p49-test-env")));
    absent.max_kids = 1;
    absent.chroot_possible = true;
    absent.setCacheAdvertisement(0, 0, 0);
    REQUIRE(worker && worker->send_msg(absent),
            "fake F replaces its advertisement with canonical absence");
    delete wait_type(worker, Msg::CS_CONF, 3000);
    list = control_text(port, "listcs");
    const size_t node = list.find("cache-ad-worker");
    const size_t line_end = node == std::string::npos
        ? std::string::npos : list.find('\n', node);
    const std::string worker_line = node == std::string::npos
        ? std::string() : list.substr(node, line_end - node);
    REQUIRE(worker_line.find("cache=off") != std::string::npos
                && worker_line.find("cache_wire=") == std::string::npos,
            "replacement Login atomically publishes cache absence");
    /* The identity-bound staleness/tracking proof (does a later UseCS ever
       reuse a value cached from an earlier Login rather than the CURRENT
       retained snapshot?) needs a mode that actually assigns identity --
       under this function's Legacy mode every UseCS projects absent
       regardless of the worker's snapshot, so a before/after comparison
       here would be confounded and prove nothing.  See
       run_cache_handoff_identity_bound below. */

    delete submitter;
    delete worker;
    worker = nullptr;
    usleep(150 * 1000);
    list = control_text(port, "listcs");
    REQUIRE(list.find("cache-ad-worker") == std::string::npos,
            "worker disconnect removes retained endpoint metadata");

    if (cache_sentinel >= 0) close(cache_sentinel);
    if (worker_listener >= 0) close(worker_listener);
    REQUIRE(stop_scheduler(scheduler),
            "cache-advertisement scheduler stopped cleanly");
}

/* BigOracle steer: the Legacy-mode run_cache_advertisement above cannot
   prove a present cache triple projects faithfully -- under Legacy every
   UseCS is absent regardless of the worker's snapshot (see the assertion
   there).  Advisory mode DOES assign a complete identity before dispatch
   (immediately, without READY-gating -- see run_advisory), so it is used
   here to prove, with two DISTINCT candidate F's:
     (b) a present triple requires the complete identity AND actually
         happens once that identity exists;
     (c) post-selection binding -- forcing selection of F_B carries B's
         snapshot, never A's, distractor-A notwithstanding;
     (c cont'd/d) swapping which port each F advertises never changes WHICH
         worker gets selected (advertisement content is not selection
         input), and the handoff tail tracks F_B's CURRENT snapshot after
         the swap, not a value cached from its first Login. */
static void run_cache_handoff_identity_bound(const std::string &binary,
                                             const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/cache-handoff-identity-bound.log";
    pid_t scheduler = start_scheduler(binary, port, "advisory", log, 16, 16);
    REQUIRE(port != 0 && scheduler > 0,
            "identity-bound cache-handoff scheduler process launched");

    int worker_a_port = 0;
    int worker_a_listener = bind_port(0, &worker_a_port);
    if (worker_a_listener >= 0) listen(worker_a_listener, 16);
    int cache_a_port = 0;
    int cache_a_sentinel = bind_port(0, &cache_a_port);
    if (cache_a_sentinel >= 0) listen(cache_a_sentinel, 4);

    int worker_b_port = 0;
    int worker_b_listener = bind_port(0, &worker_b_port);
    if (worker_b_listener >= 0) listen(worker_b_listener, 16);
    int cache_b_port = 0;
    int cache_b_sentinel = bind_port(0, &cache_b_port);
    if (cache_b_sentinel >= 0) listen(cache_b_sentinel, 4);

    ConfCSMsg *a_conf = nullptr;
    MsgChannel *worker_a = login_host(
        port, "cache-bound-a", true, worker_a_port, &a_conf,
        nullptr, 1, 0, static_cast<uint32_t>(cache_a_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU);
    REQUIRE(worker_a && a_conf && a_conf->fence_mode == ConfCSMsg::Advisory,
            "distractor candidate F_A logs in with its own valid cache advertisement");
    delete a_conf;

    ConfCSMsg *b_conf = nullptr;
    MsgChannel *worker_b = login_host(
        port, "cache-bound-b", true, worker_b_port, &b_conf,
        nullptr, 1, 0, static_cast<uint32_t>(cache_b_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU);
    REQUIRE(worker_b && b_conf && b_conf->fence_mode == ConfCSMsg::Advisory,
            "candidate F_B logs in with its own DIFFERENT valid cache advertisement");
    delete b_conf;

    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(port, "cache-bound-submit", false, 0,
                                       &submitter_conf);
    delete submitter_conf;

    REQUIRE(submitter && request_job_preferring(submitter, 6001, "cache-bound-b"),
            "assignment forced to F_B requested");
    delete wait_type(worker_b, Msg::ASSIGN_PREPARE, 3000);
    UseCSMsg *use_b = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(use_b && use_b->port == static_cast<uint32_t>(worker_b_port),
            "S2: forced selection actually picked F_B, not F_A");
    REQUIRE(use_b && use_b->hasAssignmentIdentity(),
            "S2: Advisory mode assigns a complete identity before dispatch");
    REQUIRE(use_b && use_b->cache_endpoint_port == static_cast<uint32_t>(cache_b_port)
                && use_b->cache_protocol == CACHE_WIRE_REVISION
                && use_b->cache_profile_mask == CACHE_PROFILE_ZSTD_TU,
            "S2: with a complete identity, the handoff tail faithfully "
            "carries the SELECTED F's (B's) snapshot, never A's");
    if (use_b) {
        worker_b->send_msg(JobBeginMsg(use_b->job_id, 0));
        worker_b->send_msg(job_done_for(*use_b, 0, JobDoneMsg::FROM_SERVER));
    }
    delete use_b;

    LoginMsg a_swapped(static_cast<uint32_t>(worker_a_port), "cache-bound-a",
                       "x86_64", 0);
    a_swapped.envs.push_back(std::make_pair(std::string("x86_64"),
                                            std::string("p49-test-env")));
    a_swapped.max_kids = 1;
    a_swapped.chroot_possible = true;
    a_swapped.setCacheAdvertisement(static_cast<uint32_t>(cache_b_port),
                                    CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU);
    REQUIRE(worker_a && worker_a->send_msg(a_swapped),
            "F_A relogins advertising F_B's old cache port");
    delete wait_type(worker_a, Msg::CS_CONF, 3000);

    LoginMsg b_swapped(static_cast<uint32_t>(worker_b_port), "cache-bound-b",
                       "x86_64", 0);
    b_swapped.envs.push_back(std::make_pair(std::string("x86_64"),
                                            std::string("p49-test-env")));
    b_swapped.max_kids = 1;
    b_swapped.chroot_possible = true;
    b_swapped.setCacheAdvertisement(static_cast<uint32_t>(cache_a_port),
                                    CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU);
    REQUIRE(worker_b && worker_b->send_msg(b_swapped),
            "F_B relogins advertising F_A's old cache port");
    delete wait_type(worker_b, Msg::CS_CONF, 3000);

    REQUIRE(submitter && request_job_preferring(submitter, 6002, "cache-bound-b"),
            "second assignment forced to F_B requested after the swap");
    delete wait_type(worker_b, Msg::ASSIGN_PREPARE, 3000);
    UseCSMsg *use_b2 = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(use_b2 && use_b2->port == static_cast<uint32_t>(worker_b_port),
            "S2: swapping advertisements never changes WHICH worker is selected");
    REQUIRE(use_b2 && use_b2->cache_endpoint_port == static_cast<uint32_t>(cache_a_port)
                && use_b2->cache_endpoint_port != static_cast<uint32_t>(cache_b_port),
            "S2: the handoff tail tracks B's CURRENT (just-swapped) snapshot, "
            "not a value cached from B's first Login");
    if (use_b2) {
        worker_b->send_msg(JobBeginMsg(use_b2->job_id, 0));
        worker_b->send_msg(job_done_for(*use_b2, 0, JobDoneMsg::FROM_SERVER));
    }
    delete use_b2;

    delete submitter;
    delete worker_a;
    delete worker_b;
    if (worker_a_listener >= 0) close(worker_a_listener);
    if (worker_b_listener >= 0) close(worker_b_listener);
    if (cache_a_sentinel >= 0) close(cache_a_sentinel);
    if (cache_b_sentinel >= 0) close(cache_b_sentinel);
    REQUIRE(stop_scheduler(scheduler),
            "identity-bound cache-handoff scheduler stopped cleanly");
}

static void run_cache_routing_preference(const std::string &binary,
                                         const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/cache-routing-preference.log";
    pid_t scheduler = start_cache_routing_scheduler(binary, port, log);
    REQUIRE(port != 0 && scheduler > 0,
            "cache-routing mixed-pool scheduler process launched");

    int worker_a_port = 0;
    int worker_a_listener = bind_port(0, &worker_a_port);
    if (worker_a_listener >= 0) listen(worker_a_listener, 16);
    int cache_a_port = 0;
    int cache_a_sentinel = bind_port(0, &cache_a_port);
    if (cache_a_sentinel >= 0) listen(cache_a_sentinel, 4);

    int worker_b_port = 0;
    int worker_b_listener = bind_port(0, &worker_b_port);
    if (worker_b_listener >= 0) listen(worker_b_listener, 16);
    int cache_b_port = 0;
    int cache_b_sentinel = bind_port(0, &cache_b_port);
    if (cache_b_sentinel >= 0) listen(cache_b_sentinel, 4);

    int legacy_port = 0;
    int legacy_listener = bind_port(0, &legacy_port);
    if (legacy_listener >= 0) listen(legacy_listener, 16);

    ConfCSMsg *configuration = nullptr;
    MsgChannel *worker_a = login_host(
        port, "cache-route-a", true, worker_a_port, &configuration,
        nullptr, 1, 0, static_cast<uint32_t>(cache_a_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    delete configuration;
    configuration = nullptr;
    MsgChannel *worker_b = login_host(
        port, "cache-route-b", true, worker_b_port, &configuration,
        nullptr, 1, 0, static_cast<uint32_t>(cache_b_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    delete configuration;
    configuration = nullptr;
    MsgChannel *legacy_worker = login_host(
        port, "cache-route-legacy", true, legacy_port, &configuration);
    delete configuration;
    configuration = nullptr;
    MsgChannel *submitter = login_host(
        port, "cache-route-submit", false, 0, &configuration);
    delete configuration;
    REQUIRE(worker_a && worker_b && legacy_worker && submitter &&
                cache_a_sentinel >= 0 && cache_b_sentinel >= 0,
            "two compatible workers and one legacy worker join the mixed pool");

    /* Reproduce the S70 ordering: the failed endpoint A is free (or rejoins)
       before compatible B has capacity.  The retry must remain queued until
       B frees; deleting retry exclusion makes the immediate A checks below
       receive both PREPARE and UseCS. */
    REQUIRE(submitter && request_job_preferring(
                submitter, 6097, "cache-route-b"),
            "compatible alternative B is occupied before the retry probe");
    AssignPrepareMsg *busy_b_prepare = wait_prepare(worker_b);
    UseCSMsg *busy_b_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(busy_b_prepare && busy_b_use &&
                busy_b_use->port == static_cast<uint32_t>(worker_b_port),
            "setup consumes B's only real slot");
    if (busy_b_use)
        worker_b->send_msg(JobBeginMsg(busy_b_use->job_id, 0));

    REQUIRE(submitter && request_cache_job(
                submitter, 6098, std::string(), 0, 0,
                "127.0.0.1", static_cast<uint32_t>(worker_a_port)),
            "strict retry requests exact failed endpoint A exclusion");
    AssignPrepareMsg *forbidden_a_prepare = dynamic_cast<AssignPrepareMsg *>(
        wait_type(worker_a, Msg::ASSIGN_PREPARE, 300));
    UseCSMsg *premature_retry_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 300));
    REQUIRE(!forbidden_a_prepare && !premature_retry_use,
            "retry waits instead of returning to free failed endpoint A while B is busy");
    delete forbidden_a_prepare;
    delete premature_retry_use;

    if (busy_b_use)
        worker_b->send_msg(job_done_for(
            *busy_b_use, 0, JobDoneMsg::FROM_SERVER));
    AssignPrepareMsg *retry_b_prepare = wait_prepare(worker_b);
    UseCSMsg *retry_b_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(retry_b_prepare && retry_b_use &&
                retry_b_use->port == static_cast<uint32_t>(worker_b_port),
            "queued retry selects B immediately after alternative capacity frees");
    REQUIRE(retry_b_use && wait_file_contains(log,
                retry_decision_record(*retry_b_use, worker_a_port, true), 3000),
            "busy-alternative retry decision is bound to the exact new assignment");
    if (retry_b_use) {
        worker_b->send_msg(JobBeginMsg(retry_b_use->job_id, 0));
        worker_b->send_msg(job_done_for(
            *retry_b_use, 0, JobDoneMsg::FROM_SERVER));
    }
    if (retry_b_use) {
        const std::string terminal =
            "END " + std::to_string(retry_b_use->job_id) + " ";
        REQUIRE(wait_file_contains(log, terminal, 3000),
                "retry exclusion is request-local and releases B before later jobs");
    }
    delete retry_b_use;
    delete retry_b_prepare;
    delete busy_b_use;
    delete busy_b_prepare;

    REQUIRE(submitter && request_cache_job(
                submitter, 6099, "127.0.0.1",
                static_cast<uint32_t>(cache_a_port), CACHE_PROFILE_P29V1),
            "cache-capable C submits a stale ordinary-port affinity hint");
    UseCSMsg *stale_port_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    MsgChannel *stale_port_worker = stale_port_use &&
            stale_port_use->port == static_cast<uint32_t>(worker_b_port)
        ? worker_b : worker_a;
    AssignPrepareMsg *stale_port_prepare = wait_prepare(stale_port_worker);
    REQUIRE(stale_port_use && stale_port_prepare &&
                stale_port_use->port != static_cast<uint32_t>(cache_a_port) &&
                stale_port_use->hasCacheAdvertisement(),
            "stale-port affinity falls back immediately to a compatible free worker");
    if (stale_port_use) {
        stale_port_worker->send_msg(JobBeginMsg(stale_port_use->job_id, 0));
        stale_port_worker->send_msg(job_done_for(
            *stale_port_use, 0, JobDoneMsg::FROM_SERVER));
        const std::string terminal =
            "END " + std::to_string(stale_port_use->job_id) + " ";
        REQUIRE(wait_file_contains(log, terminal, 3000),
                "stale-hint control releases its selected worker");
    }
    delete stale_port_use;
    delete stale_port_prepare;

    REQUIRE(submitter && request_cache_job(
                submitter, 6100, "127.0.0.1",
                static_cast<uint32_t>(worker_b_port), CACHE_PROFILE_ZSTD_TU),
            "cache-capable C submits an affinity hint for a non-selected profile");
    UseCSMsg *wrong_profile_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    MsgChannel *wrong_profile_worker = wrong_profile_use &&
            wrong_profile_use->port == static_cast<uint32_t>(worker_b_port)
        ? worker_b : worker_a;
    AssignPrepareMsg *wrong_profile_prepare = wait_prepare(wrong_profile_worker);
    REQUIRE(wrong_profile_use && wrong_profile_prepare &&
                wrong_profile_use->cache_profile_mask == CACHE_PROFILE_P29V1,
            "profile-mismatched affinity falls back immediately without changing the selected profile");
    if (wrong_profile_use) {
        wrong_profile_worker->send_msg(JobBeginMsg(wrong_profile_use->job_id, 0));
        wrong_profile_worker->send_msg(job_done_for(
            *wrong_profile_use, 0, JobDoneMsg::FROM_SERVER));
        const std::string terminal =
            "END " + std::to_string(wrong_profile_use->job_id) + " ";
        REQUIRE(wait_file_contains(log, terminal, 3000),
                "profile-mismatch control releases its selected worker");
    }
    delete wrong_profile_use;
    delete wrong_profile_prepare;

    /* Both workers are accepted from 127.0.0.1.  The warm authority must
       therefore include the selected worker's ordinary port: a host-only
       hint would alias A and B and leave this choice to list order. */
    REQUIRE(submitter && request_cache_job(
                submitter, 6101, "127.0.0.1",
                static_cast<uint32_t>(worker_b_port), CACHE_PROFILE_P29V1),
            "cache-capable C requests its exact known warm worker endpoint");
    AssignPrepareMsg *prepare_b = wait_prepare(worker_b);
    UseCSMsg *use_b = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(prepare_b && use_b &&
                use_b->port == static_cast<uint32_t>(worker_b_port) &&
                use_b->cache_endpoint_port == static_cast<uint32_t>(cache_b_port) &&
                use_b->cache_profile_mask == CACHE_PROFILE_P29V1,
            "a free compatible warm worker wins before other compatible and legacy workers");
    if (use_b)
        worker_b->send_msg(JobBeginMsg(use_b->job_id, 0));

    REQUIRE(submitter && request_cache_job(submitter, 6102),
            "cache-capable C requests while its warm worker is full");
    AssignPrepareMsg *prepare_a = wait_prepare(worker_a);
    UseCSMsg *use_a = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(prepare_a && use_a &&
                use_a->port == static_cast<uint32_t>(worker_a_port) &&
                use_a->cache_endpoint_port == static_cast<uint32_t>(cache_a_port) &&
                use_a->cache_profile_mask == CACHE_PROFILE_P29V1,
            "another genuinely-free compatible worker wins when the warm one is full");
    if (use_a)
        worker_a->send_msg(JobBeginMsg(use_a->job_id, 0));

    REQUIRE(submitter && request_cache_job(submitter, 6103),
            "cache-capable C requests after every compatible real slot is full");
    AssignPrepareMsg *prepare_legacy = wait_prepare(legacy_worker);
    UseCSMsg *use_legacy = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(prepare_legacy && use_legacy &&
                use_legacy->port == static_cast<uint32_t>(legacy_port) &&
                !use_legacy->hasCacheAdvertisement(),
            "full compatible workers never starve an immediately-free legacy worker");

    if (use_b) {
        worker_b->send_msg(job_done_for(*use_b, 0, JobDoneMsg::FROM_SERVER));
    }
    if (use_a) {
        worker_a->send_msg(job_done_for(*use_a, 0, JobDoneMsg::FROM_SERVER));
    }
    if (use_legacy) {
        legacy_worker->send_msg(JobBeginMsg(use_legacy->job_id, 0));
        legacy_worker->send_msg(job_done_for(*use_legacy, 0,
                                             JobDoneMsg::FROM_SERVER));
    }
    delete use_b;
    delete use_a;
    delete use_legacy;
    delete prepare_b;
    delete prepare_a;
    delete prepare_legacy;
    delete submitter;
    delete worker_a;
    delete worker_b;
    delete legacy_worker;
    if (worker_a_listener >= 0) close(worker_a_listener);
    if (worker_b_listener >= 0) close(worker_b_listener);
    if (legacy_listener >= 0) close(legacy_listener);
    if (cache_a_sentinel >= 0) close(cache_a_sentinel);
    if (cache_b_sentinel >= 0) close(cache_b_sentinel);
    REQUIRE(stop_scheduler(scheduler),
            "cache-routing mixed-pool scheduler stopped cleanly");
    REQUIRE(file_contains(log, "P50_WARM_HINT_OVERRIDE job=") &&
                file_contains(log, "warm=1 compatible_free=2 idle_excluded=1"),
            "warm affinity overriding an idle compatible worker emits an S80 diagnostic");
}

static void run_cache_retry_pressure_and_preferred(
    const std::string &binary, const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/cache-retry-pressure.log";
    pid_t scheduler = start_cache_routing_scheduler(binary, port, log);
    REQUIRE(port != 0 && scheduler > 0,
            "retry-pressure scheduler process launched");

    int worker_a_port = 0;
    int worker_a_listener = bind_port(0, &worker_a_port);
    if (worker_a_listener >= 0) listen(worker_a_listener, 16);
    int cache_a_port = 0;
    int cache_a_sentinel = bind_port(0, &cache_a_port);
    if (cache_a_sentinel >= 0) listen(cache_a_sentinel, 4);

    int worker_b_port = 0;
    int worker_b_listener = bind_port(0, &worker_b_port);
    if (worker_b_listener >= 0) listen(worker_b_listener, 16);
    int cache_b_port = 0;
    int cache_b_sentinel = bind_port(0, &cache_b_port);
    if (cache_b_sentinel >= 0) listen(cache_b_sentinel, 4);

    int local_port = 0;
    int local_listener = bind_port(0, &local_port);
    if (local_listener >= 0) listen(local_listener, 16);

    ConfCSMsg *configuration = nullptr;
    MsgChannel *worker_a = login_host(
        port, "retry-pressure-a", true, worker_a_port, &configuration,
        nullptr, 1, 0, static_cast<uint32_t>(cache_a_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    delete configuration;
    configuration = nullptr;
    MsgChannel *worker_b = login_host(
        port, "retry-pressure-b", true, worker_b_port, &configuration,
        nullptr, 1, 0, static_cast<uint32_t>(cache_b_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    delete configuration;
    configuration = nullptr;
    /* This C is deliberately also locally capable.  A wait disposition that
       merely clears the remote list would otherwise fall through to it. */
    MsgChannel *submitter = login_host(
        port, "retry-pressure-submit", true, local_port, &configuration);
    delete configuration;
    configuration = nullptr;
    MsgChannel *other_submitter = login_host(
        port, "retry-pressure-other", false, 0, &configuration);
    delete configuration;
    REQUIRE(worker_a && worker_b && submitter && other_submitter,
            "two compatible F workers and a locally capable C join");

    REQUIRE(request_job_preferring(submitter, 6201, "retry-pressure-b"),
            "first job occupies B's real slot");
    AssignPrepareMsg *busy_b1_prepare = wait_prepare(worker_b);
    UseCSMsg *busy_b1 = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(busy_b1_prepare && busy_b1 &&
                busy_b1->port == static_cast<uint32_t>(worker_b_port),
            "B's single real slot is occupied");
    if (busy_b1) worker_b->send_msg(JobBeginMsg(busy_b1->job_id, 0));

    REQUIRE(request_job_preferring(submitter, 6202, "retry-pressure-b"),
            "second forced job occupies B's sole preload allowance");
    AssignPrepareMsg *busy_b2_prepare = wait_prepare(worker_b);
    UseCSMsg *busy_b2 = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(busy_b2_prepare && busy_b2 &&
                busy_b2->port == static_cast<uint32_t>(worker_b_port),
            "B reaches maxJobs plus maxPreloadCount saturation");
    if (busy_b2) worker_b->send_msg(JobBeginMsg(busy_b2->job_id, 0));

    REQUIRE(request_cache_job(
                submitter, 6203, std::string(), 0, 0, "127.0.0.1",
                static_cast<uint32_t>(worker_a_port)),
            "retry excludes free A while compatible B is preload-saturated");
    AssignPrepareMsg *forbidden_a = dynamic_cast<AssignPrepareMsg *>(
        wait_type(worker_a, Msg::ASSIGN_PREPARE, 300));
    UseCSMsg *forbidden_local = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 300));
    REQUIRE(!forbidden_a && !forbidden_local,
            "saturated B keeps retry queued without A or local-C fallback");
    delete forbidden_a;
    delete forbidden_local;

    REQUIRE(request_job_preferring(
                other_submitter, 6204, "retry-pressure-a"),
            "ordinary work is queued behind the waiting retry");
    AssignPrepareMsg *other_prepare = wait_prepare(worker_a);
    UseCSMsg *other_use = dynamic_cast<UseCSMsg *>(
        wait_type(other_submitter, Msg::USE_CS, 3000));
    REQUIRE(other_prepare && other_use &&
                other_use->port == static_cast<uint32_t>(worker_a_port),
            "retry wait does not wedge later schedulable work");
    if (other_use) {
        worker_a->send_msg(JobBeginMsg(other_use->job_id, 0));
        worker_a->send_msg(job_done_for(
            *other_use, 0, JobDoneMsg::FROM_SERVER));
    }
    delete other_use;
    delete other_prepare;

    if (busy_b1)
        worker_b->send_msg(job_done_for(
            *busy_b1, 0, JobDoneMsg::FROM_SERVER));
    forbidden_a = dynamic_cast<AssignPrepareMsg *>(
        wait_type(worker_a, Msg::ASSIGN_PREPARE, 300));
    forbidden_local = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 300));
    REQUIRE(!forbidden_a && !forbidden_local,
            "freeing only B's preload leaves no real slot and still waits");
    delete forbidden_a;
    delete forbidden_local;

    if (busy_b2)
        worker_b->send_msg(job_done_for(
            *busy_b2, 0, JobDoneMsg::FROM_SERVER));
    AssignPrepareMsg *retry_b_prepare = wait_prepare(worker_b);
    UseCSMsg *retry_b = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(retry_b_prepare && retry_b &&
                retry_b->port == static_cast<uint32_t>(worker_b_port),
            "preload-saturated alternative receives retry at its first real slot");
    if (retry_b) {
        worker_b->send_msg(JobBeginMsg(retry_b->job_id, 0));
        worker_b->send_msg(job_done_for(
            *retry_b, 0, JobDoneMsg::FROM_SERVER));
    }
    delete retry_b;
    delete retry_b_prepare;
    delete busy_b2;
    delete busy_b2_prepare;
    delete busy_b1;
    delete busy_b1_prepare;

    REQUIRE(set_worker_load(worker_b, 1000),
            "B publishes the absolute do-not-schedule load boundary");
    usleep(100 * 1000);
    REQUIRE(request_cache_job(
                submitter, 6205, std::string(), 0, 0, "127.0.0.1",
                static_cast<uint32_t>(worker_a_port)),
            "retry excludes A while compatible B is load=1000");
    forbidden_a = dynamic_cast<AssignPrepareMsg *>(
        wait_type(worker_a, Msg::ASSIGN_PREPARE, 300));
    forbidden_local = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 300));
    REQUIRE(!forbidden_a && !forbidden_local,
            "load-blocked B keeps retry queued without A or local-C fallback");
    delete forbidden_a;
    delete forbidden_local;
    REQUIRE(set_worker_load(worker_b, 0), "B clears its load boundary");
    retry_b_prepare = wait_prepare(worker_b);
    retry_b = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(retry_b_prepare && retry_b &&
                retry_b->port == static_cast<uint32_t>(worker_b_port),
            "load recovery releases the queued retry to B");
    if (retry_b) {
        worker_b->send_msg(JobBeginMsg(retry_b->job_id, 0));
        worker_b->send_msg(job_done_for(
            *retry_b, 0, JobDoneMsg::FROM_SERVER));
    }
    delete retry_b;
    delete retry_b_prepare;

    /* A saturated cache-compatible worker gets one initial P50 probe so the
       production S95 engagement path can observe its bounded failure.  The
       per-worker latch must then reject a second independent preferred job
       until the worker reports recovered load. */
    REQUIRE(set_worker_load(worker_b, 1000),
            "B re-enters the saturated state for the one-shot probe test");
    usleep(100 * 1000);
    REQUIRE(request_job_preferring(
                submitter, 6209, "retry-pressure-b"),
            "initial saturated P50 probe is requested");
    AssignPrepareMsg *probe_prepare = wait_prepare(worker_b);
    UseCSMsg *probe_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(probe_prepare && probe_use &&
                probe_use->port == static_cast<uint32_t>(worker_b_port),
            "one-shot saturated P50 probe reaches B");
    if (probe_use) {
        worker_b->send_msg(JobBeginMsg(probe_use->job_id, 0));
        worker_b->send_msg(job_done_for(
            *probe_use, 0, JobDoneMsg::FROM_SERVER));
    }
    delete probe_use;
    delete probe_prepare;

    REQUIRE(request_job_preferring(
                submitter, 6210, "retry-pressure-b"),
            "second independent saturated P50 probe is requested");
    AssignPrepareMsg *blocked_probe = dynamic_cast<AssignPrepareMsg *>(
        wait_type(worker_b, Msg::ASSIGN_PREPARE, 300));
    UseCSMsg *blocked_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 300));
    REQUIRE(!blocked_probe && !blocked_use,
            "one-shot latch blocks repeated saturated P50 probe");
    delete blocked_use;
    delete blocked_probe;

    REQUIRE(set_worker_load(worker_b, 0),
            "B load recovery reopens the latched P50 path");
    AssignPrepareMsg *recovered_probe = wait_prepare(worker_b);
    UseCSMsg *recovered_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(recovered_probe && recovered_use &&
                recovered_use->port == static_cast<uint32_t>(worker_b_port),
            "latched P50 probe releases after load recovery");
    if (recovered_use) {
        worker_b->send_msg(JobBeginMsg(recovered_use->job_id, 0));
        worker_b->send_msg(job_done_for(
            *recovered_use, 0, JobDoneMsg::FROM_SERVER));
    }
    delete recovered_use;
    delete recovered_probe;

    REQUIRE(request_job_preferring(
                submitter, 6206, "retry-pressure-a", "127.0.0.1",
                static_cast<uint32_t>(worker_a_port)),
            "retry carries a preferredHost that names its failed A endpoint");
    retry_b_prepare = wait_prepare(worker_b);
    retry_b = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    forbidden_a = dynamic_cast<AssignPrepareMsg *>(
        wait_type(worker_a, Msg::ASSIGN_PREPARE, 300));
    REQUIRE(retry_b_prepare && retry_b && !forbidden_a &&
                retry_b->port == static_cast<uint32_t>(worker_b_port),
            "retry exclusion outranks preferredHost and selects B");
    delete forbidden_a;
    if (retry_b) {
        worker_b->send_msg(JobBeginMsg(retry_b->job_id, 0));
        worker_b->send_msg(job_done_for(
            *retry_b, 0, JobDoneMsg::FROM_SERVER));
    }
    delete retry_b;
    delete retry_b_prepare;

    REQUIRE(set_worker_load(worker_b, 1000),
            "B becomes transiently blocked before capability-loss probe");
    usleep(100 * 1000);
    REQUIRE(request_cache_job(
                submitter, 6207, std::string(), 0, 0, "127.0.0.1",
                static_cast<uint32_t>(worker_a_port)),
            "retry initially waits for the blocked compatible B");
    forbidden_a = dynamic_cast<AssignPrepareMsg *>(
        wait_type(worker_a, Msg::ASSIGN_PREPARE, 300));
    forbidden_local = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 300));
    REQUIRE(!forbidden_a && !forbidden_local,
            "retry is waiting before B withdraws cache capability");
    delete forbidden_a;
    delete forbidden_local;

    LoginMsg b_without_cache(static_cast<uint32_t>(worker_b_port),
                             "retry-pressure-b", "x86_64", 0);
    b_without_cache.envs.push_back(std::make_pair(
        std::string("x86_64"), std::string("p49-test-env")));
    b_without_cache.max_kids = 1;
    b_without_cache.noremote = false;
    b_without_cache.chroot_possible = true;
    b_without_cache.setCacheAdvertisement(0, 0, 0);
    REQUIRE(worker_b->send_msg(b_without_cache),
            "B withdraws the compatible cache capability while retry waits");
    delete wait_type(worker_b, Msg::CS_CONF, 3000);
    AssignPrepareMsg *same_a_prepare = wait_prepare(worker_a);
    UseCSMsg *same_a = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(same_a_prepare && same_a &&
                same_a->port == static_cast<uint32_t>(worker_a_port),
            "capability disappearance restores lawful same-endpoint recovery");
    REQUIRE(same_a && wait_file_contains(log,
                retry_decision_record(*same_a, worker_a_port, false), 3000),
            "withdrawn-alternative decision authenticates the exact same-endpoint retry");
    if (same_a) {
        worker_a->send_msg(JobBeginMsg(same_a->job_id, 0));
        worker_a->send_msg(job_done_for(
            *same_a, 0, JobDoneMsg::FROM_SERVER));
    }
    delete same_a;
    delete same_a_prepare;

    REQUIRE(request_job_preferring(
                submitter, 6208, "retry-pressure-a"),
            "later ordinary request carries no retry exclusion");
    same_a_prepare = wait_prepare(worker_a);
    same_a = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(same_a_prepare && same_a &&
                same_a->port == static_cast<uint32_t>(worker_a_port),
            "retry exclusion does not leak to the next logical job");
    if (same_a) {
        worker_a->send_msg(JobBeginMsg(same_a->job_id, 0));
        worker_a->send_msg(job_done_for(
            *same_a, 0, JobDoneMsg::FROM_SERVER));
    }
    delete same_a;
    delete same_a_prepare;

    REQUIRE(file_contains(log, "P50_RETRY_AVOID_WAIT job=") &&
                file_contains(log, "P50_RETRY_AVOID_APPLIED job="),
            "pressure paths emit explicit retry WAIT and APPLIED evidence");

    delete other_submitter;
    delete submitter;
    delete worker_a;
    delete worker_b;
    if (local_listener >= 0) close(local_listener);
    if (worker_a_listener >= 0) close(worker_a_listener);
    if (worker_b_listener >= 0) close(worker_b_listener);
    if (cache_a_sentinel >= 0) close(cache_a_sentinel);
    if (cache_b_sentinel >= 0) close(cache_b_sentinel);
    REQUIRE(stop_scheduler(scheduler),
            "retry-pressure scheduler stopped cleanly");
}

static void run_cache_retry_single_worker(const std::string &binary,
                                          const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/cache-retry-single-worker.log";
    pid_t scheduler = start_cache_routing_scheduler(binary, port, log);
    REQUIRE(port != 0 && scheduler > 0,
            "single-worker retry scheduler process launched");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    int cache_port = 0;
    int cache_sentinel = bind_port(0, &cache_port);
    if (cache_sentinel >= 0) listen(cache_sentinel, 4);
    int local_port = 0;
    int local_listener = bind_port(0, &local_port);
    if (local_listener >= 0) listen(local_listener, 16);

    ConfCSMsg *configuration = nullptr;
    MsgChannel *worker = login_host(
        port, "retry-single-worker", true, worker_port, &configuration,
        nullptr, 1, 0, static_cast<uint32_t>(cache_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    delete configuration;
    configuration = nullptr;
    MsgChannel *submitter = login_host(
        port, "retry-single-submit", true, local_port, &configuration);
    delete configuration;
    REQUIRE(worker && submitter && request_cache_job(
                submitter, 6210, std::string(), 0, 0, "127.0.0.1",
                static_cast<uint32_t>(worker_port)),
            "single-worker retry requests its previous exact endpoint");
    AssignPrepareMsg *prepare = wait_prepare(worker);
    UseCSMsg *use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(prepare && use &&
                use->port == static_cast<uint32_t>(worker_port),
            "no compatible alternative permits same-endpoint replacement recovery");
    REQUIRE(use && wait_file_contains(log,
                retry_decision_record(*use, worker_port, false), 3000),
            "single-worker retry emits exact zero-alternative decision evidence");
    REQUIRE(!file_contains(log, "P50_RETRY_AVOID_WAIT job=") &&
                !file_contains(log, "P50_RETRY_AVOID_APPLIED job="),
            "single-worker fallback is not mislabeled as exclusion waiting");
    if (use) {
        worker->send_msg(JobBeginMsg(use->job_id, 0));
        worker->send_msg(job_done_for(*use, 0, JobDoneMsg::FROM_SERVER));
    }
    delete use;
    delete prepare;
    delete submitter;
    delete worker;
    if (local_listener >= 0) close(local_listener);
    if (worker_listener >= 0) close(worker_listener);
    if (cache_sentinel >= 0) close(cache_sentinel);
    REQUIRE(stop_scheduler(scheduler),
            "single-worker retry scheduler stopped cleanly");
}

static void run_cache_affinity_uses_exact_serialized_host(
    const std::string &binary, const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/cache-affinity-exact-host.log";
    pid_t scheduler = start_cache_routing_scheduler(binary, port, log);
    REQUIRE(port != 0 && scheduler > 0,
            "exact-host affinity scheduler process launched");

    int shared_worker_port = 0;
    int worker_listener = bind_port(0, &shared_worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    int cache_a_port = 0;
    int cache_a_sentinel = bind_port(0, &cache_a_port);
    if (cache_a_sentinel >= 0) listen(cache_a_sentinel, 4);
    int cache_b_port = 0;
    int cache_b_sentinel = bind_port(0, &cache_b_port);
    if (cache_b_sentinel >= 0) listen(cache_b_sentinel, 4);

    ConfCSMsg *configuration = nullptr;
    MsgChannel *worker_a = login_host(
        port, "affinity-real-a", true, shared_worker_port, &configuration,
        nullptr, 2, 0, static_cast<uint32_t>(cache_a_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    delete configuration;
    configuration = nullptr;

    // F-B arrives from a different peer address but deliberately chooses a
    // Login nodeName equal to F-A's canonical peer name.  It also advertises
    // the same ordinary port/profile, so only exact serialized-host matching
    // can distinguish the retained endpoint.
    MsgChannel *worker_b_connection =
        connect_scheduler(port, 5000, 0, "127.0.0.2");
    MsgChannel *worker_b = worker_b_connection
        ? login_host(port, "127.0.0.1", true, shared_worker_port,
                     &configuration, worker_b_connection, 1, 0,
                     static_cast<uint32_t>(cache_b_port), CACHE_WIRE_REVISION,
                     CACHE_PROFILE_P29V1)
        : nullptr;
    delete configuration;
    configuration = nullptr;
    MsgChannel *submitter = login_host(
        port, "affinity-exact-submit", false, 0, &configuration);
    delete configuration;
    REQUIRE(worker_a && worker_b && submitter && worker_listener >= 0 &&
                cache_a_sentinel >= 0 && cache_b_sentinel >= 0,
            "two address-distinct workers with a colliding Login nodeName joined");

    // Keep one of A's two slots occupied.  If the cache-affinity predicate
    // widens through CompileServer::matches(), least_busy will choose idle B.
    REQUIRE(submitter && request_job_preferring(
                submitter, 6199, "affinity-real-a"),
            "one exact F-A slot is occupied before the collision probe");
    UseCSMsg *busy_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    AssignPrepareMsg *busy_prepare = wait_prepare(worker_a);
    REQUIRE(busy_use && busy_prepare && busy_use->hostname == "127.0.0.1" &&
                busy_use->cache_endpoint_port ==
                    static_cast<uint32_t>(cache_a_port),
            "preferred-node setup selected the real F-A peer");
    if (busy_use && worker_a)
        worker_a->send_msg(JobBeginMsg(busy_use->job_id, 0));

    REQUIRE(submitter && request_cache_job(
                submitter, 6200, "127.0.0.1",
                static_cast<uint32_t>(shared_worker_port),
                CACHE_PROFILE_P29V1),
            "warm probe echoes F-A's serialized host/port/profile identity");
    UseCSMsg *warm_use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    MsgChannel *selected_worker =
        warm_use && warm_use->hostname == "127.0.0.2" ? worker_b : worker_a;
    AssignPrepareMsg *warm_prepare = wait_prepare(selected_worker);
    REQUIRE(warm_use && warm_prepare && warm_use->hostname == "127.0.0.1" &&
                warm_use->cache_endpoint_port ==
                    static_cast<uint32_t>(cache_a_port),
            "colliding F-B nodeName cannot impersonate F-A's canonical warm host");
    if (warm_use && selected_worker) {
        selected_worker->send_msg(JobBeginMsg(warm_use->job_id, 0));
        selected_worker->send_msg(job_done_for(
            *warm_use, 0, JobDoneMsg::FROM_SERVER));
    }
    if (busy_use && worker_a)
        worker_a->send_msg(job_done_for(
            *busy_use, 0, JobDoneMsg::FROM_SERVER));

    delete warm_prepare;
    delete busy_prepare;
    delete warm_use;
    delete busy_use;
    delete submitter;
    delete worker_b;
    delete worker_a;
    if (worker_listener >= 0) close(worker_listener);
    if (cache_a_sentinel >= 0) close(cache_a_sentinel);
    if (cache_b_sentinel >= 0) close(cache_b_sentinel);
    REQUIRE(stop_scheduler(scheduler),
            "exact-host affinity scheduler stopped cleanly");
}

static void run_legacy_cache_routing_neutrality(const std::string &binary,
                                                const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/cache-routing-legacy-neutrality.log";
    pid_t scheduler = start_cache_routing_scheduler(binary, port, log, nullptr);
    REQUIRE(port != 0 && scheduler > 0,
            "legacy least-busy scheduler process launched for cache-neutrality gate");

    int cache_worker_port = 0;
    int cache_worker_listener = bind_port(0, &cache_worker_port);
    if (cache_worker_listener >= 0) listen(cache_worker_listener, 16);
    int cache_endpoint_port = 0;
    int cache_endpoint_sentinel = bind_port(0, &cache_endpoint_port);
    if (cache_endpoint_sentinel >= 0) listen(cache_endpoint_sentinel, 4);
    int legacy_worker_port = 0;
    int legacy_worker_listener = bind_port(0, &legacy_worker_port);
    if (legacy_worker_listener >= 0) listen(legacy_worker_listener, 16);

    ConfCSMsg *configuration = nullptr;
    MsgChannel *cache_worker = login_host(
        port, "legacy-neutral-cache", true, cache_worker_port, &configuration,
        nullptr, 2, 0, static_cast<uint32_t>(cache_endpoint_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    delete configuration;
    configuration = nullptr;
    MsgChannel *legacy_worker = login_host(
        port, "legacy-neutral-old", true, legacy_worker_port, &configuration);
    delete configuration;
    configuration = nullptr;
    MsgChannel *submitter = login_host(
        port, "legacy-neutral-submit", false, 0, &configuration);
    delete configuration;
    REQUIRE(cache_worker && legacy_worker && submitter &&
                cache_endpoint_sentinel >= 0,
            "cache-capable and legacy workers join a legacy scheduler");

    REQUIRE(submitter && request_job_preferring(
                submitter, 6201, "legacy-neutral-cache"),
            "one legacy assignment occupies half of the cache-capable worker");
    UseCSMsg *occupied = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(occupied &&
                occupied->port == static_cast<uint32_t>(cache_worker_port),
            "preferred legacy assignment reaches the cache-capable worker");
    if (occupied)
        cache_worker->send_msg(JobBeginMsg(occupied->job_id, 0));

    REQUIRE(submitter && request_cache_job(submitter, 6202),
            "cache-shaped request is submitted while scheduler mode remains legacy");
    UseCSMsg *selected = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(selected &&
                selected->port == static_cast<uint32_t>(legacy_worker_port) &&
                !selected->hasCacheAdvertisement(),
            "legacy mode ignores cache preference and preserves least-busy selection");

    if (occupied)
        cache_worker->send_msg(job_done_for(*occupied, 0,
                                            JobDoneMsg::FROM_SERVER));
    if (selected) {
        legacy_worker->send_msg(JobBeginMsg(selected->job_id, 0));
        legacy_worker->send_msg(job_done_for(*selected, 0,
                                             JobDoneMsg::FROM_SERVER));
    }
    delete occupied;
    delete selected;
    delete submitter;
    delete cache_worker;
    delete legacy_worker;
    if (cache_worker_listener >= 0) close(cache_worker_listener);
    if (legacy_worker_listener >= 0) close(legacy_worker_listener);
    if (cache_endpoint_sentinel >= 0) close(cache_endpoint_sentinel);
    REQUIRE(stop_scheduler(scheduler),
            "legacy cache-neutrality scheduler stopped cleanly");
}

static void run_cache_handoff_below_p50(const std::string &binary,
                                        const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/cache-handoff-p48-worker.log";
    pid_t scheduler = start_scheduler(binary, port, nullptr, log);
    REQUIRE(port != 0 && scheduler > 0,
            "cache-handoff below-P50-worker scheduler process launched");

    int proxy_port = 0;
    pid_t proxy = start_p48_proxy(port, &proxy_port);
    MsgChannel *p48_channel = connect_scheduler(proxy_port);
    REQUIRE(proxy > 0 && p48_channel && p48_channel->protocol == 48,
            "worker link genuinely negotiated protocol 48 for the handoff gate");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    int cache_port = 0;
    int cache_sentinel = bind_port(0, &cache_port);
    if (cache_sentinel >= 0) listen(cache_sentinel, 4);

    ConfCSMsg *worker_conf = nullptr;
    /* login_host passes a fully-valid, nonzero advertisement, but
       LoginMsg::send_to_channel gates the whole tail on the WORKER link's
       own negotiated protocol -- 48 here -- so the encoder writes nothing:
       the bytes never reach the wire and the scheduler's retained snapshot
       for this worker stays structurally (0,0,0), never "invalid". */
    MsgChannel *worker = login_host(
        port, "cache-handoff-p48-worker", true, worker_port, &worker_conf,
        p48_channel, 1, 0, static_cast<uint32_t>(cache_port),
        CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU);
    REQUIRE(worker && worker_conf && cache_sentinel >= 0,
            "P48 worker logs in even though its cache advertisement cannot "
            "be sent");
    delete worker_conf;

    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(port, "cache-handoff-p50-submit", false,
                                       0, &submitter_conf);
    delete submitter_conf;
    REQUIRE(submitter && request_job(submitter, 5101),
            "assignment requested for the below-P50 (pre-CacheWire) worker");
    UseCSMsg *use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(use && use->port == static_cast<uint32_t>(worker_port)
                && !use->hasCacheAdvertisement()
                && use->cache_protocol == 0 && use->cache_profile_mask == 0,
            "S2: a CS below protocol 50 can never retain a cache "
            "advertisement, so its UseCS handoff tail is wholly absent");
    if (use) {
        worker->send_msg(JobBeginMsg(use->job_id, 0));
        worker->send_msg(job_done_for(*use, 0, JobDoneMsg::FROM_SERVER));
    }
    delete use;
    delete submitter;
    delete worker;
    if (worker_listener >= 0) close(worker_listener);
    if (cache_sentinel >= 0) close(cache_sentinel);
    REQUIRE(stop_scheduler(proxy),
            "cache-handoff protocol-48 relay stopped cleanly");
    REQUIRE(stop_scheduler(scheduler),
            "cache-handoff below-P50-worker scheduler stopped cleanly");
}

static void run_old_peer(const std::string &binary, const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/p48-worker-scheduler.log";
    pid_t scheduler = start_scheduler(
        binary, port, "enforcing-compat", log);
    REQUIRE(port != 0 && scheduler > 0,
            "EnforcingCompat scheduler launched for negotiated-P48 worker");

    int proxy_port = 0;
    pid_t proxy = start_p48_proxy(port, &proxy_port);
    MsgChannel *p48_channel = connect_scheduler(proxy_port);
    REQUIRE(proxy > 0 && p48_channel && p48_channel->protocol == 48,
            "worker link genuinely negotiated protocol 48");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(port, "p48-worker", true, worker_port,
                                    &worker_conf, p48_channel);
    REQUIRE(worker && worker_conf && worker_conf->epoch() == 0
                && worker_conf->fence_mode == ConfCSMsg::Legacy,
            "P48 ConfCS retains its old shape and legacy defaults");
    delete worker_conf;

    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(port, "p49-submit-to-p48", false, 0,
                                       &submitter_conf);
    delete submitter_conf;
    REQUIRE(submitter && request_job(submitter, 3001),
            "assignment to negotiated-P48 worker requested");
    UseCSMsg *use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(use != nullptr,
            "configured scheduler preserves direct legacy UseCS for old worker");
    REQUIRE(no_type(worker, Msg::ASSIGN_PREPARE, 300),
            "old worker receives no P49 control and incurs no READY wait");

    if (use) {
        worker->send_msg(JobBeginMsg(use->job_id, 0));
        worker->send_msg(job_done_for(*use, 0, JobDoneMsg::FROM_SERVER));
    }
    delete use;
    delete submitter;
    delete worker;
    if (worker_listener >= 0) close(worker_listener);
    REQUIRE(stop_scheduler(proxy), "protocol-48 relay stopped cleanly");
    REQUIRE(stop_scheduler(scheduler), "old-peer scheduler stopped cleanly");
}

static void run_old_submitter_to_new_worker(const std::string &binary,
                                             const std::string &directory)
{
    const int port = reserve_port_pair();
    const std::string log = directory + "/p48-submit-p50-worker.log";
    pid_t scheduler = start_scheduler(
        binary, port, "enforcing-compat", log);
    REQUIRE(port != 0 && scheduler > 0,
            "mixed-peer EnforcingCompat scheduler launched");

    int worker_port = 0;
    int worker_listener = bind_port(0, &worker_port);
    if (worker_listener >= 0) listen(worker_listener, 16);
    ConfCSMsg *worker_conf = nullptr;
    MsgChannel *worker = login_host(port, "p50-worker-for-p48-submit", true,
                                    worker_port, &worker_conf);
    REQUIRE(worker && worker_conf &&
                worker_conf->fence_mode == ConfCSMsg::EnforcingCompat,
            "new worker receives EnforcingCompat policy");
    delete worker_conf;

    int proxy_port = 0;
    pid_t proxy = start_p48_proxy(port, &proxy_port);
    MsgChannel *p48_channel = connect_scheduler(proxy_port);
    REQUIRE(proxy > 0 && p48_channel && p48_channel->protocol == 48,
            "submitter link genuinely negotiated protocol 48");
    ConfCSMsg *submitter_conf = nullptr;
    MsgChannel *submitter = login_host(
        port, "p48-submit-to-p50", false, 0, &submitter_conf, p48_channel);
    REQUIRE(submitter && submitter_conf && submitter_conf->epoch() == 0 &&
                submitter_conf->fence_mode == ConfCSMsg::Legacy,
            "old submitter sees legacy ConfCS projection");
    delete submitter_conf;

    REQUIRE(submitter && request_job(submitter, 3002),
            "legacy assignment from old submitter to new worker requested");
    UseCSMsg *use = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(use && use->port == static_cast<uint32_t>(worker_port) &&
                !use->hasAssignmentIdentity() &&
                !use->hasCacheAdvertisement(),
            "mixed C48/F-current dispatch remains wholly legacy");
    REQUIRE(no_type(worker, Msg::ASSIGN_PREPARE, 300),
            "new worker receives no uncarryable assignment fence for old C");

    uint32_t first_job_id = 0;
    if (use) {
        first_job_id = use->job_id;
        worker->send_msg(JobBeginMsg(use->job_id, 0));
        worker->send_msg(job_done_for(*use, 0, JobDoneMsg::FROM_SERVER));
    }
    const std::string first_end =
        "END " + std::to_string(first_job_id) + " status=0";
    REQUIRE(first_job_id != 0 && wait_file_contains(log, first_end, 3000),
            "scheduler accepts the legacy mixed-peer terminal");
    const std::string after_first = control_text(port, "listcs");
    REQUIRE(after_first.find("p50-worker-for-p48-submit") != std::string::npos
                && after_first.find("jobs=0/1") != std::string::npos,
            "accepted mixed-peer terminal preserves F and releases capacity");

    REQUIRE(submitter && request_job(submitter, 3003),
            "old submitter requests another assignment on the surviving F");
    UseCSMsg *second = dynamic_cast<UseCSMsg *>(
        wait_type(submitter, Msg::USE_CS, 3000));
    REQUIRE(second && second->port == static_cast<uint32_t>(worker_port)
                && !second->hasAssignmentIdentity()
                && !second->hasCacheAdvertisement(),
            "surviving F accepts a second wholly-legacy mixed assignment");
    REQUIRE(no_type(worker, Msg::ASSIGN_PREPARE, 300),
            "second mixed assignment also avoids an uncarryable fence");
    if (second) {
        worker->send_msg(JobBeginMsg(second->job_id, 0));
        worker->send_msg(job_done_for(*second, 0, JobDoneMsg::FROM_SERVER));
    }
    const std::string second_end = "END "
        + std::to_string(second ? second->job_id : 0) + " status=0";
    REQUIRE(second && wait_file_contains(log, second_end, 3000),
            "scheduler accepts the second legacy mixed-peer terminal");
    delete second;
    delete use;
    delete submitter;
    delete worker;
    if (worker_listener >= 0) close(worker_listener);
    REQUIRE(stop_scheduler(proxy),
            "old-submitter protocol-48 relay stopped cleanly");
    REQUIRE(stop_scheduler(scheduler),
            "old-submitter/new-worker scheduler stopped cleanly");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <icecc-scheduler>\n", argv[0]);
        return 2;
    }
    const char *temporary_root = std::getenv("TMPDIR");
    std::string directory_template =
        std::string(temporary_root && temporary_root[0] ? temporary_root : "/tmp")
        + "/icecream-p49-scheduler.XXXXXX";
    char *directory = mkdtemp(&directory_template[0]);
    if (!directory) return 2;
    std::fprintf(stderr, "retained work directory: %s\n", directory);
    signal(SIGPIPE, SIG_IGN);
    if (std::getenv("ICECC_TEST_PRE_EXPOSURE_LOSS_ONLY") != nullptr) {
        run_pre_exposure_worker_loss_redispatch(argv[1], directory);
        std::fprintf(stderr, "%s: %d failure(s)\n",
                     failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
    if (std::getenv("ICECC_TEST_CACHE_RETRY_ONLY") != nullptr) {
        run_cache_retry_pressure_and_preferred(argv[1], directory);
        run_cache_retry_single_worker(argv[1], directory);
        std::fprintf(stderr, "%s: %d failure(s)\n",
                     failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
    if (std::getenv("ICECC_TEST_REMOTE_REQUIRED_ONLY") != nullptr) {
        run_remote_required_waits_for_worker(argv[1], directory);
        std::fprintf(stderr, "%s: %d failure(s)\n",
                     failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
    run_remote_required_waits_for_worker(argv[1], directory);
    run_precompile_worker_terminal(argv[1], directory);
    run_post_begin_submitter_withdrawal(argv[1], directory);
    run_enforcing(argv[1], directory);
    run_prepare_credit(argv[1], directory);
    run_ready_nested_teardown(argv[1], directory);
    run_advisory(argv[1], directory);
    run_prepare_backlog(argv[1], directory);
    run_strict_nonce(argv[1], directory);
    run_pre_exposure_worker_loss_redispatch(argv[1], directory);
    run_disabled(argv[1], directory);
    run_cache_advertisement(argv[1], directory);
    run_cache_handoff_identity_bound(argv[1], directory);
    run_cache_routing_preference(argv[1], directory);
    run_cache_retry_pressure_and_preferred(argv[1], directory);
    run_cache_retry_single_worker(argv[1], directory);
    run_cache_affinity_uses_exact_serialized_host(argv[1], directory);
    run_legacy_cache_routing_neutrality(argv[1], directory);
    run_cache_handoff_below_p50(argv[1], directory);
    run_old_peer(argv[1], directory);
    run_old_submitter_to_new_worker(argv[1], directory);
    std::fprintf(stderr, "%s: %d failure(s)\n",
                 failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
