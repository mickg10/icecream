#include "daemon/p50_fork_fd_hygiene.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>

using icecc::p50::forkfd::AcceptedSource;
using icecc::p50::forkfd::Failure;
using icecc::p50::forkfd::KeepSet;
using icecc::p50::forkfd::Result;

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "p50_fork_fd_hygiene_test: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition)
        fail(message);
}

int move_high(int fd, int minimum = 30) {
    const int result = ::fcntl(fd, F_DUPFD_CLOEXEC, minimum);
    require(result >= minimum, "could not make a nonstandard descriptor");
    require(::close(fd) == 0, "could not close low descriptor");
    return result;
}

struct Inventory {
    int stat_read = -1;
    int stat_write = -1;
    int client_peer = -1;
    int client_child = -1;
    int source = -1;
    int unrelated = -1;
    int listener = -1;

    ~Inventory() {
        for (int* fd : {&stat_read, &stat_write, &client_peer, &client_child,
                        &source, &unrelated, &listener}) {
            if (*fd >= 0)
                (void)::close(*fd);
        }
    }
};

Inventory make_inventory(bool with_source) {
    Inventory result;
    int pipe_fds[2] = {-1, -1};
    require(::pipe(pipe_fds) == 0, "pipe failed");
    result.stat_read = move_high(pipe_fds[0]);
    result.stat_write = move_high(pipe_fds[1]);
    int sockets[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "socketpair failed");
    result.client_peer = sockets[0];
    result.client_child = move_high(sockets[1]);

    if (with_source) {
        char path[] = "/tmp/icecc-fork-fd-XXXXXX";
        const int source = ::mkstemp(path);
        require(source >= 0, "mkstemp failed");
        require(::close(source) == 0, "could not close writable source");
        const int readonly_source = ::open(path, O_RDONLY | O_CLOEXEC);
        (void)::unlink(path);
        result.source = move_high(readonly_source, 35);
    }
    int unrelated_pipe[2] = {-1, -1};
    require(::pipe(unrelated_pipe) == 0, "unrelated pipe failed");
    result.unrelated = move_high(unrelated_pipe[1], 40);
    (void)::close(unrelated_pipe[0]);
    result.listener = ::socket(AF_INET, SOCK_STREAM, 0);
    require(result.listener >= 0, "listener socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(result.listener, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == 0,
            "listener bind failed");
    require(::listen(result.listener, 1) == 0, "listener listen failed");
    return result;
}

KeepSet keeps(const Inventory& inventory, bool with_source) {
    KeepSet result{inventory.stat_write, inventory.client_child, std::nullopt};
    if (with_source)
        result.source = AcceptedSource{inventory.source, 77};
    return result;
}

int child_sweep(const KeepSet& keep, int unrelated, int listener, bool await_eof,
                std::optional<icecc::p50::forkfd::TestHooks> hooks = std::nullopt) {
    if (hooks.has_value())
        icecc::p50::forkfd::set_test_hooks(*hooks);
    const Result result = icecc::p50::forkfd::sweep(keep);
    if (!result.ok())
        _exit(100 + static_cast<int>(result.failure));
    int code = 0;
    if (::fcntl(keep.stat_pipe_fd, F_GETFD) < 0 ||
        (::fcntl(keep.stat_pipe_fd, F_GETFD) & FD_CLOEXEC) == 0 ||
        ::fcntl(keep.client_fd, F_GETFD) < 0 ||
        (::fcntl(keep.client_fd, F_GETFD) & FD_CLOEXEC) == 0)
        code |= 1;
    if (keep.source.has_value() && ::fcntl(keep.source->fd, F_GETFD) < 0)
        code |= 2;
    if (::fcntl(unrelated, F_GETFD) >= 0)
        code |= 4;
    if (::fcntl(listener, F_GETFD) >= 0)
        code |= 32;
    if (await_eof) {
        struct pollfd waiter{keep.client_fd, POLLIN | POLLHUP, 0};
        if (::poll(&waiter, 1, 3000) <= 0 || !(waiter.revents & POLLHUP))
            code |= 8;
        char byte = 0;
        if (::recv(keep.client_fd, &byte, 1, MSG_DONTWAIT) != 0)
            code |= 16;
    }
    _exit(code);
}

