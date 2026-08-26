#include "daemon/p50_fork_fd_hygiene.h"

#include <cerrno>
#include <cstdlib>
#include <dirent.h>
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
#include <utility>

using icecc::p50::forkfd::DeliveryOwnerToken;
using icecc::p50::forkfd::Failure;
using icecc::p50::forkfd::ForkSourceLease;
using icecc::p50::forkfd::KeepSet;
using icecc::p50::forkfd::Result;

constexpr uint64_t kAcceptedDeliveryId = 77;

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

int make_readonly_source(int minimum) {
    char path[] = "/tmp/icecc-fork-fd-replacement-XXXXXX";
    const int writable = ::mkstemp(path);
    require(writable >= 0, "replacement mkstemp failed");
    require(::close(writable) == 0, "replacement writable close failed");
    const int readonly = ::open(path, O_RDONLY | O_CLOEXEC);
    (void)::unlink(path);
    return move_high(readonly, minimum);
}

size_t open_fd_count() {
    DIR* directory = ::opendir("/proc/self/fd");
    require(directory != nullptr, "could not enumerate test descriptors");
    size_t count = 0;
    while (::readdir(directory) != nullptr)
        ++count;
    require(::closedir(directory) == 0, "could not close test descriptor inventory");
    return count;
}

KeepSet keeps(const Inventory& inventory, bool with_source) {
    KeepSet result{inventory.stat_write, inventory.client_child, std::nullopt,
                   with_source, with_source
                       ? std::optional<int>(inventory.source) : std::nullopt};
    if (with_source) {
        auto owner = icecc::p50::forkfd::test_make_delivery_owner(
            inventory.source, 77);
        require(owner.has_value(), "delivery owner token was not minted");
        auto lease = icecc::p50::forkfd::mint_fork_source_lease(
            std::move(*owner), inventory.source, kAcceptedDeliveryId);
        require(lease.has_value(), "exact source lease was not minted");
        result.source = std::move(*lease);
    }
    return result;
}

int child_sweep(KeepSet& keep, int unrelated, int listener, bool await_eof,
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
    if (keep.source.has_value() && ::fcntl(keep.source->fd(), F_GETFD) < 0)
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
    if (pid == 0) {
        KeepSet keep = keeps(inventory, with_source);
        child_sweep(keep, with_source ? inventory.unrelated : inventory.source,
                    inventory.listener, await_eof, hooks);
    }
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
    KeepSet duplicate{inventory.stat_write, inventory.stat_write, std::nullopt,
                      false, std::nullopt};
    require(icecc::p50::forkfd::sweep(duplicate).failure == Failure::InvalidKeepSet,
            "duplicate keep set was accepted");
    KeepSet missing{inventory.stat_write, inventory.client_child, std::nullopt,
                    true, inventory.source};
    require(icecc::p50::forkfd::sweep(missing).failure == Failure::InvalidKeepSet,
            "P50 source omission was accepted");

    auto wrong_id_owner = icecc::p50::forkfd::test_make_delivery_owner(
        inventory.source, kAcceptedDeliveryId);
    require(wrong_id_owner.has_value(), "wrong-id owner token was not minted");
    require(!icecc::p50::forkfd::mint_fork_source_lease(
                 std::move(*wrong_id_owner), inventory.source,
                 kAcceptedDeliveryId + 1)
                 .has_value(),
            "mismatched DeliveryId was accepted by owner seam");

    auto wrong_fd_owner = icecc::p50::forkfd::test_make_delivery_owner(
        inventory.source, kAcceptedDeliveryId);
    require(wrong_fd_owner.has_value(), "wrong-fd owner token was not minted");
    require(!icecc::p50::forkfd::mint_fork_source_lease(
                 std::move(*wrong_fd_owner), inventory.unrelated,
                 kAcceptedDeliveryId)
                 .has_value(),
            "source-FD substitution was accepted by owner seam");

    auto valid_owner = icecc::p50::forkfd::test_make_delivery_owner(
        inventory.source, kAcceptedDeliveryId);
    require(valid_owner.has_value(), "valid owner token was not minted");
    auto valid_lease = icecc::p50::forkfd::mint_fork_source_lease(
        std::move(*valid_owner), inventory.source, kAcceptedDeliveryId);
    require(valid_lease.has_value(), "valid source lease was not minted");
    KeepSet mismatched_expected_fd{
        inventory.stat_write, inventory.client_child, std::move(valid_lease),
        true, inventory.unrelated};
    require(icecc::p50::forkfd::sweep(mismatched_expected_fd).failure ==
                Failure::InvalidKeepSet,
            "source lease was detached from its expected FD");

    KeepSet wrong_stat_direction{inventory.stat_read, inventory.client_child,
                                 std::nullopt, false, std::nullopt};
    require(icecc::p50::forkfd::sweep(wrong_stat_direction).failure ==
                Failure::OwnershipFailure,
            "statistics pipe read end was accepted");

    const int unconnected = ::socket(AF_UNIX, SOCK_STREAM, 0);
    require(unconnected >= 0, "unconnected socket failed");
    const int high_unconnected = move_high(unconnected, 45);
    KeepSet wrong_client{inventory.stat_write, high_unconnected, std::nullopt,
                         false, std::nullopt};
    require(icecc::p50::forkfd::sweep(wrong_client).failure ==
                Failure::OwnershipFailure,
            "unconnected client socket was accepted");
    (void)::close(high_unconnected);

    int datagram[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_DGRAM, 0, datagram) == 0,
            "datagram socketpair failed");
    const int high_datagram = move_high(datagram[1], 46);
    KeepSet wrong_socket_type{inventory.stat_write, high_datagram,
                              std::nullopt, false, std::nullopt};
    require(icecc::p50::forkfd::sweep(wrong_socket_type).failure ==
                Failure::TypeFailure,
            "non-stream client socket was accepted");
    (void)::close(datagram[0]);
    (void)::close(high_datagram);

    char path[] = "/tmp/icecc-fork-fd-writable-XXXXXX";
    const int writable = ::mkstemp(path);
    require(writable >= 0, "writable source mkstemp failed");
    (void)::unlink(path);
    const int high_writable = move_high(writable, 47);
    auto writable_owner = icecc::p50::forkfd::test_make_delivery_owner(
        high_writable, kAcceptedDeliveryId);
    require(writable_owner.has_value(), "writable owner token was not minted");
    auto writable_lease = icecc::p50::forkfd::mint_fork_source_lease(
        std::move(*writable_owner), high_writable, kAcceptedDeliveryId);
    require(writable_lease.has_value(), "writable source lease was not minted");
    KeepSet writable_source{inventory.stat_write, inventory.client_child,
                            std::move(writable_lease), true, high_writable};
    require(icecc::p50::forkfd::sweep(writable_source).failure ==
                Failure::OwnershipFailure,
            "writable source descriptor was accepted");
    (void)::close(high_writable);
}

