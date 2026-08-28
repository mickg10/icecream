#include "p50_endpoint.h"
#include "p50_slice0.h"

#include "p50_adopted_outcome_writer.h"
#include "p50_grz.h"
#include "p50_p29_residual.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>

namespace icecc::p50 {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

std::vector<std::vector<uint8_t>> p29_line_regions(std::span<const uint8_t> input) {
    std::vector<std::vector<uint8_t>> regions;
    size_t begin = 0;
    for (size_t at = 0; at < input.size(); ++at) {
        if (input[at] != '\n') continue;
        regions.emplace_back(input.begin() + begin, input.begin() + at + 1);
        begin = at + 1;
    }
    if (begin != input.size())
        regions.emplace_back(input.begin() + begin, input.end());
    return regions;
}

class SingleThreadOwner {
public:
    void require() const {
        const std::thread::id current = std::this_thread::get_id();
        std::lock_guard lock(mutex_);
        if (!thread_) {
            thread_ = current;
            return;
        }
        if (*thread_ != current)
            throw std::logic_error("mutable Protocol-50 state crossed its owner thread");
    }

    void check() const {
        const std::thread::id current = std::this_thread::get_id();
        std::lock_guard lock(mutex_);
        if (thread_ && *thread_ != current)
            throw std::logic_error("Protocol-50 state was read outside its owner thread");
    }

private:
    mutable std::mutex mutex_;
    mutable std::optional<std::thread::id> thread_;
};

class StaleCompletion final : public std::exception {
public:
    const char* what() const noexcept override { return "stale endpoint completion"; }
};

void validate_caps(const EndpointCaps& caps) {
    if (caps.wire.max_frame_payload < kMandatoryControlFramePayload ||
        caps.wire.max_frame_payload > kInitialMaxFramePayload)
        throw std::invalid_argument("endpoint frame cap is outside the Protocol-50 range");
    if (caps.wire.max_fill_record_bytes < 32)
        throw std::invalid_argument("endpoint FILL-record cap is too small");
    validate_zstd_tu_limits(caps.zstd);
    if (caps.profile != ProfileId::P29 && caps.profile != ProfileId::ZSTD_TU &&
        caps.profile != ProfileId::Z3_LONG
#if defined(ICECC_P50_WITH_LIBBSC)
        && caps.profile != ProfileId::GRZ
#endif
        )
        throw std::invalid_argument("endpoint profile is unsupported");
    if (caps.supported_profiles == 0 ||
        (caps.supported_profiles & ~kOperationalProfileMask) != 0)
        throw std::invalid_argument("endpoint supported profile mask is unsupported");
    if ((caps.supported_profiles & profile_bit(caps.profile)) == 0)
        throw std::invalid_argument("endpoint profile is not supported");
}

GlobalResourceLimits global_resource_limits(const P50ServerOwnerLimits& limits) {
    const uint64_t aggregate = limits.max_retained_input_bytes;
    const uint64_t staging = limits.max_pending_raw_bytes;
    const uint64_t total = staging > std::numeric_limits<uint64_t>::max() - aggregate
                               ? std::numeric_limits<uint64_t>::max()
                               : aggregate + staging;
    return {.max_aggregate_bytes = aggregate,
            .max_namespace_bytes = aggregate,
            .max_staging_bytes = staging,
            .max_total_bytes = total,
            .max_generation = KeyLayoutV1::generation_value_mask,
            .max_staging_slots = limits.max_live_sessions};
}

CompletionStamp with_operation(CompletionStamp stamp, AsyncOperationKind operation) {
    stamp.operation = operation;
    return stamp;
}

CompletionStamp completion_for_test(CompletionStamp stamp, EndpointIoControl& control) {
    if (control.wrong_digest_completion == stamp.operation && stamp.transaction_bound) {
        control.wrong_digest_completion.reset();
        stamp.transaction_digest.bytes[0] ^= 0x80;
    }
    if (control.wrong_raw_digest_completion == stamp.operation &&
        stamp.transaction_bound) {
        control.wrong_raw_digest_completion.reset();
        stamp.raw_digest.bytes[0] ^= 0x80;
    }
    return stamp;
}

void bind_live_identity(CompletionLiveIdentity& identity, const TxBegin& begin) {
    identity.history_nonce = begin.history_nonce;
    identity.rel_seq = begin.rel_seq;
    identity.tu_seq = begin.tu_seq;
    identity.transaction_digest = begin.transaction_digest;
    identity.raw_digest = begin.raw_digest;
    identity.transaction_bound = true;
}

void bind_live_identity(CompletionLiveIdentity& identity, const TxCommit& commit) {
    identity.history_nonce = commit.history_nonce;
    identity.rel_seq = commit.rel_seq;
    identity.tu_seq = commit.tu_seq;
    identity.transaction_digest = commit.transaction_digest;
    identity.raw_digest = commit.raw_digest;
    identity.transaction_bound = true;
}

void require_observed_completion(const CompletionStamp& expected,
                                 const CompletionStamp& observed) {
    if (observed != expected)
        throw StaleCompletion();
}

void require_live_completion(const CompletionStamp& expected,
                             const CompletionLiveIdentity& live) {
    if (expected.actor != live.actor)
        throw StaleCompletion();
    if (expected.c_store_guid != live.c_store_guid)
        throw StaleCompletion();
    if (expected.f_store_guid != live.f_store_guid)
        throw StaleCompletion();
    if (expected.cache_session_operation != live.cache_session_operation)
        throw StaleCompletion();
    if (expected.absolute_deadline != live.absolute_deadline)
        throw StaleCompletion();
    if (expected.session_serial != live.session_serial)
        throw StaleCompletion();
    if (expected.history_nonce != live.history_nonce)
        throw StaleCompletion();
    if (expected.rel_seq != live.rel_seq)
        throw StaleCompletion();
    if (expected.tu_seq != live.tu_seq)
        throw StaleCompletion();
    if (expected.transaction_digest != live.transaction_digest)
        throw StaleCompletion();
    if (expected.raw_digest != live.raw_digest)
        throw StaleCompletion();
    if (expected.transaction_bound != live.transaction_bound)
        throw StaleCompletion();
}

ErrorMessage bounded_error(uint16_t code, std::string_view detail, uint32_t payload_cap) {
    if (payload_cap < 6)
        throw std::invalid_argument("ERROR frame cap cannot carry its fixed fields");
    ErrorMessage result{code, std::string(detail)};
    const size_t maximum_detail = payload_cap - 6;
    if (result.detail.size() > maximum_detail)
        result.detail.resize(maximum_detail);
    return result;
}

class ClientTerminalResult final {
public:
    ClientTerminalResult(ErrorMessage error, uint32_t payload_cap)
        : error_(std::move(error)) {
        if (encode_payload(Message{error_}).size() > payload_cap)
            throw std::invalid_argument("terminal result exceeds its negotiated frame cap");
    }

    [[nodiscard]] const ErrorMessage& error() const { return error_; }

private:
    ErrorMessage error_;
};

void set_client_terminal_result(ClientRunResult& result, ErrorMessage error) {
    result.status = ClientRunStatus::TerminalError;
    result.observation = ClientRunObservation::PeerTerminalFrame;
    result.terminal_error = std::move(error);
}

template <class Function>
decltype(auto) client_checked(uint32_t payload_cap, Function&& function) {
    try {
        return std::forward<Function>(function)();
    } catch (const ClientTerminalResult&) {
        throw;
    } catch (const std::exception& error) {
        throw ClientTerminalResult(bounded_error(3, error.what(), payload_cap), payload_cap);
    }
}

void record_completion(CompletionLog* log, CompletionStamp stamp, size_t bytes,
                       const boost::system::error_code& error) {
    if (log)
        log->record({stamp, static_cast<uint64_t>(bytes), error.value()});
}

void close_now(tcp::socket& socket) {
    boost::system::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

// The timer owns the socket through this shared state.  Consequently a
// canceled/late timer completion can never dereference the run coroutine's
// stack, and no callback captures the endpoint (whose owner may be gone).
struct ClientIoState {
    explicit ClientIoState(asio::any_io_executor executor)
        : socket(executor), timer(executor) {}

    tcp::socket socket;
    asio::steady_timer timer;
    bool expired = false;
    bool committed = false;
};

class ClientRunSocketTarget final : public EndpointSocketTarget {
public:
    explicit ClientRunSocketTarget(std::weak_ptr<ClientIoState> io) noexcept
        : io_(std::move(io)) {}
    void cancel() noexcept override {
        if (const auto io = io_.lock())
            close_now(io->socket);
    }

private:
    std::weak_ptr<ClientIoState> io_;
};

struct ServerMaterializationAsyncState;
void cancel_materialization_notification(
    const std::shared_ptr<ServerMaterializationAsyncState>& state) noexcept;

// Server timers own the adopted socket through shared state for the same
// reason as the client timer above: a cancelled or late handler must never
// dereference the run coroutine's stack.
struct ServerIoState {
    explicit ServerIoState(
        tcp::socket socket_value,
        std::optional<daemon::P50FSessionOperationId> operation_value,
        std::optional<sidecar::AbsoluteMonotonicDeadline> deadline_value)
        : socket(std::move(socket_value)),
          deadline_timer(socket.get_executor()),
          operation(std::move(operation_value)),
          deadline(std::move(deadline_value)) {}

    tcp::socket socket;
    asio::posix::stream_descriptor deadline_timer;
    std::optional<daemon::P50FSessionOperationId> operation;
    std::optional<sidecar::AbsoluteMonotonicDeadline> deadline;
    std::weak_ptr<ServerMaterializationAsyncState> materialization;
    bool expired = false;
    bool cancelled = false;
};

class ServerRunSocketTarget final : public EndpointSocketTarget {
public:
    explicit ServerRunSocketTarget(std::weak_ptr<ServerIoState> io) noexcept
        : io_(std::move(io)) {}
    void cancel() noexcept override {
        if (const auto io = io_.lock()) {
            io->cancelled = true;
            if (auto materialization = io->materialization.lock())
                cancel_materialization_notification(materialization);
            close_now(io->socket);
        }
    }

private:
    std::weak_ptr<ServerIoState> io_;
};

bool arm_absolute_deadline_timer(
    ServerIoState& io,
    const sidecar::AbsoluteMonotonicDeadline& deadline,
    boost::system::error_code& error) {
    error.clear();
    if (!deadline.valid()) {
        error = asio::error::invalid_argument;
        return false;
    }
    const int timer_fd =
        ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        error.assign(errno, boost::system::generic_category());
        return false;
    }
    itimerspec timer{};
    timer.it_value.tv_sec =
        static_cast<time_t>(deadline.expires_at_ns / 1000000000);
    timer.it_value.tv_nsec =
        static_cast<long>(deadline.expires_at_ns % 1000000000);
    if (::timerfd_settime(timer_fd, TFD_TIMER_ABSTIME, &timer, nullptr) != 0) {
        error.assign(errno, boost::system::generic_category());
        (void)::close(timer_fd);
        return false;
    }
    io.deadline_timer.assign(timer_fd, error);
    if (error) {
        (void)::close(timer_fd);
        return false;
    }
    return true;
}

struct ServerMaterializationJob {
    CStoreGuid c_store_guid;
    TxBegin begin;
    TxCommit commit;
    std::shared_ptr<ProfileDialogue> dialogue;
    std::function<void()> before_materialize;
};

struct ServerMaterializationCompletion {
    TxBegin begin;
    TxCommit commit;
    std::shared_ptr<ProfileDialogue> dialogue;
    InputRecordStore::PreparedPublish prepared_input;
    std::exception_ptr failure;
};

// A worker may outlive both the endpoint coroutine and its io_context.  Keep
// its notification authority as an ordinary CLOEXEC descriptor, never as an
// Asio object or executor reference.  eventfd gives the worker and the owner
// independent descriptor ownership of one nonblocking counter and cannot
// raise SIGPIPE when the owner has already gone away.
class UniqueNotificationFd {
public:
    UniqueNotificationFd() noexcept = default;
    explicit UniqueNotificationFd(int fd) noexcept : fd_(fd) {}
    ~UniqueNotificationFd() { reset(); }

    UniqueNotificationFd(UniqueNotificationFd&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}
    UniqueNotificationFd& operator=(UniqueNotificationFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    UniqueNotificationFd(const UniqueNotificationFd&) = delete;
    UniqueNotificationFd& operator=(const UniqueNotificationFd&) = delete;

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
    void reset() noexcept {
        if (fd_ >= 0)
            (void)::close(std::exchange(fd_, -1));
    }

private:
    int fd_ = -1;
};

void signal_notification_fd(int fd) noexcept {
    const uint64_t value = 1;
    for (;;) {
        const ssize_t result = ::write(fd, &value, sizeof(value));
        if (result == static_cast<ssize_t>(sizeof(value)))
            return;
        if (result < 0 && errno == EINTR)
            continue;
        // EAGAIN means the eventfd counter is already saturated and therefore
        // readable.  Any other error is fail-closed: the adopted endpoint has
        // an absolute timer/cancellation path and the worker owns no commit
        // authority.
        return;
    }
}

struct ServerMaterializationAsyncState {
    // The worker may be the last owner of this state after an endpoint has
    // timed out.  It must therefore contain no Asio object tied to the owner
    // execution_context.  The coroutine owns the Asio descriptor while this
    // state owns a duplicate of the underlying eventfd.
    explicit ServerMaterializationAsyncState(
        UniqueNotificationFd worker_notification_value) noexcept
        : worker_notification(std::move(worker_notification_value)) {}

    UniqueNotificationFd worker_notification;
    std::mutex mutex;
    std::optional<ServerMaterializationCompletion> completion;
};

void cancel_materialization_notification(
    const std::shared_ptr<ServerMaterializationAsyncState>& state) noexcept {
    signal_notification_fd(state->worker_notification.get());
}

class EndpointCodecPool {
public:
    static constexpr size_t kWorkerCount = 2;
    static constexpr size_t kMaxOutstanding = 8;

    [[nodiscard]] bool try_acquire() noexcept {
        size_t current = outstanding_.load(std::memory_order_relaxed);
        while (current < kMaxOutstanding) {
            if (outstanding_.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed))
                return true;
        }
        return false;
    }

    void release() noexcept {
        const size_t previous =
            outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        if (previous == 0)
            std::terminate();
    }

    [[nodiscard]] asio::thread_pool& executor() noexcept { return pool_; }

private:
    asio::thread_pool pool_{kWorkerCount};
    std::atomic<size_t> outstanding_{0};
};

EndpointCodecPool& endpoint_codec_pool() {
    // Product-wide and bounded: codec work cannot create one thread per
    // relationship or per TU, and abandoned jobs cannot grow an unbounded
    // thread-pool queue.  Jobs carry no endpoint publication authority.
    static EndpointCodecPool pool;
    return pool;
}

struct EndpointCodecSlotGuard {
    EndpointCodecPool* pool = nullptr;
    ~EndpointCodecSlotGuard() {
        if (pool != nullptr)
            pool->release();
    }
};

asio::awaitable<ServerMaterializationCompletion>
async_materialize(ServerMaterializationJob job,
                  const std::shared_ptr<ServerIoState>& io) {
    const auto owner_executor = co_await asio::this_coro::executor;
    UniqueNotificationFd owner_notification(
        ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!owner_notification.valid())
        throw boost::system::system_error(
            boost::system::error_code(errno,
                                      boost::system::generic_category()));
    UniqueNotificationFd worker_notification(
        ::fcntl(owner_notification.get(), F_DUPFD_CLOEXEC, 0));
    if (!worker_notification.valid())
        throw boost::system::system_error(
            boost::system::error_code(errno,
                                      boost::system::generic_category()));

    asio::posix::stream_descriptor notification(owner_executor);
    boost::system::error_code assign_error;
    const int owner_notification_fd = owner_notification.release();
    notification.assign(owner_notification_fd, assign_error);
    if (assign_error) {
        (void)::close(owner_notification_fd);
        throw boost::system::system_error(assign_error);
    }

    auto state = std::make_shared<ServerMaterializationAsyncState>(
        std::move(worker_notification));
    io->materialization = state;
    EndpointCodecPool& codec_pool = endpoint_codec_pool();
    if (!codec_pool.try_acquire())
        throw std::runtime_error("endpoint codec queue is full");
    EndpointCodecPool* const codec_pool_owner = &codec_pool;
    try {
        asio::post(codec_pool.executor(),
               [state, codec_pool_owner, job = std::move(job)]() mutable {
                   EndpointCodecSlotGuard slot{codec_pool_owner};
                   ServerMaterializationCompletion completion{
                       .begin = job.begin,
                       .commit = job.commit,
                       .dialogue = std::move(job.dialogue),
                       .prepared_input = {},
                       .failure = {}};
                   try {
                       if (job.before_materialize)
                           job.before_materialize();
                       std::vector<uint8_t> exact =
                           completion.dialogue->materialize();
                       completion.prepared_input =
                           InputRecordStore::prepare_publish(
                               job.c_store_guid, job.begin, job.commit,
                               std::move(exact));
                   } catch (...) {
                       completion.failure = std::current_exception();
                   }
                   {
                       std::lock_guard lock(state->mutex);
                       state->completion.emplace(std::move(completion));
                   }
                   signal_notification_fd(state->worker_notification.get());
               });
    } catch (...) {
        codec_pool.release();
        if (auto active = io->materialization.lock();
            active && active.get() == state.get())
            io->materialization.reset();
        throw;
    }

    boost::system::error_code notification_error;
    co_await notification.async_wait(
        asio::posix::stream_descriptor::wait_read,
        asio::redirect_error(asio::use_awaitable, notification_error));
    std::lock_guard lock(state->mutex);
    if (auto active = io->materialization.lock();
        active && active.get() == state.get())
        io->materialization.reset();
    if (!state->completion.has_value()) {
        if (io->expired)
            throw boost::system::system_error(asio::error::timed_out);
        throw boost::system::system_error(asio::error::operation_aborted);
    }
    co_return std::move(*state->completion);
}

bool set_cloexec_fd(int fd, boost::system::error_code& error) {
    if (fd < 0) {
        error = asio::error::bad_descriptor;
        return false;
    }
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        error.assign(errno, boost::system::generic_category());
        return false;
    }
    return true;
}

