/*
 * Discriminating regression for the scheduler daemon-listener re-arm bound.
 *
 * Peer A is accepted and receives ConfCS.  The scheduler's own trace line,
 * emitted only after that accept cycle has set its monotonic one-second
 * deadline, is the barrier.  Peer B then establishes TCP while the listener
 * is absent from the poll set.  No management query, StatsMsg or broadcast
 * event is allowed to wake the loop.  The fixed scheduler must configure B
 * from the deadline wake; the exact negative control disables only that poll
 * cap and must leave B unconfigured for the observation interval.
 */
#include <config.h>

#include "../services/comm.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

const char *kPlatform = "x86_64";
const int kReplyBoundMsec = 2200;

uint64_t monotonic_msec()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return uint64_t(ts.tv_sec) * 1000ULL + uint64_t(ts.tv_nsec) / 1000000ULL;
}

int tcp_connect(int port)
{
    const int fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<unsigned short>(port));
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

MsgChannel *channel_from_fd(int fd)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    return Service::createChannel(fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa));
}

bool login_and_configure(MsgChannel *channel, const char *name, int timeout_s)
{
    if (!channel) {
        return false;
    }
    LoginMsg login(0, name, kPlatform, 0);
    login.max_kids = 0;
    login.noremote = true;       // no connectivity timer may wake the scheduler
    login.chroot_possible = false;
    if (!channel->send_msg(login)) {
        return false;
    }
    Msg *reply = channel->get_msg(timeout_s, true);
    const bool configured = reply && *reply == Msg::CS_CONF;
    delete reply;
    return configured;
}

bool port_pair_free(int port)
{
    int fds[2] = { -1, -1 };
    for (int off = 0; off < 2; ++off) {
        fds[off] = socket(PF_INET, SOCK_STREAM, 0);
        if (fds[off] < 0) {
            break;
        }
        int on = 1;
        setsockopt(fds[off], SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<unsigned short>(port + off));
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fds[off], reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)) != 0) {
            break;
        }
    }
    const bool free = fds[0] >= 0 && fds[1] >= 0;
    for (int &fd : fds) {
        if (fd >= 0) {
            close(fd);
        }
    }
    return free;
}

int choose_port()
{
    const int first = 31000 + (getpid() % 1000) * 2;
    for (int port = first; port < first + 2000; port += 2) {
        if (port_pair_free(port)) {
            return port;
        }
    }
    return -1;
}

pid_t start_scheduler(const char *binary, int port, const std::string &log,
                      bool disable_cap)
{
    const pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    signal(SIGPIPE, SIG_DFL);
    setenv("ICECC_TEST_DISABLE_BROADCAST", "1", 1);
    if (disable_cap) {
        setenv("ICECC_TEST_RELISTEN_NO_DEADLINE_CAP", "1", 1);
    } else {
        unsetenv("ICECC_TEST_RELISTEN_NO_DEADLINE_CAP");
    }
    FILE *out = fopen(log.c_str(), "w");
    if (out) {
        dup2(fileno(out), STDOUT_FILENO);
        dup2(fileno(out), STDERR_FILENO);
    }
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", port);
    execl(binary, binary, "-p", port_text, "-vvv", static_cast<char *>(nullptr));
    _exit(127);
}

bool wait_for_scheduler(int port, pid_t pid)
{
    for (int i = 0; i < 100; ++i) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return false;
        }
        const int fd = tcp_connect(port);
        if (fd >= 0) {
            close(fd);
            return true;
        }
        usleep(50 * 1000);
    }
    return false;
}

bool parse_pause_deadline(const std::string &log, uint64_t not_before,
                          uint64_t *deadline)
{
    std::ifstream in(log.c_str());
    if (!in) {
        return false;
    }
    std::string line;
    const std::string marker = "daemon listener pause armed: deadline_msec=";
    bool found = false;
    uint64_t newest = 0;
    while (std::getline(in, line)) {
        const size_t pos = line.find(marker);
        if (pos == std::string::npos) {
            continue;
        }
        const char *field = line.c_str() + pos + marker.size();
        char *end = nullptr;
        errno = 0;
        const unsigned long long value = strtoull(field, &end, 10);
        if (errno == 0 && end && end != field
                && static_cast<uint64_t>(value) >= not_before
                && static_cast<uint64_t>(value) > newest) {
            newest = static_cast<uint64_t>(value);
            found = true;
        }
    }
    if (found) {
        *deadline = newest;
    }
    return found;
}

bool wait_for_pause_barrier(const std::string &log, uint64_t not_before,
                            uint64_t *deadline)
{
    const uint64_t until = monotonic_msec() + 1500;
    while (monotonic_msec() < until) {
        if (parse_pause_deadline(log, not_before, deadline)) {
            return true;
        }
        usleep(10 * 1000);
    }
    return false;
}

struct ChildEvent {
    char stage;              // C = TCP connected, R = ConfCS received, E = error
    uint64_t elapsed_msec;
};

pid_t start_peer_b(int port, int report_fd)
{
    const pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    const uint64_t begin = monotonic_msec();
    const int fd = tcp_connect(port);
    ChildEvent event = { fd >= 0 ? 'C' : 'E', monotonic_msec() - begin };
    (void)write(report_fd, &event, sizeof(event));
    if (fd < 0) {
        _exit(2);
    }
    MsgChannel *channel = channel_from_fd(fd);
    const bool configured = login_and_configure(channel, "relisten-peer-B", 15);
    event.stage = configured ? 'R' : 'E';
    event.elapsed_msec = monotonic_msec() - begin;
    (void)write(report_fd, &event, sizeof(event));
    delete channel;
    _exit(configured ? 0 : 3);
}

