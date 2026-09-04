#include "cache/p50_input_fd_attachment.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "p50_input_fd_attachment_test: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition)
        fail(message);
}

std::vector<uint8_t> input_bytes() {
    std::vector<uint8_t> result(8192);
    for (size_t index = 0; index != result.size(); ++index)
        result[index] = static_cast<uint8_t>((index * 29 + 17) & 0xff);
    return result;
}

struct Transaction {
    TxBegin begin;
    TxCommit commit;
};

Transaction transaction_for(TuSeq tu_seq, std::span<const uint8_t> input) {
    static constexpr std::array<uint8_t, 3> body{0x50, 0x35, 0x30};
    TxBegin begin;
    begin.history_nonce = HistoryNonce{41};
    begin.rel_seq = RelSeq{2};
    begin.tu_seq = tu_seq;
    begin.profile = ProfileId::ZSTD_TU;
    begin.pre_state_digest = icecc::digest128("input-fd-pre-state");
    begin.body = describe_component(
        static_cast<uint16_t>(ProfileId::ZSTD_TU), body, input.size());
    begin.raw_bytes = input.size();
    begin.raw_digest = icecc::digest128(input);
    begin.transaction_digest = compute_transaction_digest(begin, body);
    TxCommit commit{
        begin.history_nonce,
        begin.rel_seq,
        begin.tu_seq,
        begin.transaction_digest,
        begin.raw_digest,
        compute_post_state_digest(begin.pre_state_digest, begin.history_nonce,
                                  begin.rel_seq, begin.tu_seq,
                                  begin.transaction_digest)};
    return {begin, commit};
}

std::vector<uint8_t> read_all(int fd) {
    std::vector<uint8_t> result;
    std::array<uint8_t, 257> buffer{};
    for (;;) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count == 0)
            return result;
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            fail("read of attached descriptor failed");
        result.insert(result.end(), buffer.begin(), buffer.begin() + count);
    }
}

int current_uid() {
    return static_cast<int>(::geteuid());
}

void delay_materialization() noexcept {
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
}

std::atomic<unsigned> materialization_hook_calls{0};

void delay_readonly_reopen() noexcept {
    if (materialization_hook_calls.fetch_add(1, std::memory_order_relaxed) == 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
}

}  // namespace