void close_native_fd(int fd) noexcept {
    if (fd >= 0)
        (void)::close(fd);
}

bool inspect_native_connected_tcp(int fd, int& family, boost::system::error_code& error) {
    if (!set_cloexec_fd(fd, error))
        return false;

    int socket_type = 0;
    socklen_t type_length = sizeof(socket_type);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &socket_type, &type_length) != 0 ||
        socket_type != SOCK_STREAM) {
        error.assign(errno == 0 ? ENOTSOCK : errno, boost::system::generic_category());
        return false;
    }

    sockaddr_storage local{};
    socklen_t local_length = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_length) != 0) {
        error.assign(errno, boost::system::generic_category());
        return false;
    }
    sockaddr_storage peer{};
    socklen_t peer_length = sizeof(peer);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_length) != 0) {
        error.assign(errno, boost::system::generic_category());
        return false;
    }
    if ((local.ss_family != AF_INET && local.ss_family != AF_INET6) ||
        peer.ss_family != local.ss_family) {
        error = asio::error::operation_not_supported;
        return false;
    }
    family = local.ss_family;
    return true;
}

bool validate_adopted_socket(tcp::socket& socket) {
    if (!socket.is_open())
        return false;
    boost::system::error_code error;
    if (!set_cloexec_fd(socket.native_handle(), error))
        return false;
    const tcp::endpoint local = socket.local_endpoint(error);
    if (error)
        return false;
    const tcp::endpoint peer = socket.remote_endpoint(error);
    if (error || local.protocol().family() != peer.protocol().family())
        return false;
    return local.protocol().family() == AF_INET || local.protocol().family() == AF_INET6;
}

template <class Verify>
asio::awaitable<void> async_connect(tcp::socket& socket, const tcp::endpoint& remote,
                                    CompletionStamp stamp, CompletionLog* log, Verify verify) {
    const CompletionStamp expected = with_operation(stamp, AsyncOperationKind::Connect);
    boost::system::error_code error;
    co_await socket.async_connect(remote, asio::redirect_error(asio::use_awaitable, error));
    record_completion(log, expected, 0, error);
    verify(expected);
    if (error)
        throw boost::system::system_error(error);
}

template <class Verify>
asio::awaitable<void> async_accept(tcp::acceptor& acceptor, tcp::socket& socket,
                                   CompletionStamp stamp, CompletionLog* log, Verify verify) {
    const CompletionStamp expected = with_operation(stamp, AsyncOperationKind::Accept);
    boost::system::error_code error;
    co_await acceptor.async_accept(socket, asio::redirect_error(asio::use_awaitable, error));
    record_completion(log, expected, 0, error);
    verify(expected);
    if (error)
        throw boost::system::system_error(error);
}

template <class Verify>
asio::awaitable<void> async_write_message(tcp::socket& socket, Message message,
                                          uint32_t max_payload, CompletionStamp stamp,
                                          CompletionLog* log, EndpointIoControl& control,
                                          Verify verify) {
    const MessageType type = message_type(message);
    if (control.close_before_write == type) {
        control.close_before_write.reset();
        close_now(socket);
        throw boost::system::system_error(asio::error::operation_aborted);
    }
    if (control.max_write_fragment == 0)
        throw std::invalid_argument("write fragment limit is zero");
    std::vector<uint8_t> frame = encode_frame(message);
    if (frame.size() - 4 > max_payload)
        throw std::length_error("outbound frame exceeds the negotiated cap");
    size_t offset = 0;
    while (offset != frame.size()) {
        const size_t count = std::min(control.max_write_fragment, frame.size() - offset);
        const CompletionStamp expected =
            with_operation(stamp, AsyncOperationKind::WriteFragment);
        boost::system::error_code error;
        const size_t written =
            co_await asio::async_write(socket, asio::buffer(frame.data() + offset, count),
                                       asio::redirect_error(asio::use_awaitable, error));
        record_completion(log, expected, written, error);
        verify(expected);
        if (error)
            throw boost::system::system_error(error);
        offset += written;
    }
    if (control.close_after_write == type) {
        control.close_after_write.reset();
        close_now(socket);
        throw boost::system::system_error(asio::error::operation_aborted);
    }
}

template <class Verify>
asio::awaitable<void> async_report_client_terminal(
    tcp::socket& socket, ErrorMessage terminal, uint32_t max_payload,
    CompletionStamp stamp, CompletionLog* log, EndpointIoControl& control,
    Verify verify) {
    try {
        co_await async_write_message(socket, Message{terminal}, max_payload, stamp,
                                     log, control, verify);
    } catch (const boost::system::system_error&) {
        // Reporting to the peer is best effort.  The client already knows this
        // dialogue cannot continue, so a failed ERROR write must not relabel
        // that local result as an uncertain disconnect.
    }
    throw ClientTerminalResult(std::move(terminal), max_payload);
}

template <class Verify>
asio::awaitable<Frame> async_read_frame(tcp::socket& socket, uint32_t max_payload,
                                        CompletionStamp stamp, CompletionLog* log,
                                        Verify verify) {
    std::array<uint8_t, 4> raw_header{};
    const CompletionStamp expected_header =
        with_operation(stamp, AsyncOperationKind::ReadHeader);
    boost::system::error_code error;
    const size_t header_bytes = co_await asio::async_read(
        socket, asio::buffer(raw_header), asio::redirect_error(asio::use_awaitable, error));
    record_completion(log, expected_header, header_bytes, error);
    verify(expected_header);
    if (error)
        throw boost::system::system_error(error);
    const FrameHeader header = decode_frame_header(raw_header, max_payload);
    Frame frame{header.type, std::vector<uint8_t>(header.payload_bytes)};
    if (!frame.payload.empty()) {
        const CompletionStamp expected_payload =
            with_operation(stamp, AsyncOperationKind::ReadPayload);
        error.clear();
        const size_t payload_bytes = co_await asio::async_read(
            socket, asio::buffer(frame.payload), asio::redirect_error(asio::use_awaitable, error));
        record_completion(log, expected_payload, payload_bytes, error);
        verify(expected_payload);
        if (error)
            throw boost::system::system_error(error);
    }
    co_return frame;
}

template <class Verify>
asio::awaitable<void> async_wait_peer_close(tcp::socket& socket, CompletionStamp stamp,
                                            CompletionLog* log, Verify verify) {
    std::array<uint8_t, 1> unexpected{};
    const CompletionStamp expected =
        with_operation(stamp, AsyncOperationKind::WaitPeerClose);
    boost::system::error_code error;
    const size_t bytes = co_await socket.async_read_some(
        asio::buffer(unexpected), asio::redirect_error(asio::use_awaitable, error));
    record_completion(log, expected, bytes, error);
    verify(expected);
    if (!error && bytes != 0)
        throw std::invalid_argument("message arrived after final TX_COMMIT");
    if (error != asio::error::eof && error != asio::error::connection_reset)
        throw boost::system::system_error(error);
}

template <class Component, class Verify>
asio::awaitable<void> async_write_component(tcp::socket& socket, std::span<const uint8_t> bytes,
                                            uint32_t max_payload, CompletionStamp stamp,
                                            CompletionLog* log, EndpointIoControl& control,
                                            Verify verify) {
    if (bytes.empty()) {
        co_await async_write_message(socket, Component{}, max_payload, stamp, log, control,
                                     verify);
        co_return;
    }
    for (size_t offset = 0; offset != bytes.size();) {
        const size_t count = std::min<size_t>(max_payload, bytes.size() - offset);
        Component component;
        component.bytes.assign(bytes.begin() + offset, bytes.begin() + offset + count);
        co_await async_write_message(socket, component, max_payload, stamp, log, control, verify);
        offset += count;
    }
}

bool same_commit(const TxCommit& commit, const TxBegin& begin) {
    return commit.history_nonce == begin.history_nonce && commit.rel_seq == begin.rel_seq &&
           commit.tu_seq == begin.tu_seq && commit.transaction_digest == begin.transaction_digest &&
           commit.raw_digest == begin.raw_digest &&
           commit.post_state_digest ==
               compute_post_state_digest(begin.pre_state_digest, begin.history_nonce, begin.rel_seq,
                                         begin.tu_seq, begin.transaction_digest);
}

template <class T> T decode_as(const Frame& frame) {
    Message decoded = decode_payload(frame.type, frame.payload);
    T* value = std::get_if<T>(&decoded);
    if (!value)
        throw std::invalid_argument("unexpected Protocol-50 message type");
    return std::move(*value);
}

ActionRecord action_record(ActionType action, ActorSide actor, CStoreGuid c_guid, FStoreGuid f_guid,
                           uint64_t session_serial, const TxBegin* begin, HistoryNonce nonce,
                           RelSeq rel, Digest128 state_digest) {
    ActionRecord record;
    record.action = action;
    record.actor = actor;
    record.c_store_guid = c_guid;
    record.f_store_guid = f_guid;
    record.session_serial = session_serial;
    record.history_nonce = nonce;
    record.rel_seq = rel;
    record.state_digest = state_digest;
    if (begin) {
        record.history_nonce = begin->history_nonce;
        record.rel_seq = begin->rel_seq;
        record.tu_seq = begin->tu_seq;
        record.transaction_digest = begin->transaction_digest;
        record.raw_digest = begin->raw_digest;
        if (action != ActionType::INPUT_COMMITTED && action != ActionType::COMMIT_ACCEPTED &&
            action != ActionType::LOST_COMMIT_ACCEPTED)
            record.state_digest = begin->pre_state_digest;
        switch (action) {
        case ActionType::TX_BEGIN:
            record.stage_bytes = begin->dict.encoded_bytes + begin->body.encoded_bytes;
            break;
        case ActionType::DICT_COMPLETE:
            record.stage_bytes = begin->dict.encoded_bytes;
            break;
        case ActionType::BODY_COMPLETE:
            record.stage_bytes = begin->body.encoded_bytes;
            break;
        case ActionType::INPUT_MATERIALIZED:
            record.stage_bytes = begin->raw_bytes;
            break;
        default:
            break;
        }
    }
    return record;
}

} // namespace

std::string_view async_operation_name(AsyncOperationKind operation) {
    switch (operation) {
    case AsyncOperationKind::Accept:
        return "ACCEPT";
    case AsyncOperationKind::Connect:
        return "CONNECT";
    case AsyncOperationKind::ReadHeader:
        return "READ_HEADER";
    case AsyncOperationKind::ReadPayload:
        return "READ_PAYLOAD";
    case AsyncOperationKind::WriteFragment:
        return "WRITE_FRAGMENT";
    case AsyncOperationKind::WaitPeerClose:
        return "WAIT_PEER_CLOSE";
    }
    throw std::logic_error("unknown asynchronous operation");
}

struct P50PreparationAuthority::Impl {
    struct Entry {
        PrepareRequestKey request{};
        uint64_t raw_bytes = 0;
        Digest128 raw_digest{};
        std::vector<uint8_t> raw;
        PreparedInputPtr prepared;
        uint64_t references = 1;
        uint64_t retained_bytes = 0;
        bool committed = false;
    };

    Impl(CStoreGuid c_store_guid_value, ZstdTuLimits zstd_limits_value,
         PreparationAuthorityLimits authority_limits_value, int compression_level,
         ProfileId profile_value)
        : c_guid(c_store_guid_value), zstd_limits(zstd_limits_value),
          authority_limits(authority_limits_value), codec(compression_level),
#if defined(ICECC_P50_WITH_LIBBSC)
          grz_codec(),
#endif
          identity(std::make_shared<const uint8_t>(0)), route_codec(3),
          profile(profile_value) {
        if (c_guid == CStoreGuid{})
            throw std::invalid_argument("C preparation authority GUID zero is reserved");
        validate_zstd_tu_limits(zstd_limits);
        if (authority_limits.max_live_entries == 0 ||
            authority_limits.max_retained_encoded_bytes == 0)
            throw std::invalid_argument("preparation-authority limits must be nonzero");
        if (profile != ProfileId::P29 && profile != ProfileId::ZSTD_TU &&
            profile != ProfileId::Z3_LONG
#if defined(ICECC_P50_WITH_LIBBSC)
            && profile != ProfileId::GRZ
#endif
            )
            throw std::invalid_argument("preparation authority profile is unsupported");
        if (profile == ProfileId::P29) {
            p29_authority = std::make_unique<CAuthority>(c_guid);
            p29_route = std::make_unique<CRoute>(
                *p29_authority, FStoreGuid::from_u64(UINT64_C(0x503239434c49454e)),
                HistoryNonce{1});
        }
    }

    TuSeq next_tu_candidate() const {
        if (tu_exhausted)
            throw std::overflow_error("C preparation authority TU_SEQ space exhausted");
        return TuSeq{next_tu};
    }

    void consume_tu() {
        if (next_tu == std::numeric_limits<uint64_t>::max())
            tu_exhausted = true;
        else
            ++next_tu;
    }

    uint64_t next_entry_candidate() const {
        if (entry_exhausted)
            throw std::overflow_error("C preparation handle space exhausted");
        return next_entry;
    }

    void consume_entry() {
        if (next_entry == std::numeric_limits<uint64_t>::max())
            entry_exhausted = true;
        else
            ++next_entry;
    }

    CStoreGuid c_guid{};
    ZstdTuLimits zstd_limits{};
    PreparationAuthorityLimits authority_limits{};
    ZstdTuCodec codec;
#if defined(ICECC_P50_WITH_LIBBSC)
    GrzResidualCodec grz_codec;
#endif
    SingleThreadOwner owner;
    std::shared_ptr<const void> identity;
    uint64_t next_tu = 0;
    bool tu_exhausted = false;
    uint64_t next_entry = 1;
    bool entry_exhausted = false;
    uint64_t retained_bytes = 0;
    std::vector<uint8_t> committed_route_history;
    std::optional<uint64_t> uncommitted_route_entry;
#if defined(ICECC_P50_WITH_LIBBSC)
    std::optional<uint64_t> uncommitted_grz_entry;
#endif
    ZstdRouteCodec route_codec;
    std::unique_ptr<CAuthority> p29_authority;
    std::unique_ptr<CRoute> p29_route;
    ProfileId profile = ProfileId::ZSTD_TU;
    std::map<PrepareRequestKey, uint64_t> requests;
    std::map<uint64_t, Entry> entries;
};

P50PreparationAuthority::P50PreparationAuthority(
    CStoreGuid c_store_guid, ZstdTuLimits zstd_limits,
    PreparationAuthorityLimits authority_limits, int compression_level,
    ProfileId profile)
    : impl_(std::make_unique<Impl>(c_store_guid, zstd_limits, authority_limits,
                                   compression_level, profile)) {}

P50PreparationAuthority::~P50PreparationAuthority() = default;