int run_child(const Inventory& inventory, bool with_source, bool await_eof,
              std::optional<icecc::p50::forkfd::TestHooks> hooks = std::nullopt) {
    const pid_t pid = ::fork();
    require(pid >= 0, "fork failed");
    if (pid == 0)
        child_sweep(keeps(inventory, with_source),
                    with_source ? inventory.unrelated : inventory.source,
                    inventory.listener, await_eof, hooks);
    // The child must retain the client socket long enough to observe the
    // ordinary client OP_CANCEL/EOF, while its inherited peer is not part of
    // its keep set. Closing the parent peer makes that event deterministic.
    if (await_eof)
        (void)::close(inventory.client_peer);
    int status = 0;
    require(::waitpid(pid, &status, 0) == pid, "waitpid failed");
    require(WIFEXITED(status), "hygiene child did not exit normally");
    return WEXITSTATUS(status);
}

void test_real_sweep_and_residue() {
    Inventory inventory = make_inventory(true);
    const int status = run_child(inventory, true, true);
    require(status == 0, "real close_range sweep leaked or lost a keep fd");
    require(::fcntl(inventory.unrelated, F_GETFD) >= 0,
            "child sweep altered the parent descriptor table");
    require(::fcntl(inventory.listener, F_GETFD) >= 0,
            "child sweep altered the parent listener descriptor");
}

void test_exact_two_and_source_omission() {
    Inventory inventory = make_inventory(false);
    require(run_child(inventory, false, false) == 0,
            "exact legacy two-FD inventory failed");
    Inventory extra = make_inventory(true);
    require(run_child(extra, false, false) == 0,
            "omitted source was not treated as unrelated child residue");
}

void test_validation() {
    Inventory inventory = make_inventory(true);
    KeepSet duplicate{inventory.stat_write, inventory.stat_write, std::nullopt};
    require(icecc::p50::forkfd::sweep(duplicate).failure == Failure::InvalidKeepSet,
            "duplicate keep set was accepted");
    KeepSet missing{inventory.stat_write, inventory.client_child,
                    AcceptedSource{-1, 77}};
    require(icecc::p50::forkfd::sweep(missing).failure == Failure::InvalidKeepSet,
            "source DeliveryId without source FD was accepted");
    KeepSet no_id{inventory.stat_write, inventory.client_child,
                  AcceptedSource{inventory.source, 0}};
    require(icecc::p50::forkfd::sweep(no_id).failure == Failure::InvalidKeepSet,
            "source FD without DeliveryId was accepted");
    KeepSet wrong_type{inventory.stat_write, inventory.client_child,
                       AcceptedSource{inventory.stat_read, 77}};
    require(icecc::p50::forkfd::sweep(wrong_type).failure == Failure::TypeFailure,
            "non-regular source was accepted");
}

void test_fallback_and_injected_failures() {
    Inventory inventory = make_inventory(true);
    icecc::p50::forkfd::TestHooks unsupported;
    unsupported.force_close_range_unsupported = true;
    require(run_child(inventory, true, false, unsupported) == 0,
            "ENOSYS close_range fallback failed");

    Inventory proc_failure = make_inventory(true);
    icecc::p50::forkfd::TestHooks proc;
    proc.force_close_range_unsupported = true;
    proc.force_proc_failure = true;
    require(run_child(proc_failure, true, false, proc) ==
                100 + static_cast<int>(Failure::EnumerationFailure),
            "/proc enumeration failure was not explicit");

    Inventory close_failure = make_inventory(true);
    icecc::p50::forkfd::TestHooks close;
    close.force_close_range_unsupported = true;
    close.fail_close_fd = close_failure.unrelated;
    require(run_child(close_failure, true, false, close) ==
                100 + static_cast<int>(Failure::CloseFailure),
            "fallback close failure was not explicit");

    Inventory range_failure = make_inventory(true);
    icecc::p50::forkfd::TestHooks range;
    range.force_close_range_failure = true;
    require(run_child(range_failure, true, false, range) ==
                100 + static_cast<int>(Failure::CloseFailure),
            "close_range failure was not explicit");

    Inventory parse_failure = make_inventory(true);
    icecc::p50::forkfd::TestHooks parse;
    parse.force_close_range_unsupported = true;
    parse.force_parse_failure = true;
    require(run_child(parse_failure, true, false, parse) ==
                100 + static_cast<int>(Failure::ParseFailure),
            "proc parse failure was not explicit");
}

} // namespace

int main() {
    test_real_sweep_and_residue();
    test_exact_two_and_source_omission();
    test_validation();
    test_fallback_and_injected_failures();
    std::cout << "ok - exact first-fork descriptor hygiene\n";
    return 0;
}