void test_same_number_replacement_and_transfer() {
#if defined(__linux__)
    Inventory inventory = make_inventory(true);
    const int original_backup = ::fcntl(inventory.source, F_DUPFD_CLOEXEC, 48);
    require(original_backup >= 0, "could not back up source descriptor");
    const int replacement = make_readonly_source(49);

    auto owner = icecc::p50::forkfd::test_make_delivery_owner(
        inventory.source, kAcceptedDeliveryId);
    require(owner.has_value(), "replacement owner token was not minted");
    require(::dup3(replacement, inventory.source, O_CLOEXEC) == inventory.source,
            "same-number dup3 replacement failed");
    require(!icecc::p50::forkfd::mint_fork_source_lease(
                 std::move(*owner), inventory.source, kAcceptedDeliveryId)
                 .has_value(),
            "same-number regular-file replacement passed mint");
    require(::dup3(original_backup, inventory.source, O_CLOEXEC) ==
                inventory.source,
            "could not restore source after mint replacement");

    owner = icecc::p50::forkfd::test_make_delivery_owner(
        inventory.source, kAcceptedDeliveryId);
    require(owner.has_value(), "second replacement owner token was not minted");
    auto lease = icecc::p50::forkfd::mint_fork_source_lease(
        std::move(*owner), inventory.source, kAcceptedDeliveryId);
    require(lease.has_value(), "valid lease before replacement was rejected");
    KeepSet replaced_after_mint{inventory.stat_write, inventory.client_child,
                                std::move(lease), true, inventory.source};
    require(::dup3(replacement, inventory.source, O_CLOEXEC) == inventory.source,
            "same-number post-mint replacement failed");
    require(icecc::p50::forkfd::sweep(replaced_after_mint).failure ==
                Failure::InvalidKeepSet,
            "same-number replacement bypassed sweep identity revalidation");
    require(::dup3(original_backup, inventory.source, O_CLOEXEC) ==
                inventory.source,
            "could not restore source after post-mint replacement");

    owner = icecc::p50::forkfd::test_make_delivery_owner(
        inventory.source, kAcceptedDeliveryId);
    require(owner.has_value(), "close/reopen owner token was not minted");
    const int reopen = make_readonly_source(50);
    require(::close(inventory.source) == 0, "source close before reopen failed");
    require(::dup3(reopen, inventory.source, O_CLOEXEC) == inventory.source,
            "close/reopen same-number replacement failed");
    (void)::close(reopen);
    require(!icecc::p50::forkfd::mint_fork_source_lease(
                 std::move(*owner), inventory.source, kAcceptedDeliveryId)
                 .has_value(),
            "close/reopen same-number replacement passed mint");
    require(::dup3(original_backup, inventory.source, O_CLOEXEC) ==
                inventory.source,
            "could not restore source after close/reopen");
    (void)::close(original_backup);
    (void)::close(replacement);

    Inventory transfer_inventory = make_inventory(true);
    const size_t before = open_fd_count();
    {
        auto transfer_owner = icecc::p50::forkfd::test_make_delivery_owner(
            transfer_inventory.source, kAcceptedDeliveryId);
        require(transfer_owner.has_value(), "transfer owner token was not minted");
        const size_t after_owner = open_fd_count();
        require(after_owner == before + 1,
                "owner token did not retain exactly one independent duplicate");
        auto transfer_lease = icecc::p50::forkfd::mint_fork_source_lease(
            std::move(*transfer_owner), transfer_inventory.source,
            kAcceptedDeliveryId);
        require(transfer_lease.has_value(), "transfer lease was not minted");
        require(open_fd_count() == after_owner,
                "mint changed descriptor ownership count unexpectedly");
    }
    require(open_fd_count() == before,
            "lease destruction leaked or double-closed its proof descriptor");
    require(::fcntl(transfer_inventory.source, F_GETFD) >= 0,
            "lease incorrectly closed the handoff descriptor");
#endif
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

    Inventory proc_close_failure = make_inventory(true);
    icecc::p50::forkfd::TestHooks proc_close;
    proc_close.force_close_range_unsupported = true;
    proc_close.force_proc_close_ebadf = true;
    require(run_child(proc_close_failure, true, false, proc_close) ==
                100 + static_cast<int>(Failure::CloseFailure),
            "/proc directory EBADF close failure was ignored");
}

} // namespace

int main() {
    test_real_sweep_and_residue();
    test_exact_two_and_source_omission();
    test_validation();
    test_same_number_replacement_and_transfer();
    test_fallback_and_injected_failures();
    std::cout << "ok - exact first-fork descriptor hygiene\n";
    return 0;
}