bool read_event_until(int fd, uint64_t absolute_deadline, ChildEvent *event)
{
    while (monotonic_msec() < absolute_deadline) {
        const uint64_t left = absolute_deadline - monotonic_msec();
        struct pollfd pfd = { fd, POLLIN, 0 };
        const int timeout = static_cast<int>(left > 100 ? 100 : left);
        const int pr = poll(&pfd, 1, timeout);
        if (pr < 0 && errno == EINTR) {
            continue;
        }
        if (pr <= 0) {
            continue;
        }
        const ssize_t n = read(fd, event, sizeof(*event));
        return n == static_cast<ssize_t>(sizeof(*event));
    }
    return false;
}

bool stop_scheduler(pid_t pid)
{
    kill(pid, SIGTERM);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool run_case(const char *scheduler, bool negative)
{
    const int port = choose_port();
    if (port < 0) {
        fprintf(stderr, "FAILED   - no free scheduler port pair\n");
        return false;
    }
    char tag[128];
    snprintf(tag, sizeof(tag), "relisten-%s-%d.log", negative ? "negative" : "fixed",
             static_cast<int>(getpid()));
    const std::string log(tag);
    const pid_t sched = start_scheduler(scheduler, port, log, negative);
    if (sched < 0 || !wait_for_scheduler(port, sched)) {
        fprintf(stderr, "FAILED   - scheduler startup (%s)\n", negative ? "negative" : "fixed");
        if (sched > 0) {
            kill(sched, SIGKILL);
            waitpid(sched, nullptr, 0);
        }
        return false;
    }

    const uint64_t a_accept_floor = monotonic_msec() + 1000;
    MsgChannel *a = channel_from_fd(tcp_connect(port));
    if (!login_and_configure(a, "relisten-peer-A", 5)) {
        fprintf(stderr, "FAILED   - peer A configuration (%s)\n",
                negative ? "negative" : "fixed");
        delete a;
        stop_scheduler(sched);
        return false;
    }

    uint64_t listener_deadline = 0;
    if (!wait_for_pause_barrier(log, a_accept_floor, &listener_deadline)) {
        fprintf(stderr, "FAILED   - accept-cycle pause barrier (%s)\n",
                negative ? "negative" : "fixed");
        delete a;
        stop_scheduler(sched);
        return false;
    }
    const uint64_t now = monotonic_msec();
    if (listener_deadline <= now + 400) {
        fprintf(stderr, "FAILED   - barrier did not leave a durable paused interval"
                        " (%s, remaining=%lldms)\n",
                negative ? "negative" : "fixed",
                static_cast<long long>(listener_deadline) - static_cast<long long>(now));
        delete a;
        stop_scheduler(sched);
        return false;
    }

    int report[2];
    if (pipe(report) != 0) {
        delete a;
        stop_scheduler(sched);
        return false;
    }
    const uint64_t b_start = monotonic_msec();
    const pid_t b = start_peer_b(port, report[1]);
    close(report[1]);

    ChildEvent event = { 0, 0 };
    bool tcp_connected = read_event_until(report[0], b_start + 700, &event)
                         && event.stage == 'C';
    bool configured = false;
    if (tcp_connected) {
        while (monotonic_msec() < b_start + kReplyBoundMsec) {
            if (!read_event_until(report[0], b_start + kReplyBoundMsec, &event)) {
                break;
            }
            if (event.stage == 'R') {
                configured = true;
                break;
            }
            if (event.stage == 'E') {
                break;
            }
        }
    }
    close(report[0]);

    bool case_ok = tcp_connected;
    if (!tcp_connected) {
        fprintf(stderr, "FAILED   - peer B TCP establishment (%s)\n",
                negative ? "negative" : "fixed");
    } else if (!negative && !configured) {
        fprintf(stderr, "FAILED   - fixed scheduler did not configure peer B within %dms\n",
                kReplyBoundMsec);
        case_ok = false;
    } else if (negative && configured) {
        fprintf(stderr, "FAILED   - cap-disabled scheduler configured peer B in %llums;"
                        " regression is not discriminating\n",
                static_cast<unsigned long long>(event.elapsed_msec));
        case_ok = false;
    } else {
        fprintf(stderr, "ok       - %s relisten transition (%s, elapsed=%llums)\n",
                negative ? "cap removal remains red" : "deadline wake configures peer B",
                negative ? "no configuration inside bound" : "configuration complete",
                static_cast<unsigned long long>(configured ? event.elapsed_msec
                                                           : monotonic_msec() - b_start));
    }

    if (!configured) {
        kill(b, SIGKILL);
    }
    int b_status = 0;
    while (waitpid(b, &b_status, 0) < 0 && errno == EINTR) {
    }
    delete a;
    const bool clean_exit = stop_scheduler(sched);
    if (!clean_exit) {
        fprintf(stderr, "FAILED   - scheduler requested-shutdown exit status (%s)\n",
                negative ? "negative" : "fixed");
        case_ok = false;
    }
    unlink(log.c_str());
    return case_ok;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <icecc-scheduler>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    const bool fixed = run_case(argv[1], false);
    const bool negative = run_case(argv[1], true);
    fprintf(stderr, "# relisten: fixed=%d negative-control=%d\n",
            fixed ? 1 : 0, negative ? 1 : 0);
    return fixed && negative ? 0 : 1;
}