PreparedTuHandle P50PreparationAuthority::prepare(PrepareRequestKey request,
                                                   std::span<const uint8_t> exact_input) {
    impl_->owner.require();
    if (request.producer_session == 0 || request.request_token == 0)
        throw std::invalid_argument("PrepareRequestKey zero fields are reserved");
    if (exact_input.size() > impl_->zstd_limits.max_raw_bytes)
        throw std::length_error("P50 raw input exceeds the local cap");
    const Digest128 raw_digest = digest128(exact_input);
    if (const auto request_position = impl_->requests.find(request);
        request_position != impl_->requests.end()) {
        const auto entry_position = impl_->entries.find(request_position->second);
        if (entry_position == impl_->entries.end())
            throw std::logic_error("preparation request index lost its retained entry");
        Impl::Entry& entry = entry_position->second;
        if (entry.raw_bytes != exact_input.size())
            throw std::invalid_argument("PrepareRequestKey was reused for different input");
        if (!std::equal(entry.raw.begin(), entry.raw.end(), exact_input.begin()))
            throw std::invalid_argument("PrepareRequestKey was reused for different input");
        return PreparedTuHandle(impl_->identity, entry_position->first);
    }

    if ((impl_->profile == ProfileId::Z3_LONG || impl_->profile == ProfileId::P29) &&
        impl_->uncommitted_route_entry.has_value())
        throw std::logic_error(
            "selected route profile requires its predecessor to commit before preparing the next TU");
#if defined(ICECC_P50_WITH_LIBBSC)
    if (impl_->profile == ProfileId::GRZ && impl_->uncommitted_grz_entry.has_value())
        throw std::logic_error(
            "GRZ_RESIDUAL requires its predecessor to commit before preparing the next TU");
#endif

    if (impl_->entries.size() >= impl_->authority_limits.max_live_entries)
        throw std::length_error("C preparation authority reached its live-entry bound");
    const uint64_t retained_room =
        impl_->authority_limits.max_retained_encoded_bytes - impl_->retained_bytes;
    if (retained_room == 0)
        throw std::length_error("C preparation authority reached its retained-byte bound");

    const TuSeq tu_seq = impl_->next_tu_candidate();
    const uint64_t entry_id = impl_->next_entry_candidate();
    ZstdTuLimits admission_limits = impl_->zstd_limits;
    admission_limits.max_encoded_body_bytes =
        std::min(admission_limits.max_encoded_body_bytes, retained_room);
    PreparedInputPtr prepared;
    bool p29_active_started = false;
    try {
        if (impl_->profile == ProfileId::ZSTD_TU) {
            const ZstdTuEnvelope envelope = impl_->codec.encode(
                HistoryNonce{1}, RelSeq{0}, tu_seq, Digest128{}, exact_input,
                admission_limits);
            prepared = std::make_shared<const PreparedInputEnvelope>(
                PreparedInputEnvelope{envelope.begin, {}, envelope.body, {}});
        } else if (impl_->profile == ProfileId::Z3_LONG) {
            const ZstdRouteEnvelope envelope = impl_->route_codec.encode(
                HistoryNonce{1}, RelSeq{0}, tu_seq, Digest128{},
                std::span<const uint8_t>(impl_->committed_route_history), exact_input,
                admission_limits);
            prepared = std::make_shared<const PreparedInputEnvelope>(
                PreparedInputEnvelope{envelope.begin, {}, envelope.body, {}});
        } else if (impl_->profile == ProfileId::P29) {
            std::vector<std::vector<uint8_t>> regions = p29_line_regions(exact_input);
            const PreparedTUPtr p29_prepared = impl_->p29_authority->prepare_from_regions(regions);
            residual_group::Codec residual_codec;
            residual_group::Kind residual_kind = residual_group::Kind::Zstd3;
            const std::vector<uint8_t> residual_input =
                impl_->p29_route->residual_input(p29_prepared);
            const std::vector<uint8_t> residual = residual_codec.encode(
                residual_input.data(), residual_input.size(), &residual_kind);
            const P29RootMode root_mode = impl_->p29_route->next_rel_seq().value == 0
                                              ? P29RootMode::HistoryIndependent
                                              : P29RootMode::RouteHistory;
            const CActiveTx& active = impl_->p29_route->begin(
                p29_prepared, root_mode, residual, true);
            p29_active_started = true;
            std::vector<FillRecord> fills;
            fills.reserve(active.manifest.size());
            for (Key64 key : active.manifest)
                fills.push_back(impl_->p29_authority->arena().object(key).fill_record());
            prepared = std::make_shared<const PreparedInputEnvelope>(
                PreparedInputEnvelope{active.begin, active.dict, active.body,
                                      std::move(fills)});
#if defined(ICECC_P50_WITH_LIBBSC)
        } else if (impl_->profile == ProfileId::GRZ) {
            const ZstdTuEnvelope envelope = impl_->grz_codec.encode(
                HistoryNonce{1}, RelSeq{0}, tu_seq, Digest128{}, exact_input,
                admission_limits);
            prepared = std::make_shared<const PreparedInputEnvelope>(
                PreparedInputEnvelope{envelope.begin, {}, envelope.body, {}});
#endif
        } else {
            throw std::logic_error("preparation authority profile is not runnable");
        }
    } catch (...) {
        if (p29_active_started && impl_->p29_route)
            impl_->p29_route->abandon_active();
#if defined(ICECC_P50_WITH_LIBBSC)
        if (impl_->profile == ProfileId::GRZ)
            impl_->grz_codec.discard();
#endif
        throw;
    }

    const uint64_t retained = static_cast<uint64_t>(prepared->body.size());
    bool entry_added = false;
    bool retained_added = false;
    try {
        if (retained > impl_->authority_limits.max_retained_encoded_bytes ||
            impl_->retained_bytes > impl_->authority_limits.max_retained_encoded_bytes - retained)
            throw std::length_error("C preparation authority reached its retained-byte bound");

        Impl::Entry entry{request, static_cast<uint64_t>(exact_input.size()), raw_digest,
                          std::vector<uint8_t>(exact_input.begin(), exact_input.end()), prepared, 1,
                          retained, false};
        const auto [entry_position, entry_inserted] = impl_->entries.emplace(
            entry_id, std::move(entry));
        (void)entry_position;
        if (!entry_inserted)
            throw std::logic_error("C preparation authority reused an entry identifier");
        entry_added = true;
        const auto [request_position, request_inserted] = impl_->requests.emplace(request, entry_id);
        (void)request_position;
        if (!request_inserted)
            throw std::logic_error("C preparation request was admitted twice");
        impl_->retained_bytes += retained;
        retained_added = true;
        if (impl_->profile == ProfileId::Z3_LONG)
            impl_->uncommitted_route_entry = entry_id;
        if (impl_->profile == ProfileId::P29)
            impl_->uncommitted_route_entry = entry_id;
#if defined(ICECC_P50_WITH_LIBBSC)
        if (impl_->profile == ProfileId::GRZ)
            impl_->uncommitted_grz_entry = entry_id;
#endif
        impl_->consume_tu();
        impl_->consume_entry();
        return PreparedTuHandle(impl_->identity, entry_id);
    } catch (...) {
        if (p29_active_started && impl_->p29_route)
            impl_->p29_route->abandon_active();
        if (retained_added)
            impl_->retained_bytes -= retained;
        impl_->requests.erase(request);
        if (entry_added)
            impl_->entries.erase(entry_id);
        if ((impl_->profile == ProfileId::Z3_LONG || impl_->profile == ProfileId::P29) &&
            impl_->uncommitted_route_entry == entry_id)
            impl_->uncommitted_route_entry.reset();
#if defined(ICECC_P50_WITH_LIBBSC)
        if (impl_->profile == ProfileId::GRZ) {
            if (impl_->uncommitted_grz_entry == entry_id)
                impl_->uncommitted_grz_entry.reset();
            impl_->grz_codec.discard();
        }
#endif
        throw;
    }
}

uint64_t P50PreparationAuthority::retain(PreparedTuHandle handle) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    if (position->second.references == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("prepared-TU reference count exhausted");
    return ++position->second.references;
}

uint64_t P50PreparationAuthority::release(PreparedTuHandle handle) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has already reached zero references");
    Impl::Entry& entry = position->second;
    if (--entry.references != 0)
        return entry.references;
    if (impl_->requests.erase(entry.request) != 1)
        throw std::logic_error("preparation request index lost its release target");
    if ((impl_->profile == ProfileId::Z3_LONG || impl_->profile == ProfileId::P29) && !entry.committed &&
        impl_->uncommitted_route_entry == handle.entry_id_)
    {
        impl_->uncommitted_route_entry.reset();
        if (impl_->profile == ProfileId::P29 && impl_->p29_route)
            impl_->p29_route->abandon_active();
    }
#if defined(ICECC_P50_WITH_LIBBSC)
    if (impl_->profile == ProfileId::GRZ && !entry.committed &&
        impl_->uncommitted_grz_entry == handle.entry_id_) {
        impl_->uncommitted_grz_entry.reset();
        impl_->grz_codec.discard();
    }
#endif
    impl_->retained_bytes -= entry.retained_bytes;
    impl_->entries.erase(position);
    return 0;
}

void P50PreparationAuthority::commit(PreparedTuHandle handle) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    auto& entry = position->second;
    if (impl_->profile == ProfileId::Z3_LONG && !entry.committed) {
        if (impl_->uncommitted_route_entry != handle.entry_id_)
            throw std::logic_error("ZSTD_ROUTE commit is not its prepared successor");
        const size_t limit = static_cast<size_t>(std::min<uint64_t>(
            impl_->zstd_limits.max_history_bytes,
            uint64_t{1} << impl_->zstd_limits.max_window_log));
        if (entry.raw.size() >= limit) {
            impl_->committed_route_history.assign(
                entry.raw.end() - static_cast<std::ptrdiff_t>(limit), entry.raw.end());
        } else {
            const size_t excess = impl_->committed_route_history.size() + entry.raw.size() > limit
                                      ? impl_->committed_route_history.size() + entry.raw.size() - limit
                                      : 0;
            if (excess != 0)
                impl_->committed_route_history.erase(
                    impl_->committed_route_history.begin(),
                    impl_->committed_route_history.begin() + excess);
            impl_->committed_route_history.insert(impl_->committed_route_history.end(),
                                                  entry.raw.begin(), entry.raw.end());
        }
        entry.committed = true;
        impl_->uncommitted_route_entry.reset();
    }
    if (impl_->profile == ProfileId::P29 && !entry.committed) {
        if (impl_->uncommitted_route_entry != handle.entry_id_)
            throw std::logic_error("P29 commit is not its prepared successor");
        const auto commit = TxCommit{entry.prepared->begin.history_nonce,
                                     entry.prepared->begin.rel_seq,
                                     entry.prepared->begin.tu_seq,
                                     entry.prepared->begin.transaction_digest,
                                     entry.prepared->begin.raw_digest,
                                     compute_post_state_digest(
                                         entry.prepared->begin.pre_state_digest,
                                         entry.prepared->begin.history_nonce,
                                         entry.prepared->begin.rel_seq,
                                         entry.prepared->begin.tu_seq,
                                         entry.prepared->begin.transaction_digest)};
        impl_->p29_route->accept_commit(commit);
        entry.committed = true;
        impl_->uncommitted_route_entry.reset();
    }
#if defined(ICECC_P50_WITH_LIBBSC)
    if (impl_->profile == ProfileId::GRZ && !entry.committed) {
        if (impl_->uncommitted_grz_entry != handle.entry_id_)
            throw std::logic_error("GRZ_RESIDUAL commit is not its prepared successor");
        impl_->grz_codec.commit();
        entry.committed = true;
        impl_->uncommitted_grz_entry.reset();
    }
#endif
}

CStoreGuid P50PreparationAuthority::c_store_guid() const {
    impl_->owner.check();
    return impl_->c_guid;
}

ZstdTuLimits P50PreparationAuthority::zstd_limits() const {
    impl_->owner.check();
    return impl_->zstd_limits;
}

bool P50PreparationAuthority::contains(PreparedTuHandle handle) const {
    impl_->owner.check();
    return handle.authority_.lock() == impl_->identity && handle.entry_id_ != 0 &&
           impl_->entries.contains(handle.entry_id_);
}

size_t P50PreparationAuthority::live_entry_count() const {
    impl_->owner.check();
    return impl_->entries.size();
}

uint64_t P50PreparationAuthority::retained_encoded_bytes() const {
    impl_->owner.check();
    return impl_->retained_bytes;
}

size_t P50PreparationAuthority::route_history_bytes() const {
    impl_->owner.check();
    return impl_->committed_route_history.size();
}

size_t P50PreparationAuthority::route_history_entries() const {
    impl_->owner.check();
    return impl_->committed_route_history.empty() ? 0 : 1;
}

ProfileId P50PreparationAuthority::profile() const {
    impl_->owner.check();
    return impl_->profile;
}

PreparedInputPtr
P50PreparationAuthority::resolve(PreparedTuHandle handle) const {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    return position->second.prepared;
}

void P50PreparationAuthority::validate_begin(const TxBegin& begin) const {
    impl_->owner.require();
    if (impl_->profile == ProfileId::ZSTD_TU)
        validate_zstd_tu_begin(begin, impl_->zstd_limits);
    else if (impl_->profile == ProfileId::Z3_LONG) {
        if (begin.profile != ProfileId::Z3_LONG)
            throw std::invalid_argument("prepared route profile differs from authority");
        // The route envelope has already been validated by its codec; retain
        // the profile and cap checks at the authority boundary as well.
        validate_zstd_tu_limits(impl_->zstd_limits);
        if (begin.body.encoded_bytes > impl_->zstd_limits.max_encoded_body_bytes ||
            begin.raw_bytes > impl_->zstd_limits.max_raw_bytes)
            throw std::length_error("prepared route exceeds authority limits");
    } else {
        if (begin.profile != ProfileId::P29 ||
            begin.p29_root_mode == P29RootMode::NotApplicable ||
            begin.dict.encoding != kP29KeyVectorEncoding ||
            (begin.body.encoding != kP29KeyVectorEncoding &&
             begin.body.encoding != kP29ResidualBodyEncoding))
            throw std::invalid_argument("prepared P29 profile differs from authority");
        if (begin.raw_bytes > impl_->zstd_limits.max_raw_bytes ||
            begin.body.encoded_bytes > impl_->zstd_limits.max_encoded_body_bytes ||
            begin.dict.encoded_bytes > impl_->zstd_limits.max_encoded_body_bytes)
            throw std::length_error("prepared P29 input exceeds authority limits");
    }
#if defined(ICECC_P50_WITH_LIBBSC)
    else {
        validate_grz_residual_begin(begin, impl_->zstd_limits);
    }
#endif
}

struct P50ClientEndpoint::Impl {
    struct Session {
        CStoreGuid c_guid{};
        std::optional<FStoreGuid> f_guid;
        uint64_t serial = 0;
        std::optional<HistoryNonce> provisional_nonce;
        RelSeq provisional_rel{};
    };

    struct Active {
        PreparedInputPtr prepared;
        PreparedTuHandle handle;
        TxBegin begin;
    };

    Impl(std::shared_ptr<P50PreparationAuthority> preparation_value, EndpointCaps cap_value,
         HistoryNonce first_nonce,
         CompletionLog* completion_log, ActionTrace* action_trace,
         std::optional<EndpointRunIdentity> run_identity_value,
         std::function<void(EndpointCancelPermit)> admitted_callback,
         std::function<void(EndpointCancelPermit, EndpointTerminalResult)>
             terminal_callback)
        : preparation(std::move(preparation_value)), caps(cap_value),
          next_nonce(first_nonce.value),
          completions(completion_log), actions(action_trace),
          run_identity_seed(std::move(run_identity_value)),
          on_run_admitted(std::move(admitted_callback)),
          on_run_terminal(std::move(terminal_callback)) {
        if (actions == nullptr && action_trace_sink_enabled()) {
            owned_actions = std::make_unique<ActionTrace>(1024);
            actions = owned_actions.get();
        }
        if (!preparation)
            throw std::invalid_argument("C endpoint requires its preparation authority");
        c_guid = preparation->c_store_guid();
        validate_caps(caps);
        if (caps.zstd != preparation->zstd_limits())
            throw std::invalid_argument(
                "C endpoint and preparation authority use different ZSTD_TU caps");
        if (caps.profile != preparation->profile())
            throw std::invalid_argument("C endpoint and preparation authority use different profiles");
        if (first_nonce.value == 0)
            throw std::invalid_argument("first endpoint HISTORY_NONCE must be nonzero");
    }

    uint64_t allocate_session() {
        if (session_exhausted)
            throw std::overflow_error("C endpoint session serial space exhausted");
        const uint64_t result = next_session;
        if (next_session == std::numeric_limits<uint64_t>::max())
            session_exhausted = true;
        else
            ++next_session;
        return result;
    }

    HistoryNonce allocate_nonce() {
        if (nonce_exhausted)
            throw std::overflow_error("C endpoint HISTORY_NONCE space exhausted");
        const HistoryNonce result{next_nonce};
        if (next_nonce == std::numeric_limits<uint64_t>::max())
            nonce_exhausted = true;
        else
            ++next_nonce;
        return result;
    }

    void advance_nonce_past(HistoryNonce observed) {
        if (nonce_exhausted || next_nonce > observed.value)
            return;
        if (observed.value == std::numeric_limits<uint64_t>::max()) {
            nonce_exhausted = true;
            return;
        }
        next_nonce = observed.value + 1;
    }

    CompletionStamp stamp(const Session& session, AsyncOperationKind operation) const {
        CompletionStamp result;
        result.actor = ActorSide::C;
        result.operation = operation;
        result.c_store_guid = session.c_guid;
        result.f_store_guid = session.f_guid.value_or(FStoreGuid{});
        result.session_serial = session.serial;
        if (session.provisional_nonce) {
            result.history_nonce = *session.provisional_nonce;
            result.rel_seq = session.provisional_rel;
        } else if (active) {
            result.history_nonce = active->begin.history_nonce;
            result.rel_seq = active->begin.rel_seq;
            result.tu_seq = active->begin.tu_seq;
            result.transaction_digest = active->begin.transaction_digest;
            result.raw_digest = active->begin.raw_digest;
            result.transaction_bound = true;
        } else if (route_known) {
            result.history_nonce = history_nonce;
            result.rel_seq = next_rel;
        }
        return result;
    }

    CompletionLiveIdentity live_identity(const Session& session) const {
        CompletionLiveIdentity result;
        result.actor = ActorSide::C;
        result.c_store_guid = c_guid;
        result.f_store_guid = session.f_guid.value_or(FStoreGuid{});
        result.session_serial = active_session;
        if (session.provisional_nonce) {
            result.history_nonce = *session.provisional_nonce;
            result.rel_seq = session.provisional_rel;
        } else if (active) {
            bind_live_identity(result, active->begin);
        } else if (route_known) {
            result.history_nonce = history_nonce;
            result.rel_seq = next_rel;
        }
        return result;
    }

    void record(ActionType action, const TxBegin& begin, uint64_t serial,
                Digest128 state_digest) {
        if (!actions || !f_guid)
            return;
        actions->record(action_record(action, ActorSide::C, c_guid, *f_guid, serial, &begin,
                                      begin.history_nonce, begin.rel_seq, state_digest));
    }

    void record_incarnation_replaced(FStoreGuid previous, FStoreGuid replacement,
                                     const TxBegin& retained, uint64_t serial) {
        if (!actions)
            return;
        ActionRecord value = action_record(ActionType::F_STORE_INCAR_REPLACED, ActorSide::C,
                                           c_guid, replacement, serial, &retained,
                                           retained.history_nonce, retained.rel_seq,
                                           retained.pre_state_digest);
        value.previous_f_store_guid = previous;
        actions->record(std::move(value));
    }