int main() {
    const std::filesystem::path runtime =
        std::filesystem::path("/tanksmall/scratch/ictmp/build-luna") /
        ("p50-input-fd-runtime-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code error;
    std::filesystem::remove_all(runtime, error);
    require(std::filesystem::create_directory(runtime, error) && !error,
            "private runtime directory creation failed");
    require(::chmod(runtime.c_str(), S_IRWXU) == 0,
            "private runtime directory mode failed");
    const std::string socket_path = (runtime / "attach.sock").string();

    const CStoreGuid guid = Id128::from_u64(0xabc);
    const InputRecordKey key{guid, TuSeq{9}};
    const std::vector<uint8_t> input = input_bytes();
    const Transaction transaction = transaction_for(key.tu_seq, input);
    InputRecordStore store(4, 1U << 20);
    require(store.publish(guid, transaction.begin, transaction.commit, input) ==
                InputPublishResult::Published,
            "test InputRecord was not published");

    icecc::p50::local::Status listen_status = icecc::p50::local::Status::InvalidArgument;
    const int listener = icecc::p50::local::listen_unix(socket_path, 4, &listen_status);
    require(listener >= 0 && listen_status == icecc::p50::local::Status::Ok,
            "AF_UNIX listener setup failed");
    const local::Identity identity{17, 23};
    const InputLeaseOwner owner{71, 73, 79};
    const local::CredentialExpectation peer{
        static_cast<uint64_t>(current_uid()), std::nullopt, std::nullopt};
    InputFdAttachmentService service(
        [&store](InputRecordKey requested) { return store.attach(requested); });

    std::array<InputFdAttachmentResult, 5> server_results{};
    std::thread server([&] {
        for (size_t index = 0; index != server_results.size(); ++index) {
            InputFdAttachmentResult& server_result = server_results[index];
            local::Status accept_status = local::Status::InvalidArgument;
            local::Connection connection = local::accept_unix(listener, &accept_status);
            if (!connection.valid()) {
                server_result.status = InputFdAttachmentStatus::Disconnected;
                continue;
            }
            if (index == 3)
                test_set_input_materialization_progress_hook(
                    delay_materialization);
            if (index == 4) {
                materialization_hook_calls.store(0, std::memory_order_relaxed);
                test_set_input_materialization_progress_hook(
                    delay_readonly_reopen);
            }
            server_result = service.serve(
                connection, identity, peer,
                std::chrono::steady_clock::now() +
                    (index >= 3 ? std::chrono::milliseconds(20)
                                : std::chrono::seconds(5)));
            if (index >= 3)
                test_set_input_materialization_progress_hook(nullptr);
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    InputFdAttachmentResult first = InputFdAttachmentClient::attach(
        socket_path, InputFdRequest{identity, key, owner, 1}, peer, deadline);
    require(first.status == InputFdAttachmentStatus::Accepted && first.fd.valid(),
            "first exact attachment failed");
    const int first_status_flags = ::fcntl(first.fd.get(), F_GETFL);
    const int first_descriptor_flags = ::fcntl(first.fd.get(), F_GETFD);
    require(first_status_flags >= 0 && (first_status_flags & O_ACCMODE) == O_RDONLY &&
                first_descriptor_flags >= 0 && (first_descriptor_flags & FD_CLOEXEC) != 0,
            "receiver observed the materialized snapshot as O_RDONLY|O_CLOEXEC");
    int seals = ::fcntl(first.fd.get(), F_GET_SEALS);
    require(seals >= 0 && (seals & F_SEAL_WRITE) != 0,
            "authorized descriptor was not sealed");
    std::array<uint8_t, 19> prefix{};
    require(::read(first.fd.get(), prefix.data(), prefix.size()) ==
                static_cast<ssize_t>(prefix.size()),
            "first descriptor did not start at byte zero");

    InputFdAttachmentResult second = InputFdAttachmentClient::attach(
        socket_path, InputFdRequest{identity, key, owner, 2}, peer,
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
    require(second.status == InputFdAttachmentStatus::Accepted && second.fd.valid(),
            "second exact attachment failed");
    require(read_all(second.fd.get()) == input,
            "second attachment did not have an independent byte-zero cursor");
    require(read_all(first.fd.get()) ==
                std::vector<uint8_t>(input.begin() + prefix.size(), input.end()),
            "first attachment cursor was not independent");

    const InputRecordKey missing{guid, TuSeq{10}};
    InputFdAttachmentResult missing_result = InputFdAttachmentClient::attach(
        socket_path, InputFdRequest{identity, missing, owner, 3}, peer,
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
    require(!missing_result.fd.valid() &&
                missing_result.status == InputFdAttachmentStatus::UnknownRecord,
            "missing record did not return its authenticated typed refusal");

    InputFdAttachmentResult slow = InputFdAttachmentClient::attach(
        socket_path, InputFdRequest{identity, key, owner, 4}, peer,
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
    require(!slow.fd.valid(),
            "mid-materialization deadline unexpectedly handed off a descriptor");

    InputFdAttachmentResult reopen_slow = InputFdAttachmentClient::attach(
        socket_path, InputFdRequest{identity, key, owner, 5}, peer,
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
    require(!reopen_slow.fd.valid(),
            "read-only reopen deadline unexpectedly handed off a descriptor");

    InputFdAttachmentResult expired = InputFdAttachmentClient::attach(
        socket_path, InputFdRequest{identity, key, owner, 6}, peer,
        std::chrono::steady_clock::now() - std::chrono::milliseconds(1));
    require(!expired.fd.valid() && expired.status == InputFdAttachmentStatus::Timeout,
            "expired absolute deadline did not fail closed");

    server.join();
    ::close(listener);
    std::filesystem::remove_all(runtime, error);
    require(server_results[2].status == InputFdAttachmentStatus::UnknownRecord,
            "service did not reject the missing exact key");
    require(server_results[3].status == InputFdAttachmentStatus::Timeout,
            "materialization ignored its absolute deadline");
    require(server_results[4].status == InputFdAttachmentStatus::Timeout,
            "read-only reopen lost its absolute-timeout classification");

    return 0;
}