    void start_active(const PreparedInputPtr& prepared, PreparedTuHandle handle,
                      uint64_t serial) {
        if (!route_known || !f_guid)
            throw std::logic_error("cannot begin before a route is known");
        if (next_rel.value == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("C endpoint REL_SEQ space exhausted");
        Active value;
        value.prepared = prepared;
        value.handle = handle;
        value.begin.history_nonce = history_nonce;
        value.begin.rel_seq = next_rel;
        value.begin.tu_seq = prepared->begin.tu_seq;
        value.begin.profile = prepared->begin.profile;
        value.begin.p29_root_mode = prepared->begin.p29_root_mode;
        value.begin.pre_state_digest = state;
        value.begin.dict = prepared->begin.dict;
        value.begin.body = prepared->begin.body;
        value.begin.raw_bytes = prepared->begin.raw_bytes;
        value.begin.raw_digest = prepared->begin.raw_digest;
        value.begin.transaction_digest =
            compute_transaction_digest(value.begin, prepared->dict, prepared->body);
        preparation->validate_begin(value.begin);
        active = std::move(value);
        record(ActionType::TX_BEGIN, active->begin, serial, active->begin.pre_state_digest);
    }

    void accept(const TxCommit& commit, ActionType action, uint64_t serial) {
        if (!active || !same_commit(commit, active->begin))
            throw std::logic_error("TX_COMMIT does not close C's active transaction");
        record(action, active->begin, serial, commit.post_state_digest);
        preparation->commit(active->handle);
        state = commit.post_state_digest;
        if (next_rel.value == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("C endpoint REL_SEQ space exhausted");
        ++next_rel.value;
        active.reset();
        queued.reset();
        queued_handle = {};
    }

    CStoreGuid c_guid{};
    std::shared_ptr<P50PreparationAuthority> preparation;
    SingleThreadOwner owner;
    EndpointCaps caps{};
    uint64_t next_session = 1;
    bool session_exhausted = false;
    uint64_t active_session = 0;
    uint64_t next_nonce = 1;
    bool nonce_exhausted = false;
    std::optional<FStoreGuid> f_guid;
    bool route_known = false;
    HistoryNonce history_nonce{};
    RelSeq next_rel{};
    Digest128 state{};
    std::optional<Active> active;
    PreparedInputPtr queued;
    PreparedTuHandle queued_handle;
    CompletionLog* completions = nullptr;
    std::unique_ptr<ActionTrace> owned_actions;
    ActionTrace* actions = nullptr;
    EndpointRunRegistry endpoint_runs;
    std::optional<EndpointRunIdentity> run_identity_seed;
    std::function<void(EndpointCancelPermit)> on_run_admitted;
    std::function<void(EndpointCancelPermit, EndpointTerminalResult)> on_run_terminal;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    tcp::socket* socket_for_test_cancel = nullptr;
    bool active_cancel_requested = false;
    ClientCancellationDisposition active_cancellation =
        ClientCancellationDisposition::None;
#endif
};

struct P50ServerEndpoint::Impl {
    struct Pending {
        TxBegin begin;
        std::shared_ptr<ProfileDialogue> dialogue;
        bool materializing = false;
        bool global_tu_started = false;
        bool global_staged = false;
        CStoreGuid global_c_guid{};
        Key64 global_key{};
        size_t global_slot = 0;
        uint64_t reserved_encoded_bytes = 0;
        uint64_t reserved_raw_bytes = 0;
        uint64_t reserved_window_bytes = 0;
    };

    struct PreparedBegin {
        Pending pending;
        bool replay = false;
    };

    struct MaterializedInput {
        TxBegin begin;
        TxCommit commit;
        InputRecordStore::PreparedPublish prepared_input;
    };

    struct Route {
        HistoryNonce nonce{};
        RelSeq next_rel{};
        Digest128 state{};
        std::optional<TxCommit> last_commit;
        std::optional<TxBegin> interrupted;
        std::optional<Pending> pending;
        std::shared_ptr<ProfileDialogue> dialogue;
    };

    struct Namespace {
        bool established = false;
        uint64_t active_session = 0;
        uint64_t last_touch = 0;
        std::optional<HistoryNonce> nonce_high_water;
        std::optional<Route> route;
        std::optional<InputRecordKey> last_input;
    };

    struct Revision {
        uint64_t value = 0;
        bool exhausted = false;
    };

    struct LiveSession {
        FStoreGuid f_guid{};
        std::optional<daemon::P50FSessionOperationId> operation;
        std::optional<sidecar::AbsoluteMonotonicDeadline> deadline;
        std::optional<CStoreGuid> c_guid;
        uint64_t candidate_revision = 0;
        HistoryNonce candidate_nonce{};
        RelSeq candidate_rel{};
        bool activated = false;
    };

    struct Session {
        uint64_t serial = 0;
        FStoreGuid f_guid{};
        std::optional<daemon::P50FSessionOperationId> operation;
        std::optional<sidecar::AbsoluteMonotonicDeadline> deadline;
        std::optional<CStoreGuid> c_guid;
        uint64_t candidate_revision = 0;
        std::optional<SessionState> candidate_state;
        bool activated = false;
    };

    Impl(FStoreGuid f_guid_value, EndpointCaps cap_value, CompletionLog* completion_log,
         ActionTrace* action_trace, P50ServerEndpointConfig config_value)
        : f_guid(f_guid_value), caps(cap_value), completions(completion_log),
          actions(action_trace),
          input_records(config_value.owner_limits.max_retained_input_records,
                        config_value.owner_limits.max_retained_input_bytes),
          global_resources(std::make_unique<GlobalResourceModel>(
              global_resource_limits(config_value.owner_limits), GlobalResourceFaults{},
              config_value.global_resource_trace)),
          config(std::move(config_value)) {
        if (actions == nullptr && action_trace_sink_enabled()) {
            owned_actions = std::make_unique<ActionTrace>(1024);
            actions = owned_actions.get();
        }
        if (f_guid == FStoreGuid{})
            throw std::invalid_argument("F endpoint GUID zero is reserved");
        if (config.protocol_error_code == 0)
            throw std::invalid_argument("F endpoint ERROR code zero is reserved");
        if (config.endpoint_generation == 0 ||
            config.endpoint_generation >= KeyLayoutV1::generation_value_mask)
            throw std::invalid_argument(
                "F endpoint generation is outside the global key layout");
        validate_caps(caps);
        const P50ServerOwnerLimits& limits = config.owner_limits;
        if (limits.max_live_sessions == 0 || limits.max_namespaces == 0 ||
            limits.max_pending_encoded_bytes == 0 || limits.max_pending_raw_bytes == 0 ||
            limits.max_decoder_window_bytes == 0)
            throw std::invalid_argument("F endpoint aggregate owner limits must be nonzero");
        const uint64_t one_decoder_window = uint64_t{1} << caps.zstd.max_window_log;
        if (limits.max_decoder_window_bytes < one_decoder_window)
            throw std::invalid_argument(
                "F endpoint aggregate decoder-window limit admits no dialogue");
    }

    Session allocate_session(
        std::optional<daemon::P50FSessionOperationId> operation =
            std::nullopt,
        std::optional<sidecar::AbsoluteMonotonicDeadline> deadline =
            std::nullopt) {
        if (operation.has_value() != deadline.has_value() ||
            (operation.has_value() &&
             (!operation->valid() || !deadline->valid())))
            throw std::invalid_argument(
                "typed endpoint session operation/deadline binding is incomplete");
        if (live_sessions.size() >= config.owner_limits.max_live_sessions)
            throw std::length_error("F endpoint reached its live-session bound");
        if (session_exhausted)
            throw std::overflow_error("F endpoint session serial space exhausted");
        const uint64_t result = next_session;
        if (next_session == std::numeric_limits<uint64_t>::max())
            session_exhausted = true;
        else
            ++next_session;
        const auto inserted = live_sessions.emplace(
            result, LiveSession{.f_guid = f_guid,
                                .operation = operation,
                                .deadline = deadline,
                                .c_guid = std::nullopt,
                                .candidate_revision = 0,
                                .candidate_nonce = HistoryNonce{},
                                .candidate_rel = RelSeq{},
                                .activated = false});
        if (!inserted.second)
            throw std::logic_error("F endpoint reused a live session serial");
        return Session{.serial = result,
                       .f_guid = f_guid,
                       .operation = operation,
                       .deadline = deadline,
                       .c_guid = std::nullopt,
                       .candidate_revision = 0,
                       .candidate_state = std::nullopt,
                       .activated = false};
    }

    void require_incarnation(const Session& session) const {
        const auto position = live_sessions.find(session.serial);
        if (session.serial == 0 || session.f_guid != f_guid ||
            position == live_sessions.end() ||
            position->second.f_guid != session.f_guid ||
            position->second.operation != session.operation ||
            position->second.deadline != session.deadline)
            throw StaleCompletion();
    }

    void release_session(const Session& session) {
        const auto position = live_sessions.find(session.serial);
        if (position != live_sessions.end() &&
            position->second.f_guid == session.f_guid &&
            position->second.operation == session.operation &&
            position->second.deadline == session.deadline)
            live_sessions.erase(position);
    }

    static bool exceeds(uint64_t current, uint64_t addition, uint64_t limit) {
        return addition > limit || current > limit - addition;
    }

    uint64_t reserve_namespace_touch() {
        if (namespace_touch_exhausted)
            throw std::overflow_error(
                "F endpoint namespace LRU clock requires F_STORE_GUID replacement");
        const uint64_t result = next_namespace_touch;
        if (next_namespace_touch == std::numeric_limits<uint64_t>::max())
            namespace_touch_exhausted = true;
        else
            ++next_namespace_touch;
        return result;
    }

    Key64 global_key(TuSeq tu_seq) const {
        if (tu_seq.value == KeyLayoutV1::ordinal_mask)
            throw std::overflow_error("global resource key ordinal space exhausted");
        const auto key = Key64::make(
            ObjectType::Blob, static_cast<uint16_t>(config.endpoint_generation),
            tu_seq.value + 1);
        if (!key)
            throw std::overflow_error("global resource key cannot be represented");
        return *key;
    }

    void finish_global_pending(Pending& pending, bool crash) {
        if (pending.global_staged && crash) {
            global_resources->crash_install(pending.global_c_guid,
                                            pending.global_key,
                                            pending.global_slot);
            pending.global_staged = false;
        }
        if (pending.global_staged)
            throw std::logic_error("global pending object was not completed");
        if (pending.global_tu_started) {
            global_resources->finish_tu(pending.global_c_guid);
            pending.global_tu_started = false;
        }
    }

    void touch_namespace_on_disconnect(CStoreGuid c_guid, Namespace& space) {
        if (namespace_touch_exhausted)
            return;
        space.last_touch = next_namespace_touch;
        if (next_namespace_touch == std::numeric_limits<uint64_t>::max())
            namespace_touch_exhausted = true;
        else
            ++next_namespace_touch;
        global_resources->touch(c_guid);
    }

    bool namespace_has_live_session(CStoreGuid c_guid) const {
        return std::any_of(
            live_sessions.begin(), live_sessions.end(),
            [c_guid](const auto& item) {
                return item.second.c_guid && *item.second.c_guid == c_guid;
            });
    }

    bool namespace_is_evictable(CStoreGuid c_guid) const {
        const auto position = namespaces.find(c_guid);
        if (position == namespaces.end())
            return false;
        const Namespace& space = position->second;
        return space.active_session == 0 &&
               !namespace_has_live_session(c_guid) &&
               (!space.route ||
                (!space.route->pending && !space.route->interrupted)) &&
               input_records.namespace_evictable(c_guid);
    }

    std::vector<CStoreGuid> lru_evictable_namespaces(
        CStoreGuid excluded) const {
        std::vector<std::pair<uint64_t, CStoreGuid>> ordered;
        ordered.reserve(namespaces.size());
        for (const auto& [c_guid, space] : namespaces) {
            if (c_guid != excluded && namespace_is_evictable(c_guid))
                ordered.emplace_back(space.last_touch, c_guid);
        }
        std::sort(ordered.begin(), ordered.end());
        std::vector<CStoreGuid> result;
        result.reserve(ordered.size());
        for (const auto& [touch, c_guid] : ordered) {
            (void)touch;
            result.push_back(c_guid);
        }
        return result;
    }

    void evict_namespace_inputs(CStoreGuid c_guid) {
        if (!namespace_is_evictable(c_guid))
            throw std::logic_error(
                "F endpoint selected a live namespace for LRU eviction");
        for (const InputRecordKey key : input_records.namespace_keys(c_guid))
            global_resources->release(c_guid, global_key(key.tu_seq));
        input_records.evict_namespace(c_guid);
        namespaces.at(c_guid).last_input.reset();
    }

    void release_collected_global_inputs(
        const std::map<CStoreGuid, std::vector<InputRecordKey>>& before) {
        for (const auto& [c_guid, keys] : before)
            for (const InputRecordKey key : keys)
                if (!input_records.contains(key))
                    global_resources->release(c_guid, global_key(key.tu_seq));
    }

    void evict_whole_namespace(CStoreGuid c_guid) {
        if (!namespace_is_evictable(c_guid))
            throw std::logic_error(
                "F endpoint selected a live namespace for whole-namespace eviction");
        global_resources->evict(c_guid);
        input_records.evict_namespace(c_guid);
        if (namespaces.erase(c_guid) != 1)
            throw std::logic_error(
                "F endpoint lost its whole-namespace eviction target");
        revisions.erase(c_guid);
    }

    void ensure_input_capacity(CStoreGuid incoming, uint64_t raw_bytes) {
        const size_t record_limit = input_records.max_records();
        const uint64_t byte_limit = input_records.max_retained_bytes();
        if (raw_bytes > byte_limit)
            throw std::length_error("InputRecordStore byte limit exceeded");

        size_t retained_records = input_records.record_count();
        uint64_t retained_bytes = input_records.retained_bytes();
        const auto fits = [&] {
            return retained_records < record_limit &&
                   raw_bytes <= byte_limit - retained_bytes;
        };
        if (fits())
            return;

        std::vector<CStoreGuid> selected;
        for (const CStoreGuid candidate :
             lru_evictable_namespaces(incoming)) {
            const size_t records =
                input_records.namespace_record_count(candidate);
            const uint64_t bytes =
                input_records.namespace_retained_bytes(candidate);
            if (records > retained_records || bytes > retained_bytes)
                throw std::logic_error(
                    "F endpoint namespace LRU accounting underflow");
            retained_records -= records;
            retained_bytes -= bytes;
            selected.push_back(candidate);
            if (fits())
                break;
        }
        if (!fits())
            throw std::length_error(
                "InputRecordStore limit has no evictable namespace capacity");
        for (const CStoreGuid candidate : selected)
            evict_namespace_inputs(candidate);
    }

    void reserve_pending(Pending& pending) {
        const uint64_t encoded_bytes = pending.begin.body.encoded_bytes;
        const uint64_t raw_bytes = pending.begin.raw_bytes;
        const uint64_t window_bytes = uint64_t{1} << caps.zstd.max_window_log;
        const P50ServerOwnerLimits& limits = config.owner_limits;
        if (exceeds(pending_encoded_bytes, encoded_bytes,
                    limits.max_pending_encoded_bytes) ||
            exceeds(pending_raw_bytes, raw_bytes,
                    limits.max_pending_raw_bytes) ||
            exceeds(decoder_window_bytes, window_bytes,
                    limits.max_decoder_window_bytes))
            throw std::length_error("F endpoint reached an aggregate pending-input bound");
        pending_encoded_bytes += encoded_bytes;
        pending_raw_bytes += raw_bytes;
        decoder_window_bytes += window_bytes;
        pending.reserved_encoded_bytes = encoded_bytes;
        pending.reserved_raw_bytes = raw_bytes;
        pending.reserved_window_bytes = window_bytes;
    }

    void release_pending(Pending& pending) {
        finish_global_pending(pending, true);
        if (pending.reserved_encoded_bytes > pending_encoded_bytes ||
            pending.reserved_raw_bytes > pending_raw_bytes ||
            pending.reserved_window_bytes > decoder_window_bytes)
            throw std::logic_error("F endpoint pending-input accounting underflow");
        pending_encoded_bytes -= pending.reserved_encoded_bytes;
        pending_raw_bytes -= pending.reserved_raw_bytes;
        decoder_window_bytes -= pending.reserved_window_bytes;
        pending.reserved_encoded_bytes = 0;
        pending.reserved_raw_bytes = 0;
        pending.reserved_window_bytes = 0;
    }

    Namespace& require(const Session& session) {
        require_incarnation(session);
        if (!session.c_guid || !session.activated)
            throw StaleCompletion();
        auto position = namespaces.find(*session.c_guid);
        if (position == namespaces.end() || position->second.active_session != session.serial)
            throw StaleCompletion();
        return position->second;
    }

    const Namespace& require(const Session& session) const {
        require_incarnation(session);
        if (!session.c_guid || !session.activated)
            throw StaleCompletion();
        const auto position = namespaces.find(*session.c_guid);
        if (position == namespaces.end() || position->second.active_session != session.serial)
            throw StaleCompletion();
        return position->second;
    }

    CompletionStamp stamp(const Session& session, AsyncOperationKind operation,
                          const TxBegin* explicit_begin = nullptr) const {
        CompletionStamp result;
        result.actor = ActorSide::F;
        result.operation = operation;
        result.c_store_guid = session.c_guid.value_or(CStoreGuid{});
        result.f_store_guid = session.f_guid;
        result.cache_session_operation = session.operation;
        result.absolute_deadline = session.deadline;
        result.session_serial = session.serial;
        const TxBegin* begin = explicit_begin;
        if (!begin && session.activated && session.c_guid) {
            const auto position = namespaces.find(*session.c_guid);
            if (position != namespaces.end() && position->second.route) {
                const Route& route = *position->second.route;
                result.history_nonce = route.nonce;
                result.rel_seq = route.next_rel;
                if (route.pending)
                    begin = &route.pending->begin;
            }
        } else if (!begin && session.candidate_state &&
                   session.candidate_state->route_present) {
            result.history_nonce = session.candidate_state->history_nonce;
            result.rel_seq = session.candidate_state->next_rel_seq;
        }
        if (begin) {
            result.history_nonce = begin->history_nonce;
            result.rel_seq = begin->rel_seq;
            result.tu_seq = begin->tu_seq;
            result.transaction_digest = begin->transaction_digest;
            result.raw_digest = begin->raw_digest;
            result.transaction_bound = true;
        }
        return result;
    }

    CompletionLiveIdentity live_identity(const Session& session,
                                         const CompletionStamp& expected) const {
        require_incarnation(session);
        const LiveSession& current = live_sessions.at(session.serial);
        CompletionLiveIdentity result;
        result.actor = ActorSide::F;
        result.c_store_guid = current.c_guid.value_or(CStoreGuid{});
        result.f_store_guid = f_guid;
        result.cache_session_operation = current.operation;
        result.absolute_deadline = current.deadline;
        result.session_serial = session.serial;
        if (!current.c_guid)
            return result;
        if (!current.activated) {
            const auto revision = revisions.find(*current.c_guid);
            const bool absent_revision_is_zero =
                revision == revisions.end() && current.candidate_revision == 0;
            const bool matching_revision =
                revision != revisions.end() && !revision->second.exhausted &&
                revision->second.value == current.candidate_revision;
            if (!absent_revision_is_zero && !matching_revision)
                throw StaleCompletion();
            result.history_nonce = current.candidate_nonce;
            result.rel_seq = current.candidate_rel;
            return result;
        }
        if (!session.c_guid || *session.c_guid != *current.c_guid || !session.activated)
            throw StaleCompletion();
        const Namespace& space = require(session);
        if (!space.route)
            return result;
        const Route& route = *space.route;
        result.history_nonce = route.nonce;
        result.rel_seq = route.next_rel;
        if (route.pending) {
            bind_live_identity(result, route.pending->begin);
        } else if (expected.transaction_bound && route.last_commit) {
            bind_live_identity(result, *route.last_commit);
        }
        return result;
    }

    void record(ActionType action, const Session& session, const TxBegin* begin = nullptr,
                Digest128 state_override = {}) {
        if (!actions || !session.c_guid)
            return;
        HistoryNonce nonce{};
        RelSeq rel{};
        Digest128 state_value = state_override;
        const auto position = namespaces.find(*session.c_guid);
        if (position != namespaces.end() && position->second.route) {
            nonce = position->second.route->nonce;
            rel = position->second.route->next_rel;
            if (state_value == Digest128{})
                state_value = position->second.route->state;
        }
        ActionRecord value = action_record(action, ActorSide::F, *session.c_guid, f_guid,
                                           session.serial, begin, nonce, rel, state_value);
        if (action == ActionType::NEED_RECORDED) {
            value.need_keys.clear();
            value.remaining_need = 0;
        }
        if (action == ActionType::NEED_RECORDED)
            value.stage_bytes = value.need_keys.size() * sizeof(uint64_t);
        actions->record(std::move(value));
    }

    uint64_t current_revision(CStoreGuid c_guid) const {
        const auto position = revisions.find(c_guid);
        if (position == revisions.end())
            return 0;
        const Revision& revision = position->second;
        if (revision.exhausted)
            throw std::overflow_error(
                "candidate revision requires F_STORE_GUID replacement");
        return revision.value;
    }

    Revision& require_revision_advance(CStoreGuid c_guid) {
        Revision& revision = revisions[c_guid];
        if (revision.exhausted ||
            revision.value == std::numeric_limits<uint64_t>::max()) {
            revision.exhausted = true;
            throw std::overflow_error(
                "candidate revision requires F_STORE_GUID replacement");
        }
        return revision;
    }

    static void advance_revision(Revision& revision) noexcept {
        ++revision.value;
        if (revision.value == std::numeric_limits<uint64_t>::max())
            revision.exhausted = true;
    }

    void advance_revision(CStoreGuid c_guid) {
        Revision& revision = require_revision_advance(c_guid);
        advance_revision(revision);
    }

    void advance_revision_on_disconnect(CStoreGuid c_guid) noexcept {
        Revision& revision = revisions[c_guid];
        if (revision.exhausted)
            return;
        if (revision.value == std::numeric_limits<uint64_t>::max())
            revision.exhausted = true;
        else
            ++revision.value;
    }

    SessionState snapshot(CStoreGuid c_guid, SessionSelection selection) const {
        SessionState result;
        result.selected_protocol = selection.protocol;
        result.negotiated_profiles = selection.negotiated_profiles;
        result.limits = selection.limits;
        result.f_store_guid = f_guid;
        const auto position = namespaces.find(c_guid);
        if (position == namespaces.end())
            return result;
        const Namespace& space = position->second;
        result.namespace_present = space.established;
        result.route_present = space.route.has_value();
        if (space.route) {
            result.history_nonce = space.route->nonce;
            result.next_rel_seq = space.route->next_rel;
            result.state_digest = space.route->state;
            result.last_commit = space.route->last_commit;
        }
        return result;
    }

    SessionState stage(Session& session, CStoreGuid c_guid,
                       SessionSelection selection) {
        require_incarnation(session);
        if (c_guid == CStoreGuid{})
            throw std::invalid_argument("SESSION_HELLO C_STORE_GUID zero is reserved");
        const uint64_t revision = current_revision(c_guid);
        SessionState state = snapshot(c_guid, selection);
        session.c_guid = c_guid;
        session.candidate_revision = revision;
        session.candidate_state = state;
        LiveSession& live = live_sessions.at(session.serial);
        live.c_guid = c_guid;
        live.candidate_revision = revision;
        if (state.route_present) {
            live.candidate_nonce = state.history_nonce;
            live.candidate_rel = state.next_rel_seq;
        }
        return state;
    }

    void activate(Session& session) {
        require_incarnation(session);
        if (!session.c_guid || !session.candidate_state || session.activated)
            throw std::logic_error("F endpoint candidate cannot activate");
        LiveSession& live = live_sessions.at(session.serial);
        if (!live.c_guid || *live.c_guid != *session.c_guid || live.activated)
            throw StaleCompletion();
        const uint64_t touch = reserve_namespace_touch();
        const bool new_namespace = !namespaces.contains(*session.c_guid);
        if (new_namespace &&
            namespaces.size() >= config.owner_limits.max_namespaces) {
            const std::vector<CStoreGuid> candidates =
                lru_evictable_namespaces(*session.c_guid);
            if (candidates.empty())
                throw std::length_error(
                    "F endpoint C-namespace bound has no evictable namespace");
            evict_whole_namespace(candidates.front());
        }
        auto [position, namespace_inserted] = namespaces.try_emplace(*session.c_guid);
        if (namespace_inserted) {
            try {
                global_resources->admit(
                    *session.c_guid,
                    static_cast<uint16_t>(config.endpoint_generation));
            } catch (...) {
                namespaces.erase(position);
                throw;
            }
        }
        std::map<CStoreGuid, Revision>::iterator revision;
        bool revision_inserted = false;
        try {
            std::tie(revision, revision_inserted) = revisions.try_emplace(*session.c_guid);
        } catch (...) {
            if (namespace_inserted)
                global_resources->evict(*session.c_guid);
            if (namespace_inserted)
                namespaces.erase(position);
            throw;
        }
        if (revision->second.exhausted ||
            revision->second.value != session.candidate_revision) {
            if (revision_inserted)
                revisions.erase(revision);
            if (namespace_inserted)
                namespaces.erase(position);
            throw StaleCompletion();
        }
        advance_revision(revision->second);

        Namespace& space = position->second;
        const bool replaced = !namespace_inserted && space.active_session != 0;
        if (space.route && space.route->pending) {
            space.route->interrupted = space.route->pending->begin;
            if (space.route->pending->dialogue)
                space.route->pending->dialogue->discard_tentative();
            release_pending(*space.route->pending);
            space.route->pending.reset();
        }
        space.active_session = session.serial;
        space.last_touch = touch;
        global_resources->touch(*session.c_guid);
        session.activated = true;
        live.activated = true;
        session.candidate_state.reset();
        record(replaced ? ActionType::SESSION_REPLACED : ActionType::SESSION_OPENED, session);
    }

    void disconnect(const Session& session, bool retain_interrupted) {
        if (!session.c_guid)
            return;
        auto position = namespaces.find(*session.c_guid);
        if (position == namespaces.end() || position->second.active_session != session.serial)
            return;
        Namespace& space = position->second;
        if (space.route && space.route->pending) {
            if (retain_interrupted)
                space.route->interrupted = space.route->pending->begin;
            else
                space.route->interrupted.reset();
            if (space.route->pending->dialogue)
                space.route->pending->dialogue->discard_tentative();
            release_pending(*space.route->pending);
            space.route->pending.reset();
        }
        record(ActionType::SESSION_DISCONNECTED, session);
        space.active_session = 0;
        touch_namespace_on_disconnect(*session.c_guid, space);
        advance_revision_on_disconnect(*session.c_guid);
    }

    SessionState session_state(const Session& session, SessionSelection selection) const {
        const Namespace& space = require(session);
        SessionState result;
        result.selected_protocol = selection.protocol;
        result.negotiated_profiles = selection.negotiated_profiles;
        result.limits = selection.limits;
        result.f_store_guid = f_guid;
        result.namespace_present = space.established;
        result.route_present = space.route.has_value();
        if (space.route) {
            result.history_nonce = space.route->nonce;
            result.next_rel_seq = space.route->next_rel;
            result.state_digest = space.route->state;
            result.last_commit = space.route->last_commit;
        }
        return result;
    }

    void validate_history_reset(CStoreGuid c_guid, const Namespace* space,
                                const HistoryReset& reset) const {
        if (space && space->route &&
            (space->route->pending || space->route->interrupted))
            throw std::logic_error(
                "HISTORY_RESET arrived while F retained transaction identity");
        if (reset.initial_state_digest !=
            initial_route_digest(c_guid, reset.history_nonce))
            throw std::invalid_argument("HISTORY_RESET digest was not derived locally");
        if (space && space->nonce_high_water &&
            reset.history_nonce.value <= space->nonce_high_water->value)
            throw std::invalid_argument("HISTORY_RESET nonce did not advance monotonically");
    }

    void validate_history_reset_candidate(const Session& session,
                                          const HistoryReset& reset) const {
        require_incarnation(session);
        if (!session.c_guid || session.activated)
            throw StaleCompletion();
        const auto position = namespaces.find(*session.c_guid);
        validate_history_reset(*session.c_guid,
                               position == namespaces.end() ? nullptr : &position->second,
                               reset);
    }

    void reset_history(const Session& session, const HistoryReset& reset) {
        Namespace& space = require(session);
        validate_history_reset(*session.c_guid, &space, reset);
        space.established = true;
        space.nonce_high_water = reset.history_nonce;
        space.route = Route{.nonce = reset.history_nonce,
                            .next_rel = RelSeq{0},
                            .state = reset.initial_state_digest,
                            .last_commit = std::nullopt,
                            .interrupted = std::nullopt,
                            .pending = std::nullopt,
                            .dialogue = nullptr};
        record(ActionType::HISTORY_RESET, session);
    }

    PreparedBegin prepare_begin(const Namespace& space, const TxBegin& begin,
                                uint32_t negotiated_profiles,
                                CStoreGuid c_store_guid) const {
        if (!space.route)
            throw std::logic_error("TX_BEGIN arrived before HISTORY_RESET");
        const Route& route = *space.route;
        if (begin.history_nonce != route.nonce || begin.rel_seq != route.next_rel ||
            begin.pre_state_digest != route.state)
            throw std::logic_error("TX_BEGIN differs from the F route cursor");
        if (route.next_rel.value == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("F endpoint REL_SEQ space exhausted");
        const uint32_t transaction_profile_bit = profile_bit(begin.profile);
        if (transaction_profile_bit == 0 ||
            (transaction_profile_bit & negotiated_profiles) != transaction_profile_bit)
            throw std::invalid_argument("TX_BEGIN profile was not negotiated");
        if (route.pending)
            throw std::logic_error("F endpoint already has one active transaction");
        if (route.interrupted && *route.interrupted != begin)
            throw std::logic_error(
                "TX_BEGIN differs from the interrupted transaction identity");
        PreparedBegin result;
        result.replay = route.interrupted.has_value();
        result.pending.begin = begin;
        result.pending.dialogue = std::make_shared<ProfileDialogue>(
            ProfileDialogue::create(
                begin.profile,
                ProfileDialogueConfig{.negotiated_profiles = negotiated_profiles,
                                      .c_store_guid = c_store_guid,
                                      .max_encoded_body_bytes = caps.zstd.max_encoded_body_bytes,
                                      .max_raw_bytes = caps.zstd.max_raw_bytes,
                                      .max_window_log = caps.zstd.max_window_log,
                                      .max_history_bytes = caps.zstd.max_history_bytes}));
        if ((begin.profile == ProfileId::P29 || begin.profile == ProfileId::Z3_LONG
#if defined(ICECC_P50_WITH_LIBBSC)
             || begin.profile == ProfileId::GRZ
#endif
             ) && route.dialogue)
            result.pending.dialogue = route.dialogue;
        return result;
    }

    PreparedBegin prepare_begin_candidate(const Session& session, const TxBegin& begin,
                                          uint32_t negotiated_profiles) const {
        require_incarnation(session);
        if (!session.c_guid || session.activated)
            throw StaleCompletion();
        const auto position = namespaces.find(*session.c_guid);
        if (position == namespaces.end())
            throw std::logic_error("TX_BEGIN arrived before HISTORY_RESET");
        return prepare_begin(position->second, begin, negotiated_profiles, *session.c_guid);
    }

    bool install_begin(const Session& session, PreparedBegin prepared) {
        Namespace& space = require(session);
        Route& route = *space.route;
        if (route.pending)
            throw std::logic_error("F endpoint already has one active transaction");
        if (route.interrupted && route.interrupted != prepared.pending.begin)
            throw StaleCompletion();
        if (prepared.pending.begin.profile == ProfileId::P29 ||
            prepared.pending.begin.profile == ProfileId::Z3_LONG
#if defined(ICECC_P50_WITH_LIBBSC)
            || prepared.pending.begin.profile == ProfileId::GRZ
#endif
            ) {
            if (!route.dialogue)
                route.dialogue = prepared.pending.dialogue;
            else
                prepared.pending.dialogue = route.dialogue;
        }
        const TxBegin begin = prepared.pending.begin;
        prepared.pending.dialogue->begin(begin);
        const bool replay = prepared.replay;
        prepared.pending.global_c_guid = *session.c_guid;
        try {
            global_resources->start_tu(*session.c_guid);
            prepared.pending.global_tu_started = true;
            reserve_pending(prepared.pending);
            const InputRecordKey input_key{*session.c_guid, begin.tu_seq};
            if (!input_records.contains(input_key) && begin.raw_bytes != 0) {
                const auto slot = global_resources->first_free_staging_slot();
                if (!slot)
                    throw std::length_error("F endpoint staging-slot pool is exhausted");
                prepared.pending.global_key = global_key(begin.tu_seq);
                prepared.pending.global_slot = *slot;
                global_resources->begin_install(
                    *session.c_guid, prepared.pending.global_key,
                    begin.raw_digest, begin.raw_bytes, *slot, replay);
                prepared.pending.global_staged = true;
            }
        } catch (...) {
            release_pending(prepared.pending);
            throw;
        }
        route.interrupted.reset();
        route.pending = std::move(prepared.pending);
        record(replay ? ActionType::ACTIVE_REPLAYED : ActionType::TX_BEGIN, session, &begin);
        record(ActionType::DICT_COMPLETE, session, &begin);
        record(ActionType::NEED_RECORDED, session, &begin);
        return replay;
    }

    bool begin(const Session& session, const TxBegin& begin,
               uint32_t negotiated_profiles) {
        const Namespace& space = require(session);
        return install_begin(session,
                             prepare_begin(space, begin, negotiated_profiles,
                                           *session.c_guid));
    }

    void append_body(const Session& session, BodyMessage message) {
        Namespace& space = require(session);
        if (!space.route || !space.route->pending)
            throw std::logic_error("BODY has no F active transaction");
        Pending& pending = *space.route->pending;
        pending.dialogue->append_body(message);
        if (pending.dialogue->state() == ProfileDialogueState::BodyClosed)
            record(ActionType::BODY_COMPLETE, session, &pending.begin);
    }

    void append_dict(const Session& session, const DictMessage& message) {
        Namespace& space = require(session);
        if (!space.route || !space.route->pending)
            throw std::logic_error("DICT has no F active transaction");
        space.route->pending->dialogue->append_dict(message);
    }

    void receive_need(const Session& session, const NeedMessage& message) {
        Namespace& space = require(session);
        if (!space.route || !space.route->pending)
            throw std::logic_error("NEED has no F active transaction");
        space.route->pending->dialogue->receive_need(message);
    }

    void receive_fill(const Session& session, const FillMessage& message) {
        Namespace& space = require(session);
        if (!space.route || !space.route->pending)
            throw std::logic_error("FILL has no F active transaction");
        space.route->pending->dialogue->receive_fill(message);
    }

    bool body_complete(const Session& session) const {
        const Namespace& space = require(session);
        if (!space.route || !space.route->pending)
            throw std::logic_error("BODY has no F active transaction");
        return space.route->pending->dialogue->state() ==
               ProfileDialogueState::BodyClosed;
    }

    ServerMaterializationJob begin_materialization(
        const Session& session, std::function<void()> before_materialize) {
        Namespace& space = require(session);
        if (!space.route || !space.route->pending)
            throw std::logic_error("F endpoint has no active transaction");
        Pending& pending = *space.route->pending;
        if (pending.materializing || !pending.dialogue ||
            pending.dialogue->state() != ProfileDialogueState::BodyClosed)
            throw std::logic_error("input cannot materialize before BODY closure");
        if (!session.c_guid)
            throw StaleCompletion();
        const TxBegin begin = pending.begin;
        const TxCommit commit{
            begin.history_nonce,
            begin.rel_seq,
            begin.tu_seq,
            begin.transaction_digest,
            begin.raw_digest,
            compute_post_state_digest(begin.pre_state_digest,
                                      begin.history_nonce, begin.rel_seq,
                                      begin.tu_seq,
                                      begin.transaction_digest)};
        pending.materializing = true;
        return ServerMaterializationJob{
            .c_store_guid = *session.c_guid,
            .begin = begin,
            .commit = commit,
            .dialogue = pending.dialogue,
            .before_materialize = std::move(before_materialize)};
    }

    MaterializedInput finish_materialization(
        const Session& session,
        ServerMaterializationCompletion completion) {
        Namespace& space = require(session);
        if (!space.route || !space.route->pending)
            throw StaleCompletion();
        Pending& pending = *space.route->pending;
        if (!pending.materializing || !pending.dialogue ||
            pending.begin != completion.begin)
            throw StaleCompletion();
        pending.materializing = false;
        if (completion.failure)
            std::rethrow_exception(completion.failure);
        if (!pending.dialogue ||
            pending.dialogue->state() != ProfileDialogueState::Materialized ||
            !completion.prepared_input.valid() ||
            completion.prepared_input.key() !=
                InputRecordKey{*session.c_guid, pending.begin.tu_seq})
            throw StaleCompletion();
        record(ActionType::INPUT_MATERIALIZED, session, &pending.begin);
        return MaterializedInput{completion.begin, completion.commit,
                                 std::move(completion.prepared_input)};
    }

    InputJobState select_materialized_job_state(
        const Session& session, const MaterializedInput& materialized) {
        Namespace& space = require(session);
        if (!space.route || !space.route->pending)
            throw std::logic_error("F endpoint has no active transaction");
        const Pending& pending = *space.route->pending;
        if (pending.materializing || !pending.dialogue ||
            pending.dialogue->state() != ProfileDialogueState::Materialized ||
            pending.begin != materialized.begin ||
            !materialized.prepared_input.valid())
            throw StaleCompletion();
        return config.input_job_state
                   ? config.input_job_state(
                         *session.c_guid, materialized.begin,
                         materialized.commit,
                         materialized.prepared_input.exact_input())
                   : InputJobState::Open;
    }

    TxCommit commit_materialized(
        const Session& session, MaterializedInput materialized,
        InputJobState job_state,
        std::optional<InputRecordKey>& candidate_input,
        std::optional<InputRecordKey>& completed_input,
        std::optional<InputRecordKey>& committed_input) {
        Namespace& space = require(session);
        if (!space.route || !space.route->pending)
            throw std::logic_error("F endpoint has no active transaction");
        Route& route = *space.route;
        Pending& pending = *route.pending;
        if (pending.materializing || !pending.dialogue ||
            pending.dialogue->state() != ProfileDialogueState::Materialized ||
            pending.begin != materialized.begin)
            throw StaleCompletion();
        Revision& revision = require_revision_advance(*session.c_guid);
        const uint64_t touch = reserve_namespace_touch();
        const InputRecordKey input_key = materialized.prepared_input.key();
        candidate_input = input_key;
        if (job_state == InputJobState::Open &&
            !input_records.contains(input_key))
            ensure_input_capacity(*session.c_guid,
                                  materialized.begin.raw_bytes);
        if (pending.global_staged) {
            if (job_state == InputJobState::Open)
                global_resources->publish(*session.c_guid, pending.global_key,
                                          pending.global_slot,
                                          materialized.begin.raw_digest);
            else
                global_resources->crash_install(*session.c_guid,
                                                pending.global_key,
                                                pending.global_slot);
            pending.global_staged = false;
        }
        const InputPublishResult publication =
            job_state == InputJobState::Open
                ? input_records.commit_prepared(
                      std::move(materialized.prepared_input))
                : input_records.observe_closed_job_commit(
                      std::move(materialized.prepared_input));
        finish_global_pending(pending, false);
        completed_input = input_key;
        if (job_state == InputJobState::Open &&
            publication != InputPublishResult::NotRetainedJobClosed &&
            input_records.job_open(input_key)) {
            space.last_input = input_key;
            committed_input = input_key;
        } else {
            space.last_input.reset();
        }
        if (config.on_input_committed)
            config.on_input_committed(input_key, committed_input.has_value());
        advance_revision(revision);
        route.state = materialized.commit.post_state_digest;
        ++route.next_rel.value;
        route.last_commit = materialized.commit;
        route.interrupted.reset();
        pending.dialogue->commit_visible(materialized.commit);
        release_pending(pending);
        route.pending.reset();
        space.last_touch = touch;
        record(ActionType::INPUT_COMMITTED, session, &materialized.begin,
               materialized.commit.post_state_digest);
        return materialized.commit;
    }

    FStoreGuid f_guid{};
    EndpointCaps caps{};
    SingleThreadOwner owner;
    uint64_t next_session = 1;
    bool session_exhausted = false;
    uint64_t next_namespace_touch = 1;
    bool namespace_touch_exhausted = false;
    std::map<uint64_t, LiveSession> live_sessions;
    std::map<CStoreGuid, Namespace> namespaces;
    std::map<CStoreGuid, Revision> revisions;
    uint64_t pending_encoded_bytes = 0;
    uint64_t pending_raw_bytes = 0;
    uint64_t decoder_window_bytes = 0;
    CompletionLog* completions = nullptr;
    std::unique_ptr<ActionTrace> owned_actions;
    ActionTrace* actions = nullptr;
    InputRecordStore input_records;
    std::unique_ptr<GlobalResourceModel> global_resources;
    P50ServerEndpointConfig config{};
    EndpointRunRegistry endpoint_runs;
    uint64_t next_run_sequence = 1;
    uint64_t next_socket_ownership_generation = 1;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    ServerIoState* active_io = nullptr;
#endif
};

// The live-session row is inserted before the shared reducer coroutine is
// created. Move this lease into that coroutine so allocation failure,
// cancellation before entry, or owner-affinity rejection cannot strand it.
class P50ServerEndpoint::SessionRegistration {
public:
    SessionRegistration(Impl& owner, Impl::Session session) noexcept
        : owner_(&owner), session_(session) {}

    ~SessionRegistration() { reset(); }

    SessionRegistration(const SessionRegistration&) = delete;
    SessionRegistration& operator=(const SessionRegistration&) = delete;

    SessionRegistration(SessionRegistration&& other) noexcept
        : owner_(other.owner_), session_(other.session_) {
        other.owner_ = nullptr;
    }

    SessionRegistration& operator=(SessionRegistration&& other) noexcept {
        if (this != &other) {
            reset();
            owner_ = other.owner_;
            session_ = other.session_;
            other.owner_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] uint64_t serial() const noexcept { return session_.serial; }
    [[nodiscard]] const Impl::Session& session() const noexcept {
        return session_;
    }

    void reset() noexcept {
        if (owner_ != nullptr) {
            owner_->release_session(session_);
            owner_ = nullptr;
        }
    }

private:
    Impl* owner_ = nullptr;
    Impl::Session session_{};
};

void require_outbound_profile_negotiated(uint32_t negotiated_profiles,
                                         const TxBegin& begin) {
    if ((negotiated_profiles & profile_bit(begin.profile)) == 0)
        throw std::logic_error("C selected a profile outside the negotiated mask");
}

P50ClientEndpoint::P50ClientEndpoint(std::shared_ptr<P50PreparationAuthority> preparation,
                                     EndpointCaps caps, HistoryNonce first_history_nonce,
                                     CompletionLog* completions, ActionTrace* actions,
                                     std::optional<EndpointRunIdentity> run_identity_seed,
                                     std::function<void(EndpointCancelPermit)> on_run_admitted,
                                     std::function<void(EndpointCancelPermit,
                                                        EndpointTerminalResult)> on_run_terminal)
    : impl_(std::make_unique<Impl>(std::move(preparation), caps, first_history_nonce, completions,
                                   actions, std::move(run_identity_seed),
                                   std::move(on_run_admitted),
                                   std::move(on_run_terminal))) {}

P50ClientEndpoint::~P50ClientEndpoint() = default;

boost::asio::awaitable<ClientRunResult> P50ClientEndpoint::run(
    tcp::endpoint remote, PreparedTuHandle prepared, EndpointIoControl control,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    co_return co_await run_connected(remote, std::nullopt, std::move(prepared),
                                     std::move(control), deadline);
}

boost::asio::awaitable<ClientRunResult> P50ClientEndpoint::run(
    tcp::socket socket, PreparedTuHandle prepared, EndpointIoControl control,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    co_return co_await run_connected(std::nullopt, std::optional<tcp::socket>(std::move(socket)),
                                     std::move(prepared), std::move(control), deadline);
}

boost::asio::awaitable<ClientRunResult> P50ClientEndpoint::run_adopted_fd(
    int fd, PreparedTuHandle prepared, EndpointIoControl control,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    const auto executor = co_await asio::this_coro::executor;
    boost::system::error_code error;
    std::optional<tcp::socket> socket = adopt_connected_fd(executor, fd, error);
    if (!socket) {
        ClientRunResult result;
        if (deadline && *deadline <= std::chrono::steady_clock::now())
            result.status = ClientRunStatus::DeadlineExceeded;
        co_return result;
    }
    co_return co_await run(std::move(*socket), std::move(prepared), std::move(control), deadline);
}

boost::asio::awaitable<ClientRunResult> P50ClientEndpoint::run_connected(
    std::optional<tcp::endpoint> remote, std::optional<tcp::socket> adopted,
    PreparedTuHandle prepared, EndpointIoControl control,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    impl_->owner.require();
    if (deadline && *deadline <= std::chrono::steady_clock::now()) {
        if (adopted && adopted->is_open())
            close_now(*adopted);
        ClientRunResult result;
        result.status = ClientRunStatus::DeadlineExceeded;
        co_return result;
    }
    if (!remote) {
        if (!adopted || !validate_adopted_socket(*adopted)) {
            if (adopted && adopted->is_open())
                close_now(*adopted);
            co_return ClientRunResult{};
        }
    }
    if (impl_->active_session != 0)
        throw std::logic_error("C endpoint already has one active dialogue");
    PreparedInputPtr admitted;
    if (prepared)
        admitted = impl_->preparation->resolve(prepared);
    if (impl_->active) {
        if (admitted && admitted != impl_->active->prepared)
            throw std::invalid_argument("retry supplied a different PreparedInput");
    } else if (admitted) {
        if (impl_->queued && admitted != impl_->queued)
            throw std::invalid_argument("C endpoint already has different queued work");
        impl_->queued = std::move(admitted);
        impl_->queued_handle = prepared;
    }
    if (!impl_->active && !impl_->queued)
        throw std::invalid_argument("C endpoint run has no prepared transaction");

    Impl::Session session{impl_->c_guid, impl_->f_guid, impl_->allocate_session(), std::nullopt,
                          RelSeq{}};
    const uint64_t serial = session.serial;
    impl_->active_session = session.serial;
    const auto executor = co_await asio::this_coro::executor;
    auto io = std::make_shared<ClientIoState>(executor);
    if (adopted)
        io->socket = std::move(*adopted);
    tcp::socket& socket = io->socket;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    struct ActiveSocketGuard {
        Impl& owner;
        tcp::socket* socket;
        ~ActiveSocketGuard() {
            if (owner.socket_for_test_cancel == socket) {
                owner.socket_for_test_cancel = nullptr;
                owner.active_cancel_requested = false;
                owner.active_cancellation = ClientCancellationDisposition::None;
            }
        }
    } socket_cancel_guard{*impl_, &socket};
    impl_->socket_for_test_cancel = &socket;
    impl_->active_cancel_requested = false;
    impl_->active_cancellation = ClientCancellationDisposition::None;
#endif
    struct ClientRunLeaseGuard {
        EndpointRunRegistry* registry = nullptr;
        std::optional<EndpointRunIdentity> identity;
        std::optional<EndpointCancelPermit> permit;
        std::function<void(EndpointCancelPermit, EndpointTerminalResult)> terminal;
        ~ClientRunLeaseGuard() {
            if (registry && identity && permit) {
                const EndpointTerminalResult terminal{
                    EndpointTerminalResultState::Failed, 0};
                if (registry->mark_terminal(*identity, terminal)) {
                    if (this->terminal) {
                        try {
                            this->terminal(*permit, terminal);
                        } catch (...) {
                            // Terminal publication is an owner callback; a
                            // callback failure cannot strand endpoint state.
                        }
                    }
                    (void)registry->consume_terminal(*identity);
                }
            }
        }
    } run_lease{&impl_->endpoint_runs, std::nullopt, std::nullopt,
                impl_->on_run_terminal};
    if (impl_->run_identity_seed && impl_->run_identity_seed->valid() &&
        deadline) {
        EndpointRunIdentity identity = *impl_->run_identity_seed;
        identity.endpoint_session_serial = serial;
        auto handle = impl_->endpoint_runs.admit(
            identity, impl_->run_identity_seed->sidecar_launch.valid()
                         ? sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
                               *deadline,
                               impl_->run_identity_seed->sidecar_launch.identity.generation,
                               impl_->run_identity_seed->sidecar_launch.identity.attempt)
                         : sidecar::AbsoluteMonotonicDeadline{},
            std::make_shared<ClientRunSocketTarget>(io));
        if (handle) {
            run_lease.identity = identity;
            run_lease.permit = handle->permit(EndpointCancelReason::CallerRequested);
            (void)impl_->endpoint_runs.set_phase(identity, EndpointRunPhase::CacheWire);
            if (impl_->on_run_admitted)
                impl_->on_run_admitted(*run_lease.permit);
        }
    }
    if (deadline) {
        io->timer.expires_at(*deadline);
        io->timer.async_wait([io](const boost::system::error_code& error) {
            if (error)
                return;
            io->expired = true;
            if (!io->committed)
                close_now(io->socket);
        });
    }
    const auto deadline_crossed = [&]() {
        return deadline.has_value() &&
               std::chrono::steady_clock::now() >= *deadline;
    };
    const auto require_deadline = [&]() {
        if (deadline_crossed())
            throw boost::system::system_error(asio::error::timed_out);
    };
    ClientRunResult result;
    uint32_t terminal_cap = impl_->caps.wire.max_frame_payload;
    const auto verify = [&](const CompletionStamp& expected) {
        impl_->owner.require();
        require_deadline();
        CompletionStamp observed = expected;
        if (control.before_completion_check)
            control.before_completion_check(observed);
        observed = completion_for_test(observed, control);
        require_observed_completion(expected, observed);
        CompletionLiveIdentity live = impl_->live_identity(session);
        if (control.before_live_identity_check)
            control.before_live_identity_check(expected, live);
        require_live_completion(expected, live);
        require_deadline();
    };
    try {
        require_deadline();
        if (remote)
            co_await async_connect(socket, *remote,
                                   impl_->stamp(session, AsyncOperationKind::Connect),
                                   impl_->completions, verify);

        SessionHello hello;
        hello.c_store_guid = impl_->c_guid;
        hello.supported_profiles = profile_bit(impl_->caps.profile);
        hello.limits = impl_->caps.wire;
        if (control.before_first_remote_write)
            control.before_first_remote_write();
        // From this point a completed, partial, or locally interrupted write
        // may have exposed CacheWire authority to the peer.  Cancellation is
        // consequently reconciliation work even when the write completion
        // itself reports zero progress.
        co_await async_write_message(socket, hello, impl_->caps.wire.max_frame_payload,
                                     impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                     impl_->completions, control, verify);

        Frame state_frame = co_await async_read_frame(
            socket, impl_->caps.wire.max_frame_payload,
            impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions, verify);
        SessionState peer = client_checked(impl_->caps.wire.max_frame_payload, [&] {
            if (state_frame.type == MessageType::ERROR)
                throw ClientTerminalResult(decode_as<ErrorMessage>(state_frame),
                                           impl_->caps.wire.max_frame_payload);
            SessionState received = decode_as<SessionState>(state_frame);
            validate_session_state(hello, received);
            if ((received.negotiated_profiles & profile_bit(impl_->caps.profile)) == 0)
                throw std::invalid_argument("peer did not negotiate the configured profile");
            return received;
        });
        terminal_cap = peer.limits.max_frame_payload;

        const bool knew_f = impl_->f_guid.has_value();
        const bool same_f = knew_f && *impl_->f_guid == peer.f_store_guid;
        const bool established_relationship = knew_f && impl_->route_known;
        session.f_guid = peer.f_store_guid;
        if (same_f && established_relationship && !peer.namespace_present) {
            const ErrorMessage terminal = bounded_error(
                2, "established F_STORE_GUID reported a missing C namespace",
                peer.limits.max_frame_payload);
            co_await async_report_client_terminal(
                socket, terminal, peer.limits.max_frame_payload,
                impl_->stamp(session, AsyncOperationKind::WriteFragment), impl_->completions,
                control, verify);
        }
        const bool exact = same_f && peer.namespace_present && peer.route_present &&
                           impl_->route_known && peer.history_nonce == impl_->history_nonce &&
                           peer.next_rel_seq == impl_->next_rel &&
                           peer.state_digest == impl_->state;

        // An adopted descriptor is already an exact FSession claim.  A local
        // peer observation that is not the exact retained route cannot grant
        // a reset, abort, fallback, or second BODY.  Freeze all retained
        // claim/route/queued authority and return the observation to the
        // owning FSession operation for authenticated settlement.
        const bool exact_claim_adopted =
            !remote && impl_->run_identity_seed &&
            impl_->run_identity_seed->valid();
        if (exact_claim_adopted && !exact) {
            result.observation = same_f ? ClientRunObservation::ReconcileRequired
                                        : ClientRunObservation::WrongAdoptedPeer;
            result.reconnect = same_f ? EndpointReconnectOutcome::RouteHistoryReset
                                      : EndpointReconnectOutcome::WrongAdoptedPeer;
            close_now(socket);
            impl_->active_session = 0;
            co_return result;
        }
        if (same_f && impl_->active && peer.last_commit && peer.namespace_present &&
            peer.route_present && same_commit(*peer.last_commit, impl_->active->begin) &&
            peer.history_nonce == impl_->history_nonce &&
            peer.next_rel_seq.value == impl_->active->begin.rel_seq.value + 1 &&
            peer.state_digest == peer.last_commit->post_state_digest) {
            require_deadline();
            impl_->accept(*peer.last_commit, ActionType::LOST_COMMIT_ACCEPTED, serial);
            io->committed = true;
            result.committed_commit = *peer.last_commit;
            result.committed_input = InputRecordKey{impl_->c_guid, peer.last_commit->tu_seq};
            result.observation = ClientRunObservation::ExactCommitObserved;
            result.status = ClientRunStatus::Committed;
            result.reconnect = EndpointReconnectOutcome::LostFinalAcknowledgement;
            close_now(socket);
            boost::system::error_code timer_error;
            io->timer.cancel(timer_error);
            impl_->active_session = 0;
            co_return result;
        }
        if (same_f && impl_->active && !exact) {
            result.reconnect = EndpointReconnectOutcome::RouteHistoryReset;
            const ErrorMessage terminal = bounded_error(
                2, "established F route differs while C retains an active transaction",
                peer.limits.max_frame_payload);
            co_await async_report_client_terminal(
                socket, terminal, peer.limits.max_frame_payload,
                impl_->stamp(session, AsyncOperationKind::WriteFragment), impl_->completions,
                control, verify);
        }

        if (exact) {
            result.reconnect = EndpointReconnectOutcome::ExactMatch;
        } else {
            result.reconnect = same_f ? EndpointReconnectOutcome::RouteHistoryReset
                                      : EndpointReconnectOutcome::ColdFStore;
            // A different route is an observation only; an endpoint-local
            // result cannot authorize a replacement attempt.
            result.observation = ClientRunObservation::ReconcileRequired;
            PreparedInputPtr retry = impl_->active ? impl_->active->prepared : impl_->queued;
            impl_->queued_handle = impl_->active ? impl_->active->handle : impl_->queued_handle;
            if (same_f && peer.route_present)
                impl_->advance_nonce_past(peer.history_nonce);
            const HistoryNonce replacement_nonce = impl_->allocate_nonce();
            const RelSeq replacement_rel{0};
            const Digest128 replacement_state =
                initial_route_digest(impl_->c_guid, replacement_nonce);
            session.provisional_nonce = replacement_nonce;
            session.provisional_rel = replacement_rel;
            HistoryReset reset{replacement_nonce, replacement_state};
            co_await async_write_message(socket, reset, peer.limits.max_frame_payload,
                                         impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                         impl_->completions, control, verify);
            Frame reset_ack_frame = co_await async_read_frame(
                socket, peer.limits.max_frame_payload,
                impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions, verify);
            const SessionState reset_ack = client_checked(peer.limits.max_frame_payload, [&] {
                if (reset_ack_frame.type == MessageType::ERROR)
                    throw ClientTerminalResult(decode_as<ErrorMessage>(reset_ack_frame),
                                               peer.limits.max_frame_payload);
                SessionState received = decode_as<SessionState>(reset_ack_frame);
                validate_session_state(hello, received);
                if (received.selected_protocol != peer.selected_protocol ||
                    received.negotiated_profiles != peer.negotiated_profiles ||
                    received.limits != peer.limits || !received.namespace_present ||
                    !received.route_present || received.f_store_guid != peer.f_store_guid ||
                    received.history_nonce != replacement_nonce ||
                    received.next_rel_seq != replacement_rel ||
                    received.state_digest != replacement_state || received.last_commit)
                    throw std::invalid_argument(
                        "HISTORY_RESET acknowledgement differs from the new route");
                return received;
            });
            (void)reset_ack;
            if (impl_->active) {
                if (same_f) {
                    impl_->record(ActionType::TX_ABORTED, impl_->active->begin, serial,
                                  impl_->active->begin.pre_state_digest);
                } else if (established_relationship) {
                    impl_->record_incarnation_replaced(*impl_->f_guid, peer.f_store_guid,
                                                       impl_->active->begin, serial);
                }
            }
            impl_->active.reset();
            impl_->queued = retry;
            impl_->f_guid = peer.f_store_guid;
            impl_->history_nonce = replacement_nonce;
            impl_->next_rel = replacement_rel;
            impl_->state = replacement_state;
            impl_->route_known = true;
            session.provisional_nonce.reset();
        }

        if (!impl_->active) {
            if (!impl_->queued)
                throw std::logic_error("route reconciliation lost queued work");
            const PreparedInputPtr queued = impl_->queued;
            const PreparedTuHandle queued_handle = impl_->queued_handle;
            impl_->start_active(queued, queued_handle, serial);
            impl_->queued.reset();
            impl_->queued_handle = {};
        }
        TxBegin begin = impl_->active->begin;
        if (control.outbound_begin_transform)
            begin = control.outbound_begin_transform(begin);
        require_outbound_profile_negotiated(peer.negotiated_profiles, begin);
        const uint32_t frame_cap = peer.limits.max_frame_payload;
        co_await async_write_message(socket, begin, frame_cap,
                                     impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                     impl_->completions, control, verify);

        if (begin.profile == ProfileId::P29) {
            co_await async_write_component<DictMessage>(
                socket, impl_->active->prepared->dict, frame_cap,
                impl_->stamp(session, AsyncOperationKind::WriteFragment), impl_->completions,
                control, verify);
            NeedStreamDecoder need_decoder;
            for (;;) {
                Frame need_frame = co_await async_read_frame(
                    socket, frame_cap,
                    impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions,
                    verify);
                if (need_frame.type == MessageType::ERROR)
                    throw ClientTerminalResult(decode_as<ErrorMessage>(need_frame), frame_cap);
                if (need_frame.type != MessageType::NEED)
                    throw std::invalid_argument("P29 expected NEED before FILL");
                need_decoder.push(decode_as<NeedMessage>(need_frame));
                if (need_decoder.complete())
                    break;
            }
            std::map<Key64, const FillRecord*> available;
            for (const FillRecord& record : impl_->active->prepared->p29_fill_records)
                available.emplace(record.key, &record);
            std::vector<FillRecord> selected;
            selected.reserve(need_decoder.keys().size());
            for (Key64 key : need_decoder.keys()) {
                const auto position = available.find(key);
                if (position == available.end())
                    throw std::logic_error("P29 Need requested an unprepared object");
                selected.push_back(*position->second);
            }
            const std::vector<FillMessage> fills = encode_fill_messages(selected, frame_cap);
            for (const FillMessage& fill : fills)
                co_await async_write_message(
                    socket, fill, frame_cap,
                    impl_->stamp(session, AsyncOperationKind::WriteFragment),
                    impl_->completions, control, verify);
            co_await async_write_component<BodyMessage>(
                socket, impl_->active->prepared->body, frame_cap,
                impl_->stamp(session, AsyncOperationKind::WriteFragment), impl_->completions,
                control, verify);
        } else {
            co_await async_write_component<BodyMessage>(
                socket, impl_->active->prepared->body, frame_cap,
                impl_->stamp(session, AsyncOperationKind::WriteFragment), impl_->completions,
                control, verify);
        }
        Frame commit_frame = co_await async_read_frame(
            socket, frame_cap, impl_->stamp(session, AsyncOperationKind::ReadHeader),
            impl_->completions, verify);
        client_checked(frame_cap, [&] {
            if (commit_frame.type == MessageType::ERROR)
                throw ClientTerminalResult(decode_as<ErrorMessage>(commit_frame), frame_cap);
            const TxCommit commit = decode_as<TxCommit>(commit_frame);
            require_deadline();
            impl_->accept(commit, ActionType::COMMIT_ACCEPTED, serial);
            io->committed = true;
            result.committed_commit = commit;
            result.committed_input = InputRecordKey{impl_->c_guid, commit.tu_seq};
            result.observation = ClientRunObservation::ExactCommitObserved;
        });
        result.status = ClientRunStatus::Committed;
        close_now(socket);
    } catch (const ClientTerminalResult& terminal) {
        close_now(socket);
        set_client_terminal_result(result, terminal.error());
    } catch (const StaleCompletion&) {
        close_now(socket);
        result.status = ClientRunStatus::Disconnected;
        result.observation = ClientRunObservation::Disconnected;
    } catch (const boost::system::system_error&) {
        close_now(socket);
        result.status = io->expired || deadline_crossed()
                            ? ClientRunStatus::DeadlineExceeded
                            : ClientRunStatus::Disconnected;
        result.observation = io->expired || deadline_crossed()
                                 ? ClientRunObservation::DeadlineExpired
                                 : ClientRunObservation::Disconnected;
    } catch (const std::bad_alloc&) {
        close_now(socket);
        if (impl_->active_session == serial)
            impl_->active_session = 0;
        boost::system::error_code timer_error;
        io->timer.cancel(timer_error);
        throw;
    } catch (const std::exception& error) {
        close_now(socket);
        set_client_terminal_result(result, bounded_error(3, error.what(), terminal_cap));
    } catch (...) {
        close_now(socket);
        if (impl_->active_session == serial)
            impl_->active_session = 0;
        boost::system::error_code timer_error;
        io->timer.cancel(timer_error);
        throw;
    }
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    if (impl_->active_cancel_requested) {
        // An adopted endpoint cannot prove pre-durable abort locally. The
        // owning FSession operation must settle this exact observation.
        result.cancellation = ClientCancellationDisposition::ReconcileRequired;
        result.observation = ClientRunObservation::Cancelled;
    }
#endif
    if (!io->committed && io->expired
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
        && !impl_->active_cancel_requested
#endif
    ) {
        // A timeout is fail-closed: the active transaction/queued work remains
        // exactly as reconciliation state for the next run.
        result = ClientRunResult{};
        result.status = ClientRunStatus::DeadlineExceeded;
        result.observation = ClientRunObservation::DeadlineExpired;
    }
    boost::system::error_code timer_error;
    io->timer.cancel(timer_error);
    if (impl_->active_session == serial)
        impl_->active_session = 0;
    co_return result;
}

std::optional<tcp::socket> P50ClientEndpoint::adopt_connected_fd(
    asio::any_io_executor executor, int fd, boost::system::error_code& error) {
    // Keep one ownership/validation law for both endpoint directions.  The
    // server helper consumes and closes fd on every failure path, and proves
    // CLOEXEC plus connected IPv4/IPv6 TCP before returning the socket.
    return P50ServerEndpoint::adopt_connected_fd(executor, fd, error);
}

EndpointCancelResult P50ClientEndpoint::request_cancel(
    const EndpointCancelPermit& permit) noexcept {
    return impl_->endpoint_runs.request_cancel(permit);
}

size_t P50ClientEndpoint::cancel_all_for_incarnation(
    const SidecarLaunchIdentity& incarnation) noexcept {
    return impl_->endpoint_runs.cancel_all_for_incarnation(incarnation);
}

#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
void P50ClientEndpoint::request_cancel_for_test() noexcept {
    if (impl_->socket_for_test_cancel == nullptr)
        return;
    impl_->active_cancel_requested = true;
    impl_->active_cancellation = ClientCancellationDisposition::ReconcileRequired;
    close_now(*impl_->socket_for_test_cancel);
}
#endif

CStoreGuid P50ClientEndpoint::c_store_guid() const {
    impl_->owner.check();
    return impl_->c_guid;
}

std::optional<FStoreGuid> P50ClientEndpoint::f_store_guid() const {
    impl_->owner.check();
    return impl_->f_guid;
}

bool P50ClientEndpoint::has_active_transaction() const {
    impl_->owner.check();
    return impl_->active.has_value();
}

bool P50ClientEndpoint::has_reconciliation_work() const {
    impl_->owner.check();
    return impl_->active.has_value() || static_cast<bool>(impl_->queued);
}

RelSeq P50ClientEndpoint::next_rel_seq() const {
    impl_->owner.check();
    return impl_->next_rel;
}

Digest128 P50ClientEndpoint::state_digest() const {
    impl_->owner.check();
    return impl_->state;
}

P50ServerEndpoint::P50ServerEndpoint(FStoreGuid f_store_guid, EndpointCaps caps,
                                     CompletionLog* completions, ActionTrace* actions,
                                     P50ServerEndpointConfig config)
    : impl_(std::make_unique<Impl>(f_store_guid, caps, completions, actions,
                                   std::move(config))) {}

P50ServerEndpoint::~P50ServerEndpoint() = default;

std::optional<tcp::socket> P50ServerEndpoint::adopt_connected_fd(
    asio::any_io_executor executor, int fd, boost::system::error_code& error) {
    error.clear();
    int family = AF_UNSPEC;
    if (!inspect_native_connected_tcp(fd, family, error)) {
        close_native_fd(fd);
        return std::nullopt;
    }
    try {
        tcp::socket socket(executor);
        socket.assign(family == AF_INET ? tcp::v4() : tcp::v6(), fd, error);
        if (error) {
            close_native_fd(fd);
            return std::nullopt;
        }
        fd = -1;
        if (!validate_adopted_socket(socket)) {
            close_now(socket);
            error = asio::error::operation_not_supported;
            return std::nullopt;
        }
        return socket;
    } catch (...) {
        close_native_fd(fd);
        throw;
    }
}

boost::asio::awaitable<ServerRunResult> P50ServerEndpoint::run_adopted(
    tcp::socket socket, EndpointIoControl control) {
    impl_->owner.require();
    if (!validate_adopted_socket(socket)) {
        close_now(socket);
        co_return ServerRunResult{};
    }
    const Impl::Session session = impl_->allocate_session();
    SessionRegistration registration(*impl_, session);
    co_return co_await run_connected(std::move(socket), std::move(registration),
                                     std::move(control), nullptr, std::nullopt,
                                     std::nullopt);
}

boost::asio::awaitable<ServerRunResult> P50ServerEndpoint::run_adopted(
    sidecar::P5coEndpointHandoff handoff, EndpointIoControl control) {
    impl_->owner.require();
    ServerRunResult invalid;
    if (!handoff.valid() ||
        handoff.outcome().kind !=
            daemon::P50CacheSessionOutcomeKind::Adopted ||
        handoff.outcome().f_store_guid != impl_->f_guid.bytes) {
        co_return invalid;
    }

    const daemon::P50CacheSessionOutcome exact_outcome = handoff.outcome();
    const std::optional<daemon::P50CacheSessionWireClaim> exact_claim =
        daemon::decode_cache_session_wire_claim(
            exact_outcome.canonical_claim);
    if (!exact_claim.has_value())
        co_return invalid;
    const CStoreGuid expected_c_store_guid{
        exact_claim->binding.arm.c_store_guid};
    const sidecar::AbsoluteMonotonicDeadline exact_deadline =
        handoff.deadline();
    sidecar::SystemMonotonicObservationSource observations;
    const std::optional<sidecar::MonotonicObservation> observed =
        observations.observe();
    if (!observed.has_value() || !observed->valid() ||
        !exact_deadline.matches_clock(observed->clock) ||
        observed->now_ns >= exact_deadline.expires_at_ns) {
        invalid.status = ServerRunStatus::DeadlineExceeded;
        co_return invalid;
    }

    std::unique_ptr<sidecar::P5coAdoptedSocketLease> lease =
        handoff.take_lease_for_endpoint();
    if (!lease)
        co_return invalid;
    struct LeaseFailureFence {
        sidecar::P5coAdoptedSocketLease* lease = nullptr;
        ~LeaseFailureFence() {
            if (lease != nullptr)
                lease->fence();
        }
        void dismiss() noexcept { lease = nullptr; }
    } lease_failure_fence{lease.get()};
    if (!lease->revalidate(exact_outcome, exact_deadline)) {
        co_return invalid;
    }
    const int adopted_fd =
        lease->release_native_fd_for_endpoint(exact_outcome, exact_deadline);
    if (adopted_fd < 0) {
        co_return invalid;
    }

    const auto executor = co_await asio::this_coro::executor;
    boost::system::error_code adoption_error;
    std::optional<tcp::socket> socket =
        adopt_connected_fd(executor, adopted_fd, adoption_error);
    if (!socket) {
        // The endpoint adoption path consumed/closed adopted_fd.  Fence the
        // exact detached operation as well; descriptor cleanup alone is not an
        // operation-level terminal witness.
        co_return invalid;
    }

    const Impl::Session session =
        impl_->allocate_session(exact_outcome.operation, exact_deadline);
    SessionRegistration registration(*impl_, session);
    // From here the exact typed session registration and connected reducer own
    // terminal settlement.  Before this point every exception or early return
    // fences the consumed post-P5CO authority exactly once.
    lease_failure_fence.dismiss();
    co_return co_await run_connected(
        std::move(*socket), std::move(registration), std::move(control),
        nullptr, exact_deadline,
        expected_c_store_guid);
}

boost::asio::awaitable<ServerRunResult> P50ServerEndpoint::accept_one(tcp::acceptor& acceptor,
                                                                      EndpointIoControl control) {
    impl_->owner.require();
    const Impl::Session session = impl_->allocate_session();
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    SessionRegistration registration(*impl_, session);
    co_return co_await run_connected(std::move(socket), std::move(registration),
                                     std::move(control), &acceptor,
                                     std::nullopt, std::nullopt);
}

boost::asio::awaitable<ServerRunResult> P50ServerEndpoint::run_connected(
    tcp::socket socket_value, SessionRegistration registration,
    EndpointIoControl control,
    tcp::acceptor* acceptor,
    std::optional<sidecar::AbsoluteMonotonicDeadline> deadline,
    std::optional<CStoreGuid> expected_c_store_guid) {
    impl_->owner.require();
    Impl::Session session = registration.session();
    ServerRunResult result;
    result.session_serial = session.serial;
    auto io = std::make_shared<ServerIoState>(
        std::move(socket_value), session.operation, session.deadline);
    tcp::socket& socket_for_run = io->socket;
    struct TimerCancelGuard {
        std::shared_ptr<ServerIoState> io;
        ~TimerCancelGuard() {
            boost::system::error_code ignored;
            io->deadline_timer.cancel(ignored);
            io->deadline_timer.close(ignored);
        }
    } timer_cancel_guard{io};
    sidecar::SystemMonotonicObservationSource deadline_observations;
    const auto deadline_crossed = [&]() {
        if (!deadline)
            return false;
        const std::optional<sidecar::MonotonicObservation> observed =
            deadline_observations.observe();
        return !observed.has_value() || !observed->valid() ||
               !deadline->matches_clock(observed->clock) ||
               observed->now_ns >= deadline->expires_at_ns;
    };
    if (deadline_crossed()) {
        close_now(socket_for_run);
        result.status = ServerRunStatus::DeadlineExceeded;
        co_return result;
    }
    if (deadline) {
        boost::system::error_code timer_error;
        if (!arm_absolute_deadline_timer(*io, *deadline, timer_error)) {
            close_now(socket_for_run);
            result.status = ServerRunStatus::TerminalError;
            co_return result;
        }
        io->deadline_timer.async_wait(
            asio::posix::stream_descriptor::wait_read,
            [io](const boost::system::error_code& error) {
                if (error)
                    return;
                uint64_t expirations = 0;
                const ssize_t drained =
                    ::read(io->deadline_timer.native_handle(), &expirations,
                           sizeof(expirations));
                if (drained < 0 && errno != EAGAIN &&
                    errno != EWOULDBLOCK)
                    io->cancelled = true;
                io->expired = true;
                if (auto materialization = io->materialization.lock()) {
                    cancel_materialization_notification(materialization);
                }
                close_now(io->socket);
            });
    }
    const auto require_deadline = [&]() {
        if (deadline_crossed())
            throw boost::system::system_error(asio::error::timed_out);
    };
    const auto require_operation = [&]() {
        if (io->operation != session.operation ||
            io->deadline != session.deadline ||
            io->deadline != deadline)
            throw StaleCompletion();
        if (io->cancelled)
            throw boost::system::system_error(asio::error::operation_aborted);
        require_deadline();
    };
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    struct ActiveIoGuard {
        Impl& owner;
        ServerIoState* io;
        ~ActiveIoGuard() {
            if (owner.active_io == io)
                owner.active_io = nullptr;
        }
    } active_io_guard{*impl_, io.get()};
    impl_->active_io = io.get();
#endif
    tcp::socket& socket = socket_for_run;
    uint32_t reply_cap = impl_->caps.wire.max_frame_payload;
    struct EndpointRunLeaseGuard {
        EndpointRunRegistry* registry = nullptr;
        std::optional<EndpointRunIdentity> identity;
        std::optional<EndpointCancelPermit> permit;
        std::function<void(EndpointCancelPermit, EndpointTerminalResult)> terminal;
        ~EndpointRunLeaseGuard() {
            if (!registry || !identity || !permit)
                return;
            const EndpointTerminalResult result{EndpointTerminalResultState::Failed, 0};
            if (registry->mark_terminal(*identity, result)) {
                if (terminal) {
                    try {
                        terminal(*permit, result);
                    } catch (...) {
                        // Owner notification cannot make destructor cleanup
                        // terminate the endpoint executor.
                    }
                }
                (void)registry->consume_terminal(*identity);
            }
        }
    } run_lease{&impl_->endpoint_runs, std::nullopt, std::nullopt,
                impl_->config.on_run_terminal};
    if (impl_->config.sidecar_launch && session.operation &&
        expected_c_store_guid &&
        impl_->config.endpoint_generation != 0 &&
        impl_->next_run_sequence != 0 &&
        impl_->next_socket_ownership_generation != 0) {
        EndpointRunIdentity identity;
        identity.sidecar_launch = *impl_->config.sidecar_launch;
        identity.c_store_guid = *expected_c_store_guid;
        identity.f_store_guid = impl_->f_guid;
        identity.f_session_operation = *session.operation;
        identity.endpoint_generation = impl_->config.endpoint_generation;
        identity.endpoint_session_serial = session.serial;
        identity.run_sequence = impl_->next_run_sequence++;
        identity.socket_ownership_generation =
            impl_->next_socket_ownership_generation++;
        auto handle = impl_->endpoint_runs.admit(
            identity, deadline.value_or(sidecar::AbsoluteMonotonicDeadline{}),
            std::make_shared<ServerRunSocketTarget>(io));
        if (!handle)
            throw std::length_error("endpoint run identity admission failed");
        run_lease.identity = identity;
        run_lease.permit = handle->permit(EndpointCancelReason::CallerRequested);
        (void)impl_->endpoint_runs.set_phase(identity, EndpointRunPhase::CacheWire);
        if (impl_->config.on_run_admitted)
            impl_->config.on_run_admitted(*run_lease.permit);
    }
    const auto verify = [&](const CompletionStamp& expected) {
        impl_->owner.require();
        require_operation();
        CompletionStamp observed = expected;
        if (control.before_completion_check)
            control.before_completion_check(observed);
        observed = completion_for_test(observed, control);
        require_observed_completion(expected, observed);
        CompletionLiveIdentity live = impl_->live_identity(session, expected);
        if (control.before_live_identity_check)
            control.before_live_identity_check(expected, live);
        require_live_completion(expected, live);
        // Both completion hooks are arbitrary owner-affine product callbacks.
        // They may cross the absolute deadline or synchronously request
        // cancellation, so their identity observation is not the final
        // operation/deadline observation for this completion.
        require_operation();
    };
    std::optional<ErrorMessage> terminal_error;
    try {
        require_operation();
        if (acceptor != nullptr) {
            co_await async_accept(*acceptor, socket,
                                  impl_->stamp(session, AsyncOperationKind::Accept),
                                  impl_->completions, verify);
        }
        Frame hello_frame = co_await async_read_frame(
            socket, impl_->caps.wire.max_frame_payload,
            impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions, verify);
        SessionHello hello = decode_as<SessionHello>(hello_frame);
        if (expected_c_store_guid.has_value() &&
            hello.c_store_guid != *expected_c_store_guid)
            throw std::invalid_argument(
                "SESSION_HELLO differs from the adopted P5CO claim");
        result.c_store_guid = hello.c_store_guid;
        reply_cap = std::min(reply_cap, hello.limits.max_frame_payload);
        const SessionSelection selection =
            negotiate_session(hello, kProtocolVersion, kProtocolVersion,
                              impl_->caps.supported_profiles, impl_->caps.wire);
        SessionState state = impl_->stage(session, hello.c_store_guid, selection);
        co_await async_write_message(socket, state, selection.limits.max_frame_payload,
                                     impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                     impl_->completions, control, verify);

        Frame next = co_await async_read_frame(
            socket, selection.limits.max_frame_payload,
            impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions, verify);
        if (next.type == MessageType::HISTORY_RESET) {
            const HistoryReset reset = decode_as<HistoryReset>(next);
            impl_->validate_history_reset_candidate(session, reset);
            impl_->activate(session);
            impl_->reset_history(session, reset);
            SessionState reset_ack = impl_->session_state(session, selection);
            co_await async_write_message(socket, reset_ack, selection.limits.max_frame_payload,
                                         impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                         impl_->completions, control, verify);
            next = co_await async_read_frame(socket, selection.limits.max_frame_payload,
                                             impl_->stamp(session, AsyncOperationKind::ReadHeader),
                                             impl_->completions, verify);
            const TxBegin begin = decode_as<TxBegin>(next);
            impl_->begin(session, begin, selection.negotiated_profiles);
        } else {
            const TxBegin begin = decode_as<TxBegin>(next);
            Impl::PreparedBegin prepared = impl_->prepare_begin_candidate(
                session, begin, selection.negotiated_profiles);
            impl_->activate(session);
            impl_->install_begin(session, std::move(prepared));
        }

        do {
            Frame component = co_await async_read_frame(
                socket, selection.limits.max_frame_payload,
                impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions, verify);
            // The reducer owns message ordering, while the selected profile
            // owns DICT/BODY/NEED/FILL semantics. ZSTD_TU deliberately rejects
            // the interactive messages through its adapter; future profiles
            // can implement the same bidirectional dialogue without a new
            // concrete-profile branch here.
            switch (component.type) {
            case MessageType::DICT:
                impl_->append_dict(session, decode_as<DictMessage>(component));
                if (impl_->namespaces.at(*session.c_guid).route->pending->begin.profile ==
                    ProfileId::P29) {
                    const std::vector<NeedMessage> needs =
                        impl_->namespaces.at(*session.c_guid).route->pending->dialogue->
                            need_messages(selection.limits.max_frame_payload);
                    for (const NeedMessage& need : needs)
                        co_await async_write_message(
                            socket, need, selection.limits.max_frame_payload,
                            impl_->stamp(session, AsyncOperationKind::WriteFragment),
                            impl_->completions, control, verify);
                }
                break;
            case MessageType::BODY:
                impl_->append_body(session, decode_as<BodyMessage>(component));
                break;
            case MessageType::NEED:
                impl_->receive_need(session, decode_as<NeedMessage>(component));
                break;
            case MessageType::FILL:
                impl_->receive_fill(session, decode_as<FillMessage>(component));
                break;
            default:
                throw std::invalid_argument("unexpected profile component message");
            }
        } while (!impl_->body_complete(session));

        ServerMaterializationJob materialization =
            impl_->begin_materialization(
                session, std::move(control.before_materialize_on_worker));
        ServerMaterializationCompletion materialization_completion =
            co_await async_materialize(std::move(materialization), io);
        // The codec worker owns no publication authority.  Revalidate the
        // exact owner operation after its completion and before moving the
        // dialogue or exact bytes back into owner-visible state.
        require_operation();
        Impl::MaterializedInput materialized =
            impl_->finish_materialization(
                session, std::move(materialization_completion));
        const InputJobState job_state =
            impl_->select_materialized_job_state(session, materialized);
        // The selector is a product callback and may run for arbitrarily long.
        // Sample the original absolute deadline again after it returns and
        // immediately before the allocation-free owner publication seam.
        require_operation();
        const TxBegin committed_begin = materialized.begin;
        const TxCommit commit = impl_->commit_materialized(
            session, std::move(materialized), job_state,
            result.candidate_input,
            result.completed_input, result.committed_input);
        co_await async_write_message(
            socket, commit, selection.limits.max_frame_payload,
            impl_->stamp(session, AsyncOperationKind::WriteFragment, &committed_begin),
            impl_->completions, control, verify);
        co_await async_wait_peer_close(
            socket, impl_->stamp(session, AsyncOperationKind::WaitPeerClose, &committed_begin),
            impl_->completions, verify);
        impl_->disconnect(session, false);
        result.status = ServerRunStatus::Completed;
        close_now(socket);
        co_return result;
    } catch (const StaleCompletion&) {
        close_now(socket);
        impl_->disconnect(session, true);
        result.status = ServerRunStatus::Disconnected;
        co_return result;
    } catch (const boost::system::system_error&) {
        close_now(socket);
        impl_->disconnect(session, true);
        result.status = io->expired || deadline_crossed()
                            ? ServerRunStatus::DeadlineExceeded
                            : ServerRunStatus::Disconnected;
        co_return result;
    } catch (const std::exception& error) {
        terminal_error = bounded_error(impl_->config.protocol_error_code,
                                       error.what(), reply_cap);
    }

    // C++ forbids a coroutine suspension directly inside an exception handler.
    // Preserve the bounded reply there and send it on the ordinary coroutine path.
    if (io->expired || deadline_crossed()) {
        close_now(socket);
        impl_->disconnect(session, true);
        result.status = ServerRunStatus::DeadlineExceeded;
        co_return result;
    }
    try {
        co_await async_write_message(socket, *terminal_error, reply_cap,
                                     impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                     impl_->completions, control, verify);
    } catch (...) {
    }
    close_now(socket);
    // A terminal reply ends this dialogue and discards its component overlay,
    // but an installed TX_BEGIN remains the exact retry/reconciliation identity.
    impl_->disconnect(session, true);
    result.status = ServerRunStatus::TerminalError;
    result.terminal_error = std::move(*terminal_error);
    co_return result;
}

EndpointCancelResult P50ServerEndpoint::request_cancel(
    const EndpointCancelPermit& permit) noexcept {
    return impl_->endpoint_runs.request_cancel(permit);
}

size_t P50ServerEndpoint::cancel_all_for_incarnation(
    const SidecarLaunchIdentity& incarnation) noexcept {
    return impl_->endpoint_runs.cancel_all_for_incarnation(incarnation);
}

#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
void P50ServerEndpoint::request_cancel_for_test() noexcept {
    if (impl_->active_io != nullptr) {
        impl_->active_io->cancelled = true;
        if (auto materialization =
                impl_->active_io->materialization.lock()) {
            cancel_materialization_notification(materialization);
        }
        close_now(impl_->active_io->socket);
    }
}
#endif

void P50ServerEndpoint::reset_store(FStoreGuid new_guid) {
    impl_->owner.require();
    if (new_guid == FStoreGuid{})
        throw std::invalid_argument("F store reset GUID zero is reserved");
    if (new_guid == impl_->f_guid)
        throw std::invalid_argument("F store reset requires a fresh GUID");
    for (auto& [guid, space] : impl_->namespaces) {
        if (space.route && space.route->pending) {
            if (space.route->pending->dialogue)
                space.route->pending->dialogue->discard_tentative();
            impl_->release_pending(*space.route->pending);
        }
        if (space.active_session != 0) {
            Impl::Session invalidated{.serial = space.active_session,
                                      .f_guid = impl_->f_guid,
                                      .operation = std::nullopt,
                                      .deadline = std::nullopt,
                                      .c_guid = guid,
                                      .candidate_revision = 0,
                                      .candidate_state = std::nullopt,
                                      .activated = true};
            impl_->record(ActionType::SESSION_DISCONNECTED, invalidated);
        }
    }
    while (impl_->global_resources->live_namespace_count() != 0)
        impl_->global_resources->evict_oldest();
    impl_->live_sessions.clear();
    impl_->namespaces.clear();
    impl_->revisions.clear();
    impl_->input_records.clear();
    impl_->next_namespace_touch = 1;
    impl_->namespace_touch_exhausted = false;
    impl_->f_guid = new_guid;
}

FStoreGuid P50ServerEndpoint::f_store_guid() const {
    impl_->owner.check();
    return impl_->f_guid;
}

size_t P50ServerEndpoint::namespace_count() const {
    impl_->owner.check();
    return impl_->namespaces.size();
}

size_t P50ServerEndpoint::revision_count() const {
    impl_->owner.check();
    return impl_->revisions.size();
}

size_t P50ServerEndpoint::live_session_count() const {
    impl_->owner.check();
    return impl_->live_sessions.size();
}

InputCursor P50ServerEndpoint::attach_input(InputRecordKey key) const {
    impl_->owner.require();
    return impl_->input_records.attach(key);
}

void P50ServerEndpoint::close_input_job(InputRecordKey key) {
    impl_->owner.require();
    impl_->input_records.close_job(key);
}

void P50ServerEndpoint::collect_input_garbage() {
    impl_->owner.require();
    std::map<CStoreGuid, std::vector<InputRecordKey>> before;
    for (const auto& [c_guid, space] : impl_->namespaces) {
        (void)space;
        before.emplace(c_guid, impl_->input_records.namespace_keys(c_guid));
    }
    impl_->input_records.collect_garbage();
    impl_->release_collected_global_inputs(before);
}

P50ServerOwnerUsage P50ServerEndpoint::owner_usage() const {
    impl_->owner.check();
    return {.live_sessions = impl_->live_sessions.size(),
            .namespaces = impl_->namespaces.size(),
            .revisions = impl_->revisions.size(),
            .pending_encoded_bytes = impl_->pending_encoded_bytes,
            .pending_raw_bytes = impl_->pending_raw_bytes,
            .decoder_window_bytes = impl_->decoder_window_bytes,
            .retained_input_records = impl_->input_records.record_count(),
            .retained_input_bytes = impl_->input_records.retained_bytes()};
}

std::optional<InputRecordKey>
P50ServerEndpoint::last_committed_input(CStoreGuid c_store_guid) const {
    impl_->owner.check();
    const auto position = impl_->namespaces.find(c_store_guid);
    return position == impl_->namespaces.end() ? std::nullopt
                                               : position->second.last_input;
}

} // namespace icecc::p50
