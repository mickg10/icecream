#include "p50_endpoint.h"
#include "../services/digest128.h"
#include "p50_slice0.h"

#include "p50_adopted_outcome_writer.h"
#include "codec/p29_wire.h"

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
#include <chrono>
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

uint64_t elapsed_nanoseconds(
    std::chrono::steady_clock::time_point started) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    return elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
}

void add_saturating(uint64_t& target, uint64_t value) noexcept {
    target = value > std::numeric_limits<uint64_t>::max() - target
                 ? std::numeric_limits<uint64_t>::max()
                 : target + value;
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
    if (caps.profile != ProfileId::P29V1 &&
        caps.profile != ProfileId::ZSTD_TU &&
        caps.profile != ProfileId::ZSTD_ROUTE)
        throw std::invalid_argument("endpoint profile is unsupported");
    if (caps.supported_profiles == 0 ||
        (caps.supported_profiles & ~kOperationalProfileMask) != 0)
        throw std::invalid_argument("endpoint supported profile mask is unsupported");
    if ((caps.supported_profiles & profile_bit(caps.profile)) == 0)
        throw std::invalid_argument("endpoint profile is not supported");
}

ProfileId require_single_route_profile(uint32_t mask) {
    if (mask == profile_bit(ProfileId::P29V1))
        return ProfileId::P29V1;
    if (mask == profile_bit(ProfileId::ZSTD_TU))
        return ProfileId::ZSTD_TU;
    if (mask == profile_bit(ProfileId::ZSTD_ROUTE))
        return ProfileId::ZSTD_ROUTE;
    throw std::invalid_argument(
        "one CacheWire session must select exactly one route profile");
}

GlobalResourceLimits global_resource_limits(const P50ServerOwnerLimits& limits) {
    if (limits.max_retained_input_bytes >
            std::numeric_limits<uint64_t>::max() / 2 ||
        limits.max_pending_raw_bytes >
            std::numeric_limits<uint64_t>::max() / 2 ||
        limits.max_live_sessions >
            std::numeric_limits<size_t>::max() / 2)
        throw std::overflow_error(
            "P29V1 global resource limits cannot be doubled");
    // Every retained P29V1 input may own one equally bounded receiver segment,
    // and every live P29V1 TU may stage both objects before either is visible.
    const uint64_t aggregate = 2 * limits.max_retained_input_bytes;
    const uint64_t staging = 2 * limits.max_pending_raw_bytes;
    const uint64_t total = staging > std::numeric_limits<uint64_t>::max() - aggregate
                               ? std::numeric_limits<uint64_t>::max()
                               : aggregate + staging;
    return {.max_aggregate_bytes = aggregate,
            .max_namespace_bytes = aggregate,
            .max_staging_bytes = staging,
            .max_total_bytes = total,
            .max_generation = KeyLayoutV1::generation_value_mask,
            .max_staging_slots = 2 * limits.max_live_sessions};
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

class SocketDeadlineGuard {
public:
    SocketDeadlineGuard(asio::any_io_executor executor, tcp::socket& socket,
                       std::chrono::steady_clock::time_point deadline)
        : timer_(std::move(executor)), state_(std::make_shared<State>()) {
        state_->socket = &socket;
        timer_.expires_at(deadline);
        timer_.async_wait([state = state_](const boost::system::error_code& error) {
            if (!error && state->active.exchange(false, std::memory_order_acq_rel) &&
                state->socket != nullptr)
                close_now(*state->socket);
        });
    }
    SocketDeadlineGuard(const SocketDeadlineGuard&) = delete;
    SocketDeadlineGuard& operator=(const SocketDeadlineGuard&) = delete;
    ~SocketDeadlineGuard() {
        state_->active.store(false, std::memory_order_release);
        boost::system::error_code ignored;
        timer_.cancel(ignored);
    }

private:
    struct State {
        std::atomic<bool> active{true};
        tcp::socket* socket = nullptr;
    };
    asio::steady_timer timer_;
    std::shared_ptr<State> state_;
};

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
    uint64_t deadline_generation = 0;
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
    uint64_t materialize_ns = 0;
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
                       .failure = {},
                       .materialize_ns = 0};
                   try {
                       if (job.before_materialize)
                           job.before_materialize();
                       const auto materialize_started =
                           std::chrono::steady_clock::now();
                       VerifiedMaterialization exact =
                           completion.dialogue->materialize_verified();
                       completion.materialize_ns =
                           elapsed_nanoseconds(materialize_started);
                       completion.prepared_input =
                           InputRecordStore::prepare_verified_publish(
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
#if !defined(ICECC_P29V1_MUTANT_NO_NODELAY)
    // Protocol 50 is an interactive request/response dialogue with small
    // frames.  Nagle plus delayed ACK otherwise adds roughly one timer tick
    // to every transaction.
    socket.set_option(tcp::no_delay(true), error);
    if (error)
        throw boost::system::system_error(error);
#endif
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
#if !defined(ICECC_P29V1_MUTANT_NO_NODELAY)
    socket.set_option(tcp::no_delay(true), error);
    if (error)
        throw boost::system::system_error(error);
#endif
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
    if (control.outbound_message_observer)
        control.outbound_message_observer(stamp.actor, message);
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
                           ProfileId profile, uint64_t session_serial,
                           const TxBegin* begin, HistoryNonce nonce,
                           RelSeq rel, Digest128 state_digest) {
    ActionRecord record;
    record.action = action;
    record.actor = actor;
    record.profile = profile;
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
            record.stage_bytes = begin->body.encoded_bytes;
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
    struct RouteState {
        ProfileId profile = ProfileId::ZSTD_TU;
        std::vector<uint8_t> committed_route_history;
        Digest128 committed_route_history_digest = digest128(std::span<const uint8_t>{});
        std::vector<uint8_t> speculative_route_history;
        Digest128 speculative_route_history_digest = digest128(std::span<const uint8_t>{});
        uint64_t next_speculative_route_rel = 0;
        uint64_t next_confirmed_route_rel = 0;
        // Ordered per-relationship admission ledger. P29V1 may have one
        // active NEED/FILL transaction while later entries are staged; all
        // profiles confirm strictly from the front.
        std::vector<uint64_t> speculative_entries;
        std::optional<uint64_t> active_p29_entry;
        uint64_t speculative_raw_bytes = 0;
        std::unique_ptr<CRoute> p29_route;
        Digest128 p29v1_system_source_fingerprint{};
    };

    // One request/input record is C-wide.  A route view below carries only
    // relationship cursor state and its own prepared envelope.
    struct Shared {
        PrepareRequestKey request{};
        uint64_t raw_bytes = 0;
        Digest128 raw_digest{};
        std::vector<uint8_t> raw;
        TuSeq tu_seq{};
        PreparedTUPtr p29_source;
        std::map<PreparationRouteKey, uint64_t> entries;
    };

    struct Entry {
        PrepareRequestKey request{};
        PreparationRouteKey route{};
        std::shared_ptr<Shared> shared;
        PreparedInputPtr prepared;
        uint64_t references = 1;
        uint64_t retained_bytes = 0;
        bool committed = false;
        bool speculative_advanced = false;
        bool r2_rebuild_required = false;
    };

    Impl(CStoreGuid c_store_guid_value, ZstdTuLimits zstd_limits_value,
         PreparationAuthorityLimits authority_limits_value, int compression_level,
         ProfileId profile_value, TuSeq first_tu_seq,
         P29InternerFaultInjection fault_injection)
        : c_guid(c_store_guid_value), zstd_limits(zstd_limits_value),
          authority_limits(authority_limits_value), codec(compression_level),
          identity(std::make_shared<const uint8_t>(0)), route_codec(3),
          profile(profile_value) {
        if (c_guid == CStoreGuid{})
            throw std::invalid_argument("C preparation authority GUID zero is reserved");
        validate_zstd_tu_limits(zstd_limits);
        if (authority_limits.max_live_entries == 0 ||
            authority_limits.max_retained_encoded_bytes == 0 ||
            authority_limits.max_interner_reserved_bytes == 0 ||
            authority_limits.max_route_state_bytes == 0 ||
            authority_limits.max_speculative_tus == 0 ||
            authority_limits.max_speculative_tus > 30 ||
            authority_limits.max_speculative_raw_bytes == 0)
            throw std::invalid_argument("preparation-authority limits must be nonzero");
        if (profile != ProfileId::P29V1 &&
            profile != ProfileId::ZSTD_TU &&
            profile != ProfileId::ZSTD_ROUTE)
            throw std::invalid_argument("preparation authority profile is unsupported");
        p29_authority = std::make_unique<CAuthority>(
            c_guid, p29::OnlineS1::Config{}, first_tu_seq,
            fault_injection);
        if (profile == ProfileId::P29V1)
            ensure_p29v1();
    }

    void ensure_p29v1() {
        if (p29v1_enabled)
            return;
        try {
            p29_authority->enable_p29v1(
                authority_limits.max_interner_reserved_bytes,
                zstd_limits.max_raw_bytes);
        } catch (const std::length_error& error) {
            throw P29V1CapabilityUnavailable(error.what());
        }
        p29v1_enabled = true;
    }

    static PreparationRouteKey legacy_route(ProfileId profile) {
        return {FStoreGuid::from_u64(UINT64_C(0x503239434c49454e)), 1,
                profile};
    }

    RouteState& route_state(PreparationRouteKey key) {
        if (key.f_store_guid == FStoreGuid{} || key.f_store_generation == 0)
            throw std::invalid_argument("preparation route identity is zero");
        if (key.profile != ProfileId::ZSTD_TU &&
            key.profile != ProfileId::ZSTD_ROUTE &&
            key.profile != ProfileId::P29V1)
            throw std::invalid_argument("preparation route profile is unsupported");
        auto found = routes.find(key);
        if (found != routes.end()) return *found->second;
        auto state = std::make_unique<RouteState>();
        state->profile = key.profile;
        state->speculative_entries.reserve(authority_limits.max_speculative_tus);
        state->speculative_route_history = state->committed_route_history;
        state->speculative_route_history_digest =
            state->committed_route_history_digest;
        if (key.profile == ProfileId::P29V1) {
            ensure_p29v1();
            // A relationship that starts before the asynchronous daemon
            // fingerprint is ready remains reuse-off for its lifetime.  A
            // later relationship may capture the completed fingerprint;
            // neither relationship can change its reuse decision mid-route.
            state->p29v1_system_source_fingerprint =
                p29_system_source_fingerprint();
        }
        if (key.profile == ProfileId::P29V1) {
            state->p29_route = std::make_unique<CRoute>(
                *p29_authority, key.f_store_guid, HistoryNonce{1});
            state->p29_route->configure_speculative_window(
                authority_limits.max_speculative_tus,
                authority_limits.max_speculative_raw_bytes);
        }
        RouteState& result = *state;
        routes.emplace(key, std::move(state));
        return result;
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
    SingleThreadOwner owner;
    std::shared_ptr<const void> identity;
    uint64_t next_entry = 1;
    bool entry_exhausted = false;
    uint64_t retained_bytes = 0;
    ZstdRouteCodec route_codec;
    std::unique_ptr<CAuthority> p29_authority;
    bool p29v1_enabled = false;
    std::map<PreparationRouteKey, std::unique_ptr<RouteState>> routes;
    ProfileId profile = ProfileId::ZSTD_TU;
    std::map<PrepareRequestKey, std::shared_ptr<Shared>> requests;
    std::map<uint64_t, Entry> entries;
};

P50PreparationAuthority::P50PreparationAuthority(
    CStoreGuid c_store_guid, ZstdTuLimits zstd_limits,
    PreparationAuthorityLimits authority_limits, int compression_level,
    ProfileId profile, TuSeq first_tu_seq)
    : P50PreparationAuthority(
          c_store_guid, zstd_limits, authority_limits, compression_level,
          profile, first_tu_seq, P29InternerFaultInjection::Disabled) {}

P50PreparationAuthority::P50PreparationAuthority(
    CStoreGuid c_store_guid, ZstdTuLimits zstd_limits,
    PreparationAuthorityLimits authority_limits, int compression_level,
    ProfileId profile, TuSeq first_tu_seq,
    P29InternerFaultInjection fault_injection)
    : impl_(std::make_unique<Impl>(c_store_guid, zstd_limits, authority_limits,
                                   compression_level, profile, first_tu_seq,
                                   fault_injection)) {}

P50PreparationAuthority::~P50PreparationAuthority() = default;

PreparedTuHandle P50PreparationAuthority::prepare(PrepareRequestKey request,
                                                   std::span<const uint8_t> exact_input) {
    return prepare_for_route(Impl::legacy_route(impl_->profile), request, exact_input);
}

PreparedTuHandle P50PreparationAuthority::prepare_for_route(
    PreparationRouteKey route_key, PrepareRequestKey request,
    std::span<const uint8_t> exact_input) {
    impl_->owner.require();
    if (request.producer_session == 0 || request.request_token == 0)
        throw std::invalid_argument("PrepareRequestKey zero fields are reserved");
    if (exact_input.size() > impl_->zstd_limits.max_raw_bytes)
        throw std::length_error("P50 raw input exceeds the local cap");
    const Digest128 raw_digest = digest128(exact_input);
    std::shared_ptr<Impl::Shared> shared;
    std::optional<TuSeq> reserved_tu_seq;
    if (const auto request_position = impl_->requests.find(request);
        request_position != impl_->requests.end()) {
        shared = request_position->second;
        if (shared->raw_bytes != exact_input.size())
            throw std::invalid_argument("PrepareRequestKey was reused for different input");
        if (!std::equal(shared->raw.begin(), shared->raw.end(), exact_input.begin()))
            throw std::invalid_argument("PrepareRequestKey was reused for different input");
        if (const auto route_entry = shared->entries.find(route_key);
            route_entry != shared->entries.end())
            return PreparedTuHandle(impl_->identity, route_entry->second);
    }

    // Do not allocate route state or pin an asynchronous P29V1 fingerprint for
    // a malformed request.  Once admitted, the route captures its fingerprint
    // exactly once and cannot change its reuse decision mid-relationship.
    Impl::RouteState& route = impl_->route_state(route_key);

    if (route.speculative_entries.size() >=
        impl_->authority_limits.max_speculative_tus)
        throw std::length_error(
            "C preparation authority reached its per-route TU window");
    if (exact_input.size() > impl_->authority_limits.max_speculative_raw_bytes ||
        route.speculative_raw_bytes >
            impl_->authority_limits.max_speculative_raw_bytes - exact_input.size())
        throw std::length_error(
            "C preparation authority reached its per-route raw-byte window");
    if (route.profile == ProfileId::P29V1 && route.active_p29_entry)
        throw std::logic_error(
            "P29V1 predecessor must finish NEED/FILL advancement before the next TU");

    if (impl_->entries.size() >= impl_->authority_limits.max_live_entries)
        throw std::length_error("C preparation authority reached its live-entry bound");
    const uint64_t retained_room =
        impl_->authority_limits.max_retained_encoded_bytes - impl_->retained_bytes;
    if (retained_room == 0)
        throw std::length_error("C preparation authority reached its retained-byte bound");

    if (!shared) {
        shared = std::make_shared<Impl::Shared>();
        shared->request = request;
        shared->raw_bytes = exact_input.size();
        shared->raw_digest = raw_digest;
        shared->raw.assign(exact_input.begin(), exact_input.end());
        // Reserve exactly once at C-wide request admission.  The reservation
        // is committed only after encoding and all retained-entry bookkeeping
        // succeeds, so a failed attempt leaves the same TU_SEQ for retry.
        reserved_tu_seq = impl_->p29_authority->reserve_tu_seq();
        shared->tu_seq = *reserved_tu_seq;
    }
    const TuSeq tu_seq = shared->tu_seq;
    const uint64_t entry_id = impl_->next_entry_candidate();
    ZstdTuLimits admission_limits = impl_->zstd_limits;
    admission_limits.max_encoded_body_bytes =
        std::min(admission_limits.max_encoded_body_bytes, retained_room);
    PreparedInputPtr prepared;
    std::vector<uint8_t> next_route_history;
    Digest128 next_route_history_digest{};
    bool advance_route_history = false;
    bool p29_active_started = false;
    try {
        if (route.profile == ProfileId::ZSTD_TU) {
            const ZstdTuEnvelope envelope = impl_->codec.encode(
                HistoryNonce{1}, RelSeq{0}, tu_seq, Digest128{}, exact_input,
                admission_limits);
            prepared = std::make_shared<const PreparedInputEnvelope>(
                PreparedInputEnvelope{envelope.begin, envelope.body});
        } else if (route.profile == ProfileId::ZSTD_ROUTE) {
            const size_t route_limit = static_cast<size_t>(std::min<uint64_t>(
                impl_->zstd_limits.max_history_bytes,
                uint64_t{1} << impl_->zstd_limits.max_window_log));
            next_route_history = route.speculative_route_history;
            if (exact_input.size() >= route_limit) {
                next_route_history.assign(
                    exact_input.end() - static_cast<std::ptrdiff_t>(route_limit),
                    exact_input.end());
            } else {
                const size_t excess = next_route_history.size() + exact_input.size() > route_limit
                    ? next_route_history.size() + exact_input.size() - route_limit
                    : 0;
                if (excess != 0)
                    next_route_history.erase(
                        next_route_history.begin(),
                        next_route_history.begin() + static_cast<std::ptrdiff_t>(excess));
                next_route_history.insert(next_route_history.end(), exact_input.begin(),
                                          exact_input.end());
            }
            if (route.next_speculative_route_rel ==
                std::numeric_limits<uint64_t>::max())
                throw std::overflow_error("ZSTD_ROUTE REL_SEQ space exhausted");
            const RelSeq route_rel{route.next_speculative_route_rel};
            const ZstdRouteEnvelope envelope = impl_->route_codec.encode(
                HistoryNonce{1}, route_rel, tu_seq,
                route.speculative_route_history_digest,
                std::span<const uint8_t>(route.speculative_route_history), exact_input,
                admission_limits);
            prepared = std::make_shared<const PreparedInputEnvelope>(
                PreparedInputEnvelope{envelope.begin, envelope.body});
            next_route_history_digest = digest128(
                std::span<const uint8_t>(next_route_history));
            advance_route_history = true;
        } else if (route.profile == ProfileId::P29V1) {
            if (!shared->p29_source) {
                try {
                    shared->p29_source =
                        impl_->p29_authority->prepare_p29v1_at_seq(
                            exact_input, shared->tu_seq, shared->raw_digest);
                } catch (...) {
                    /* The interner marks itself permanently non-runnable for
                       any failure after processing begins.  Translate only
                       that owner state; the per-TU size check happens before
                       the interner's guarded region and remains an ordinary
                       length_error. */
                    if (!impl_->p29_authority->p29v1_runnable())
                        throw P29V1CapabilityUnavailable(
                            "P29V1 interner is not runnable until READY lease replacement");
                    throw;
                }
            }
            if (shared->p29_source->tu_seq != tu_seq)
                throw std::logic_error("P29V1 shared TU identity changed");
            const CActiveTx& active = route.p29_route->begin_v1(
                shared->p29_source,
                impl_->authority_limits.max_route_state_bytes);
            p29_active_started = true;
            prepared = std::make_shared<const PreparedInputEnvelope>(
                PreparedInputEnvelope{active.begin, active.body});
        } else {
            throw std::logic_error("preparation authority profile is not runnable");
        }
    } catch (...) {
        if (p29_active_started && route.p29_route)
            route.p29_route->abandon_active();
        throw;
    }

    const uint64_t retained = static_cast<uint64_t>(prepared->body.size());
    bool entry_added = false;
    bool retained_added = false;
    try {
        if (retained > impl_->authority_limits.max_retained_encoded_bytes ||
            impl_->retained_bytes > impl_->authority_limits.max_retained_encoded_bytes - retained)
            throw std::length_error("C preparation authority reached its retained-byte bound");

        if (!prepared)
            throw std::logic_error("shared preparation route has no prepared input");
        Impl::Entry entry{request, route_key, shared, prepared, 1, retained,
                          false, false};
        const auto [entry_position, entry_inserted] = impl_->entries.emplace(
            entry_id, std::move(entry));
        (void)entry_position;
        if (!entry_inserted)
            throw std::logic_error("C preparation authority reused an entry identifier");
        entry_added = true;
        if (shared->entries.emplace(route_key, entry_id).second == false)
            throw std::logic_error("C preparation route was admitted twice");
        impl_->requests.emplace(request, shared);
        impl_->retained_bytes += retained;
        retained_added = true;
        route.speculative_entries.push_back(entry_id);
        route.speculative_raw_bytes += exact_input.size();
        if (route.profile == ProfileId::ZSTD_ROUTE)
            ++route.next_speculative_route_rel;
        if (route.profile == ProfileId::P29V1)
            route.active_p29_entry = entry_id;
        impl_->consume_entry();
        if (reserved_tu_seq)
            impl_->p29_authority->commit_tu_seq(*reserved_tu_seq);
        if (advance_route_history) {
            route.speculative_route_history = std::move(next_route_history);
            route.speculative_route_history_digest = next_route_history_digest;
        }
        return PreparedTuHandle(impl_->identity, entry_id);
    } catch (...) {
        if (p29_active_started && route.p29_route)
            route.p29_route->abandon_active();
        if (retained_added)
            impl_->retained_bytes -= retained;
        shared->entries.erase(route_key);
        if (shared->entries.empty())
            impl_->requests.erase(request);
        if (entry_added)
            impl_->entries.erase(entry_id);
        if (!route.speculative_entries.empty() &&
            route.speculative_entries.back() == entry_id) {
            route.speculative_entries.pop_back();
            route.speculative_raw_bytes -= exact_input.size();
            if (route.profile == ProfileId::ZSTD_ROUTE)
                --route.next_speculative_route_rel;
        }
        if (route.active_p29_entry == entry_id)
            route.active_p29_entry.reset();
        throw;
    }
}

std::span<const uint8_t> P50PreparationAuthority::answer_p29v1_need(
    PreparedTuHandle handle, std::span<const uint8_t> inner_need) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument(
            "prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    Impl::Entry& entry = position->second;
    if (entry.route.profile != ProfileId::P29V1 || entry.committed)
        throw std::invalid_argument("prepared-TU handle is not active P29V1");
    Impl::RouteState& route = impl_->route_state(entry.route);
    if (!route.p29_route || route.active_p29_entry != handle.entry_id_)
        throw std::logic_error("P29V1 NEED does not identify the route successor");
    return route.p29_route->build_fill_v1(inner_need);
}

std::vector<uint8_t> P50PreparationAuthority::predicted_p29v1_need(
    PreparedTuHandle handle) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument(
            "prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    Impl::Entry& entry = position->second;
    if (entry.route.profile != ProfileId::P29V1 || entry.committed)
        throw std::invalid_argument("prepared-TU handle is not active P29V1");
    Impl::RouteState& route = impl_->route_state(entry.route);
    if (!route.p29_route || route.active_p29_entry != handle.entry_id_)
        throw std::logic_error("P29V1 NEED does not identify the route successor");
    return route.p29_route->predicted_need_v1();
}

void P50PreparationAuthority::advance_p29v1_speculative(
    PreparedTuHandle handle) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument(
            "prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    Impl::Entry& entry = position->second;
    if (entry.route.profile != ProfileId::P29V1 || entry.committed ||
        entry.speculative_advanced)
        throw std::invalid_argument("prepared-TU handle is not unadvanced P29V1");
    Impl::RouteState& route = impl_->route_state(entry.route);
    if (!route.p29_route || route.active_p29_entry != handle.entry_id_)
        throw std::logic_error("P29V1 advancement does not identify the active TU");
    route.p29_route->advance_speculative_v1();
    entry.speculative_advanced = true;
    route.active_p29_entry.reset();
}

void P50PreparationAuthority::advance_speculative(PreparedTuHandle handle) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument(
            "prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    Impl::Entry& entry = position->second;
    if (entry.committed || entry.speculative_advanced)
        throw std::invalid_argument("prepared-TU is already advanced or committed");
    if (entry.route.profile == ProfileId::P29V1) {
        advance_p29v1_speculative(handle);
        return;
    }
    Impl::RouteState& route = impl_->route_state(entry.route);
    const auto first_unadvanced = std::find_if(
        route.speculative_entries.begin(), route.speculative_entries.end(),
        [this](uint64_t entry_id) {
            const auto candidate = impl_->entries.find(entry_id);
            return candidate == impl_->entries.end() ||
                   !candidate->second.speculative_advanced;
        });
    if (first_unadvanced == route.speculative_entries.end() ||
        *first_unadvanced != handle.entry_id_)
        throw std::logic_error(
            "speculative TU advancement is not in relationship order");
    entry.speculative_advanced = true;
    entry.r2_rebuild_required = false;
}

void P50PreparationAuthority::reset_r2_route_for_recovery(
    PreparationRouteKey route_key, FStoreGuid f_store_guid,
    HistoryNonce history_nonce) {
    impl_->owner.require();
    if (f_store_guid == FStoreGuid{} || history_nonce.value == 0)
        throw std::invalid_argument("R2 recovery reset identity is invalid");
    auto route_position = impl_->routes.find(route_key);
    if (route_position == impl_->routes.end()) {
        if (route_key.profile == ProfileId::P29V1)
            throw std::logic_error("P29V1 recovery has no retained route state");
        route_position = impl_->routes.emplace(
            route_key, std::make_unique<Impl::RouteState>()).first;
        route_position->second->profile = route_key.profile;
    }
    Impl::RouteState& route = *route_position->second;
    if (route.profile != route_key.profile)
        throw std::logic_error("R2 recovery route profile changed");
    uint64_t raw_bytes = 0;
    for (const uint64_t id : route.speculative_entries) {
        const auto position = impl_->entries.find(id);
        if (position == impl_->entries.end() || position->second.committed ||
            position->second.route != route_key ||
            position->second.shared->raw_bytes >
                std::numeric_limits<uint64_t>::max() - raw_bytes)
            throw std::logic_error("R2 recovery suffix ledger is invalid");
        raw_bytes += position->second.shared->raw_bytes;
    }
    if (route_key.profile == ProfileId::P29V1) {
        if (!route.p29_route)
            throw std::logic_error("P29V1 recovery lost its C route state");
        route.p29_route->reset_v1_route(f_store_guid, history_nonce);
    }
    route.committed_route_history.clear();
    route.committed_route_history_digest = digest128(std::span<const uint8_t>{});
    route.speculative_route_history.clear();
    route.speculative_route_history_digest = digest128(std::span<const uint8_t>{});
    route.next_confirmed_route_rel = 0;
    route.next_speculative_route_rel = 0;
    route.active_p29_entry.reset();
    route.speculative_raw_bytes = raw_bytes;
    for (const uint64_t id : route.speculative_entries) {
        Impl::Entry& entry = impl_->entries.at(id);
        entry.speculative_advanced = false;
        entry.r2_rebuild_required = true;
    }
}

void P50PreparationAuthority::rebuild_r2_entry_for_recovery(
    PreparedTuHandle handle, Digest128 f_system_source_fingerprint) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared TU handle belongs to another authority");
    auto entry_position = impl_->entries.find(handle.entry_id_);
    if (entry_position == impl_->entries.end())
        throw std::invalid_argument("recovery TU handle has been released");
    Impl::Entry& entry = entry_position->second;
    if (!entry.r2_rebuild_required || entry.committed)
        throw std::logic_error("recovery TU is not awaiting codec rebuild");
    Impl::RouteState& route = impl_->route_state(entry.route);
    const auto first_unadvanced = std::find_if(
        route.speculative_entries.begin(), route.speculative_entries.end(),
        [this](uint64_t id) {
            const auto candidate = impl_->entries.find(id);
            return candidate == impl_->entries.end() ||
                   !candidate->second.speculative_advanced;
        });
    if (first_unadvanced == route.speculative_entries.end() ||
        *first_unadvanced != handle.entry_id_)
        throw std::logic_error("R2 recovery rebuild is not in relationship order");

    PreparedInputPtr replacement = entry.prepared;
    bool started_p29 = false;
    try {
        if (entry.route.profile == ProfileId::P29V1) {
            if (!route.p29_route || route.active_p29_entry ||
                !entry.shared->p29_source)
                throw std::logic_error("P29V1 recovery cannot start the next retained TU");
            const CActiveTx& active = route.p29_route->begin_v1(
                entry.shared->p29_source,
                impl_->authority_limits.max_route_state_bytes);
            started_p29 = true;
            route.active_p29_entry = handle.entry_id_;
            replacement = std::make_shared<const PreparedInputEnvelope>(
                PreparedInputEnvelope{active.begin, active.body});
            pin_p29v1_system_source_reuse(handle, f_system_source_fingerprint);
        } else if (entry.route.profile == ProfileId::ZSTD_ROUTE) {
            const size_t route_limit = static_cast<size_t>(std::min<uint64_t>(
                impl_->zstd_limits.max_history_bytes,
                uint64_t{1} << impl_->zstd_limits.max_window_log));
            if (route.next_speculative_route_rel ==
                std::numeric_limits<uint64_t>::max())
                throw std::overflow_error("ZSTD_ROUTE recovery REL_SEQ exhausted");
            const ZstdRouteEnvelope envelope = impl_->route_codec.encode(
                HistoryNonce{1}, RelSeq{route.next_speculative_route_rel},
                entry.shared->tu_seq, route.speculative_route_history_digest,
                std::span<const uint8_t>(route.speculative_route_history),
                std::span<const uint8_t>(entry.shared->raw), impl_->zstd_limits);
            (void)route_limit;
            replacement = std::make_shared<const PreparedInputEnvelope>(
                PreparedInputEnvelope{envelope.begin, envelope.body});
        } else if (entry.route.profile != ProfileId::ZSTD_TU) {
            throw std::logic_error("unsupported R2 recovery codec profile");
        }
        const uint64_t retained = static_cast<uint64_t>(replacement->body.size());
        const uint64_t other_retained = impl_->retained_bytes - entry.retained_bytes;
        if (retained > impl_->authority_limits.max_retained_encoded_bytes ||
            other_retained > impl_->authority_limits.max_retained_encoded_bytes - retained)
            throw std::length_error("R2 recovery rebuild exceeds retained encoded-byte cap");
        impl_->retained_bytes = other_retained + retained;
        entry.retained_bytes = retained;
        entry.prepared = std::move(replacement);
        entry.r2_rebuild_required = false;
    } catch (...) {
        if (started_p29 && route.p29_route)
            route.p29_route->abandon_active();
        if (route.active_p29_entry == handle.entry_id_)
            route.active_p29_entry.reset();
        throw;
    }
}

TxBegin P50PreparationAuthority::r2_staged_begin(
    PreparedTuHandle handle, HistoryNonce history_nonce, RelSeq rel_seq,
    Digest128 pre_state_digest) const {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0 ||
        history_nonce.value == 0)
        throw std::invalid_argument("R2 staged-begin identity is invalid");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end() || position->second.committed ||
        position->second.speculative_advanced)
        throw std::invalid_argument("R2 staged-begin TU is not unadvanced");
    const Impl::Entry& entry = position->second;
    if (entry.route.profile == ProfileId::P29V1) {
        const auto route_position = impl_->routes.find(entry.route);
        if (route_position == impl_->routes.end() ||
            !route_position->second->p29_route ||
            route_position->second->active_p29_entry != handle.entry_id_)
            throw std::logic_error("P29V1 R2 begin is not the active CRoute TU");
        const auto& active = route_position->second->p29_route->active();
        if (!active || active->prepared->tu_seq != entry.prepared->begin.tu_seq)
            throw std::logic_error("P29V1 R2 begin lost its active prepared TU");
        const TxBegin& begin = active->begin;
        if (begin.history_nonce != history_nonce || begin.rel_seq != rel_seq ||
            begin.pre_state_digest != pre_state_digest)
            throw std::logic_error("P29V1 speculative cursor differs from its CRoute");
        return begin;
    }
    TxBegin begin = entry.prepared->begin;
    begin.history_nonce = history_nonce;
    begin.rel_seq = rel_seq;
    begin.pre_state_digest = pre_state_digest;
    begin.transaction_digest = compute_transaction_digest(
        begin, entry.prepared->body);
    return begin;
}

Digest128 P50PreparationAuthority::p29v1_system_source_fingerprint(
    PreparedTuHandle handle) const {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument(
            "prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end() ||
        position->second.route.profile != ProfileId::P29V1)
        throw std::invalid_argument("prepared-TU handle is not P29V1");
    const auto route = impl_->routes.find(position->second.route);
    if (route == impl_->routes.end())
        throw std::logic_error("P29V1 route state is absent");
    return route->second->p29v1_system_source_fingerprint;
}

std::optional<bool> P50PreparationAuthority::p29v1_system_source_reuse(
    PreparedTuHandle handle) const {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument(
            "prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end() ||
        position->second.route.profile != ProfileId::P29V1)
        throw std::invalid_argument("prepared-TU handle is not P29V1");
    const auto route = impl_->routes.find(position->second.route);
    if (route == impl_->routes.end() || !route->second->p29_route)
        throw std::logic_error("P29V1 route state is absent");
    return route->second->p29_route->p29v1_system_source_reuse();
}

void P50PreparationAuthority::pin_p29v1_system_source_reuse(
    PreparedTuHandle handle, Digest128 f_fingerprint) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument(
            "prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end() || position->second.committed ||
        position->second.route.profile != ProfileId::P29V1)
        throw std::invalid_argument("prepared-TU handle is not active P29V1");
    Impl::RouteState& route = impl_->route_state(position->second.route);
    if (!route.p29_route || route.active_p29_entry != handle.entry_id_)
        throw std::logic_error(
            "P29V1 fingerprint does not identify the route successor");
    const Digest128 c_fingerprint = route.p29v1_system_source_fingerprint;
    route.p29_route->pin_v1_system_source_reuse(
        c_fingerprint != Digest128{} && c_fingerprint == f_fingerprint);
}

void P50PreparationAuthority::restart_p29v1_transport_retry(
    PreparedTuHandle handle) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument(
            "prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    Impl::Entry& entry = position->second;
    if (entry.route.profile != ProfileId::P29V1 || entry.committed)
        throw std::invalid_argument(
            "prepared-TU handle is not active P29V1");
    Impl::RouteState& route = impl_->route_state(entry.route);
    if (!route.p29_route || route.active_p29_entry != handle.entry_id_)
        throw std::logic_error(
            "P29V1 retry does not identify the route successor");
    route.p29_route->restart_v1_for_transport_retry();
}

PreparedInputPtr P50PreparationAuthority::reset_p29v1_route(
    PreparedTuHandle handle, FStoreGuid f_store_guid,
    HistoryNonce history_nonce) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument(
            "prepared-TU handle does not belong to this C authority");
    auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    Impl::Entry& entry = position->second;
    if (entry.route.profile != ProfileId::P29V1 || entry.committed ||
        f_store_guid == FStoreGuid{} || history_nonce.value == 0)
        throw std::invalid_argument(
            "P29V1 route reset identity is invalid");
    Impl::RouteState& route = impl_->route_state(entry.route);
    if (!route.p29_route || route.active_p29_entry != handle.entry_id_ ||
        route.speculative_entries.size() != 1 || entry.speculative_advanced ||
        !entry.shared->p29_source)
        throw std::logic_error(
            "single-active P29V1 reset does not identify its prepared successor");

    route.p29_route->reset_v1_route(f_store_guid, history_nonce);
    const CActiveTx& active = route.p29_route->begin_v1(
        entry.shared->p29_source,
        impl_->authority_limits.max_route_state_bytes);
    PreparedInputPtr replacement =
        std::make_shared<const PreparedInputEnvelope>(
            PreparedInputEnvelope{active.begin, active.body});
    const uint64_t retained = static_cast<uint64_t>(replacement->body.size());
    const uint64_t other_retained = impl_->retained_bytes - entry.retained_bytes;
    if (retained > impl_->authority_limits.max_retained_encoded_bytes ||
        other_retained >
            impl_->authority_limits.max_retained_encoded_bytes - retained) {
        route.p29_route->abandon_active();
        throw std::length_error(
            "C preparation authority reached its retained-byte bound");
    }
    impl_->retained_bytes = other_retained + retained;
    entry.retained_bytes = retained;
    entry.prepared = replacement;
    entry.speculative_advanced = false;
    return replacement;
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
    Impl::RouteState& route = impl_->route_state(entry.route);
    if (entry.references > 1)
        return --entry.references;
    const auto route_entry = std::find(route.speculative_entries.begin(),
                                       route.speculative_entries.end(),
                                       handle.entry_id_);
    if (!entry.committed && route_entry != route.speculative_entries.end()) {
        if (entry.route.profile == ProfileId::P29V1) {
            if (route.speculative_entries.size() != 1 ||
                route.active_p29_entry != handle.entry_id_ ||
                entry.speculative_advanced)
                throw std::logic_error(
                    "unconfirmed P29V1 TU requires ordered receipt or coordinated reset before release");
            route.p29_route->abandon_active();
            route.active_p29_entry.reset();
        } else if (entry.speculative_advanced) {
            throw std::logic_error(
                "staged TU requires ordered receipt or coordinated reset before release");
        } else if (entry.route.profile == ProfileId::ZSTD_ROUTE &&
                   std::next(route_entry) != route.speculative_entries.end()) {
            throw std::logic_error(
                "ZSTD_ROUTE suffix depends on this TU; reset and rebuild before release");
        }
        route.speculative_raw_bytes -= entry.shared->raw_bytes;
        route.speculative_entries.erase(route_entry);
        if (entry.route.profile == ProfileId::ZSTD_ROUTE) {
            --route.next_speculative_route_rel;
            const size_t limit = static_cast<size_t>(std::min<uint64_t>(
                impl_->zstd_limits.max_history_bytes,
                uint64_t{1} << impl_->zstd_limits.max_window_log));
            route.speculative_route_history = route.committed_route_history;
            for (const uint64_t retained_id : route.speculative_entries) {
                const auto retained = impl_->entries.find(retained_id);
                if (retained == impl_->entries.end())
                    throw std::logic_error("ZSTD_ROUTE speculative ledger is corrupt");
                const auto& raw = retained->second.shared->raw;
                if (raw.size() >= limit) {
                    route.speculative_route_history.assign(
                        raw.end() - static_cast<std::ptrdiff_t>(limit), raw.end());
                } else {
                    const size_t excess = route.speculative_route_history.size() + raw.size() > limit
                        ? route.speculative_route_history.size() + raw.size() - limit
                        : 0;
                    if (excess != 0)
                        route.speculative_route_history.erase(
                            route.speculative_route_history.begin(),
                            route.speculative_route_history.begin() +
                                static_cast<std::ptrdiff_t>(excess));
                    route.speculative_route_history.insert(
                        route.speculative_route_history.end(), raw.begin(), raw.end());
                }
            }
            route.speculative_route_history_digest = digest128(
                std::span<const uint8_t>(route.speculative_route_history));
        }
    }
    if (--entry.references != 0)
        return entry.references;
    impl_->retained_bytes -= entry.retained_bytes;
    const PreparationRouteKey route_key = entry.route;
    const PrepareRequestKey request = entry.request;
    const std::shared_ptr<Impl::Shared> shared = entry.shared;
    impl_->entries.erase(position);
    shared->entries.erase(route_key);
    if (shared->entries.empty())
        impl_->requests.erase(request);
    return 0;
}

void P50PreparationAuthority::accept_commit(PreparedTuHandle handle,
                                           const TxCommit& receipt) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    accept_commit(handle, position->second.prepared->begin, receipt);
}

void P50PreparationAuthority::accept_commit(PreparedTuHandle handle,
                                            const TxBegin& sent_begin,
                                            const TxCommit& receipt) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    auto& entry = position->second;
    if (entry.committed)
        throw std::logic_error("prepared TU already has a confirmed receipt");
    Impl::RouteState& route = impl_->route_state(entry.route);
    if (route.speculative_entries.empty() ||
        route.speculative_entries.front() != handle.entry_id_)
        throw std::logic_error("receiver receipt is not the relationship ledger front");
    const TxBegin& prepared_begin = entry.prepared->begin;
    if (sent_begin.tu_seq != prepared_begin.tu_seq ||
        sent_begin.profile != prepared_begin.profile ||
        sent_begin.body != prepared_begin.body ||
        sent_begin.raw_bytes != prepared_begin.raw_bytes ||
        sent_begin.raw_digest != prepared_begin.raw_digest ||
        (sent_begin.profile == ProfileId::P29V1 &&
         (sent_begin.history_nonce != prepared_begin.history_nonce ||
          sent_begin.rel_seq != prepared_begin.rel_seq ||
          sent_begin.pre_state_digest != prepared_begin.pre_state_digest)) ||
        (sent_begin.profile == ProfileId::ZSTD_ROUTE &&
         sent_begin.rel_seq != prepared_begin.rel_seq) ||
        sent_begin.transaction_digest !=
            compute_transaction_digest(sent_begin, entry.prepared->body) ||
        !same_commit(receipt, sent_begin))
        throw std::logic_error("receiver receipt differs from the retained TU witness");
    if (entry.route.profile == ProfileId::P29V1) {
        if (!entry.speculative_advanced &&
            route.active_p29_entry != handle.entry_id_)
            throw std::logic_error("P29V1 receipt has no active or staged TU");
        route.p29_route->accept_commit(receipt);
    } else if (entry.route.profile == ProfileId::ZSTD_ROUTE) {
        if (sent_begin.rel_seq.value !=
            route.next_confirmed_route_rel)
            throw std::logic_error("ZSTD_ROUTE receipt is not the next confirmed REL_SEQ");
        const size_t limit = static_cast<size_t>(std::min<uint64_t>(
            impl_->zstd_limits.max_history_bytes,
            uint64_t{1} << impl_->zstd_limits.max_window_log));
        std::vector<uint8_t> next_committed_history = route.committed_route_history;
        const auto& raw = entry.shared->raw;
        if (raw.size() >= limit) {
            next_committed_history.assign(
                raw.end() - static_cast<std::ptrdiff_t>(limit), raw.end());
        } else {
            const size_t excess = next_committed_history.size() + raw.size() > limit
                                      ? next_committed_history.size() + raw.size() - limit
                                      : 0;
            if (excess != 0)
                next_committed_history.erase(
                    next_committed_history.begin(),
                    next_committed_history.begin() + static_cast<std::ptrdiff_t>(excess));
            next_committed_history.insert(next_committed_history.end(), raw.begin(), raw.end());
        }
        const Digest128 next_digest = digest128(
            std::span<const uint8_t>(next_committed_history));
        route.committed_route_history = std::move(next_committed_history);
        route.committed_route_history_digest = next_digest;
        ++route.next_confirmed_route_rel;
    } else if (entry.route.profile != ProfileId::ZSTD_TU) {
        throw std::logic_error("unsupported profile in preparation receipt ledger");
    }
    entry.committed = true;
    route.speculative_raw_bytes -= entry.shared->raw_bytes;
    route.speculative_entries.erase(route.speculative_entries.begin());
    if (route.active_p29_entry == handle.entry_id_)
        route.active_p29_entry.reset();
    if (route.speculative_entries.empty() &&
        entry.route.profile == ProfileId::ZSTD_ROUTE) {
        route.speculative_route_history = route.committed_route_history;
        route.speculative_route_history_digest =
            route.committed_route_history_digest;
    }
}

void P50PreparationAuthority::accept_p29v1_commit(
    PreparedTuHandle handle, const TxCommit& receipt) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end() ||
        position->second.route.profile != ProfileId::P29V1)
        throw std::invalid_argument("prepared-TU handle is not P29V1");
    accept_commit(handle, receipt);
}

void P50PreparationAuthority::commit(PreparedTuHandle handle) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    auto& entry = position->second;
    if (entry.committed) return;
    if (entry.speculative_advanced)
        throw std::logic_error(
            "staged TU requires an actual receiver receipt; synthetic commit is R1-only");
    const auto receipt = TxCommit{
        entry.prepared->begin.history_nonce,
        entry.prepared->begin.rel_seq,
        entry.prepared->begin.tu_seq,
        entry.prepared->begin.transaction_digest,
        entry.prepared->begin.raw_digest,
        compute_post_state_digest(entry.prepared->begin.pre_state_digest,
                                  entry.prepared->begin.history_nonce,
                                  entry.prepared->begin.rel_seq,
                                  entry.prepared->begin.tu_seq,
                                  entry.prepared->begin.transaction_digest)};
    accept_commit(handle, receipt);
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

TuSeq P50PreparationAuthority::prepared_tu_seq(PreparedTuHandle handle) const {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    return position->second.prepared->begin.tu_seq;
}

ProfileId P50PreparationAuthority::prepared_profile(PreparedTuHandle handle) const {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    return position->second.prepared->begin.profile;
}

size_t P50PreparationAuthority::live_entry_count() const {
    impl_->owner.check();
    return impl_->entries.size();
}

uint64_t P50PreparationAuthority::retained_encoded_bytes() const {
    impl_->owner.check();
    return impl_->retained_bytes;
}

uint64_t P50PreparationAuthority::p29v1_interner_reserved_bytes() const {
    impl_->owner.check();
    return impl_->p29_authority->p29v1_interner_reserved_bytes();
}

uint64_t P50PreparationAuthority::p29v1_interner_committed_bytes() const {
    impl_->owner.check();
    return impl_->p29_authority->p29v1_interner_committed_bytes();
}

uint64_t P50PreparationAuthority::p29v1_route_state_bytes(
    PreparationRouteKey route_key) const {
    impl_->owner.check();
    const Impl::RouteState& route = impl_->route_state(route_key);
    return route.p29_route ? route.p29_route->p29v1_route_state_bytes() : 0;
}

size_t P50PreparationAuthority::route_history_bytes() const {
    return route_history_bytes(Impl::legacy_route(impl_->profile));
}

size_t P50PreparationAuthority::route_history_bytes(
    PreparationRouteKey route_key) const {
    impl_->owner.check();
    const Impl::RouteState& route = impl_->route_state(route_key);
    return route.committed_route_history.size();
}

Digest128 P50PreparationAuthority::route_history_digest() const {
    return route_history_digest(Impl::legacy_route(impl_->profile));
}

Digest128 P50PreparationAuthority::route_history_digest(
    PreparationRouteKey route_key) const {
    impl_->owner.check();
    return impl_->route_state(route_key).committed_route_history_digest;
}

size_t P50PreparationAuthority::route_history_entries() const {
    return route_history_entries(Impl::legacy_route(impl_->profile));
}

size_t P50PreparationAuthority::route_history_entries(
    PreparationRouteKey route_key) const {
    impl_->owner.check();
    const Impl::RouteState& route = impl_->route_state(route_key);
    return route.committed_route_history.empty() ? 0 : 1;
}

bool P50PreparationAuthority::reset_route(PreparationRouteKey route_key) noexcept {
    try {
        impl_->owner.require();
        for (const auto& [ignored, entry] : impl_->entries) {
            (void)ignored;
            if (entry.route == route_key)
                return false;
        }
        impl_->routes.erase(route_key);
        return true;
    } catch (...) {
        // Teardown cannot report exceptions; false keeps the relationship
        // owner alive rather than silently discarding an active view.
        return false;
    }
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
    if (begin.profile == ProfileId::ZSTD_TU)
        validate_zstd_tu_begin(begin, impl_->zstd_limits);
    else if (begin.profile == ProfileId::ZSTD_ROUTE) {
        // The route envelope has already been validated by its codec; retain
        // the profile and cap checks at the authority boundary as well.
        validate_zstd_tu_limits(impl_->zstd_limits);
        if (begin.body.encoding != kZstdRouteBodyEncoding)
            throw std::invalid_argument(
                "prepared ZSTD_ROUTE profile differs from authority");
        if (begin.body.encoded_bytes > impl_->zstd_limits.max_encoded_body_bytes ||
            begin.raw_bytes > impl_->zstd_limits.max_raw_bytes)
            throw std::length_error("prepared route exceeds authority limits");
    }
    else if (begin.profile == ProfileId::P29V1) {
        if (begin.body.encoding !=
            static_cast<uint16_t>(ProfileId::P29V1))
            throw std::invalid_argument(
                "prepared P29V1 profile differs from authority");
        if (begin.raw_bytes > impl_->zstd_limits.max_raw_bytes ||
            begin.body.encoded_bytes >
                impl_->zstd_limits.max_encoded_body_bytes)
            throw std::length_error(
                "prepared P29V1 input exceeds authority limits");
    }
    else {
        throw std::invalid_argument("prepared profile is unsupported");
    }
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
        bool p29v1_transport_retry = false;
    };

    Impl(std::shared_ptr<P50PreparationAuthority> preparation_value, EndpointCaps cap_value,
         HistoryNonce first_nonce,
         CompletionLog* completion_log, ActionTrace* action_trace,
         std::optional<EndpointRunIdentity> run_identity_value,
         std::function<void(EndpointCancelPermit)> admitted_callback,
         std::function<void(EndpointCancelPermit, EndpointTerminalResult)>
             terminal_callback,
         std::optional<PreparationRouteKey> route)
        : preparation(std::move(preparation_value)), caps(cap_value),
          next_nonce(first_nonce.value),
          completions(completion_log), actions(action_trace),
          run_identity_seed(std::move(run_identity_value)),
          on_run_admitted(std::move(admitted_callback)),
          on_run_terminal(std::move(terminal_callback)) {
        if (actions == nullptr && action_trace_sink_enabled()) {
            owned_actions = std::make_unique<ActionTrace>();
            actions = owned_actions.get();
        }
        if (!preparation)
            throw std::invalid_argument("C endpoint requires its preparation authority");
        c_guid = preparation->c_store_guid();
        validate_caps(caps);
        if (caps.zstd != preparation->zstd_limits())
            throw std::invalid_argument(
                "C endpoint and preparation authority use different ZSTD_TU caps");
        if (first_nonce.value == 0)
            throw std::invalid_argument("first endpoint HISTORY_NONCE must be nonzero");
        (void)route;
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
        actions->record(action_record(action, ActorSide::C, c_guid, *f_guid,
                                      begin.profile, serial, &begin,
                                      begin.history_nonce, begin.rel_seq,
                                      state_digest));
    }

    void record_incarnation_replaced(FStoreGuid previous, FStoreGuid replacement,
                                     const TxBegin& retained, uint64_t serial) {
        if (!actions)
            return;
        ActionRecord value = action_record(ActionType::F_STORE_INCAR_REPLACED, ActorSide::C,
                                           c_guid, replacement, retained.profile,
                                           serial, &retained,
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
        value.begin.pre_state_digest = state;
        value.begin.body = prepared->begin.body;
        value.begin.raw_bytes = prepared->begin.raw_bytes;
        value.begin.raw_digest = prepared->begin.raw_digest;
        value.begin.transaction_digest =
            compute_transaction_digest(value.begin, prepared->body);
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
    std::optional<LinkHello> r2_link_hello;
    std::optional<LinkState> r2_link_state;
    uint64_t r2_sent_ordinal = 0;
    // Includes the exact immutable bundle witness reserved before the first
    // write attempt. A transport failure may leave END delivery ambiguous;
    // recovery therefore reconciles through staged_ordinal, not only the
    // callback-confirmed sent cursor.
    uint64_t r2_staged_ordinal = 0;
    uint64_t r2_confirmed_ordinal = 0;
    uint64_t r2_ack_sent_ordinal = 0;
    uint64_t r2_speculative_rel = 0;
    Digest128 r2_speculative_state{};
    std::map<uint64_t, R2SentBundle> r2_pending_bundles;
    bool r2_recovery_pending = false;
    std::optional<ResetRequest> r2_reset_retry;
    std::optional<Id128> r2_recovery_operation;
    uint64_t r2_recovery_floor = 0;
    uint64_t r2_session_serial = 0;
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
        std::optional<JobBind> p51_binding;
        std::shared_ptr<ProfileDialogue> dialogue;
        bool materializing = false;
        bool global_tu_started = false;
        bool global_staged = false;
        bool global_segment_staged = false;
        CStoreGuid global_c_guid{};
        Key64 global_key{};
        Key64 global_segment_key{};
        size_t global_slot = 0;
        size_t global_segment_slot = 0;
        uint64_t reserved_encoded_bytes = 0;
        uint64_t reserved_raw_bytes = 0;
        uint64_t reserved_window_bytes = 0;
        uint64_t fill_apply_ns = 0;
    };

    struct PreparedBegin {
        Pending pending;
        bool replay = false;
    };

    struct MaterializedInput {
        TxBegin begin;
        TxCommit commit;
        InputRecordStore::PreparedPublish prepared_input;
        uint64_t f_apply_materialize_ns = 0;
    };

    struct RecoveryInstall {
        TxBegin begin{};
        JobBind binding{};
        std::vector<Key64> global_keys;
    };

    struct Route {
        HistoryNonce nonce{};
        RelSeq next_rel{};
        Digest128 state{};
        Digest128 c_system_source_fingerprint{};
        Digest128 f_system_source_fingerprint{};
        bool system_source_reuse = false;
        std::optional<TxCommit> last_commit;
        std::optional<TxBegin> interrupted;
        std::optional<RecoveryInstall> recovery_install;
        std::optional<Pending> pending;
        std::shared_ptr<ProfileDialogue> dialogue;
        std::optional<ProfileId> dialogue_profile;
        // The authenticated Protocol-50 cursor survives payload eviction, but
        // a discarded continuing codec cannot honestly mirror that cursor.
        // Advertise no route until HISTORY_RESET replaces it.
        bool codec_history_reset_required = false;
    };

    struct Namespace {
        bool established = false;
        uint64_t active_session = 0;
        uint64_t last_touch = 0;
        // A codec is part of route identity.  Histories and persistent
        // dialogues for one C/F pair therefore remain disjoint by profile.
        std::map<ProfileId, HistoryNonce> nonce_high_water;
        std::map<ProfileId, Route> routes;
        std::optional<InputRecordKey> last_input;
        std::vector<Key64> p29v1_segments;
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
        std::optional<ProfileId> profile;
        uint64_t candidate_revision = 0;
        HistoryNonce candidate_nonce{};
        RelSeq candidate_rel{};
        Digest128 candidate_c_fingerprint{};
        Digest128 candidate_f_fingerprint{};
        bool activated = false;
    };

    struct Session {
        uint64_t serial = 0;
        FStoreGuid f_guid{};
        std::optional<daemon::P50FSessionOperationId> operation;
        std::optional<sidecar::AbsoluteMonotonicDeadline> deadline;
        std::optional<CStoreGuid> c_guid;
        std::optional<ProfileId> profile;
        uint64_t candidate_revision = 0;
        Digest128 candidate_c_fingerprint{};
        Digest128 candidate_f_fingerprint{};
        std::optional<SessionState> candidate_state;
        std::optional<JobBind> current_p51_binding;
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
            owned_actions = std::make_unique<ActionTrace>();
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
                                .profile = std::nullopt,
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
                       .profile = std::nullopt,
                       .candidate_revision = 0,
                       .candidate_state = std::nullopt,
                       .current_p51_binding = std::nullopt,
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

    Key64 global_segment_key(TuSeq tu_seq) const {
        if (tu_seq.value == KeyLayoutV1::ordinal_mask)
            throw std::overflow_error(
                "P29V1 segment key ordinal space exhausted");
        const auto key = Key64::make(
            ObjectType::P29Segment,
            static_cast<uint16_t>(config.endpoint_generation),
            tu_seq.value + 1);
        if (!key)
            throw std::overflow_error(
                "P29V1 segment key cannot be represented");
        return *key;
    }

    static Route* find_route(Namespace& space, ProfileId profile) {
        const auto position = space.routes.find(profile);
        return position == space.routes.end() ? nullptr : &position->second;
    }

    static const Route* find_route(const Namespace& space, ProfileId profile) {
        const auto position = space.routes.find(profile);
        return position == space.routes.end() ? nullptr : &position->second;
    }

    void release_p29v1_segments(CStoreGuid c_guid, Namespace& space) {
        for (const Key64 key : space.p29v1_segments)
            global_resources->release(c_guid, key);
        space.p29v1_segments.clear();
    }

    void invalidate_p29v1_codec(CStoreGuid c_guid, Namespace& space) {
        Route* const route = find_route(space, ProfileId::P29V1);
        if (route == nullptr ||
            route->dialogue_profile != ProfileId::P29V1)
            return;
        release_p29v1_segments(c_guid, space);
        if (route->dialogue) {
            route->dialogue->reset();
            route->dialogue.reset();
        }
        route->dialogue_profile.reset();
        route->codec_history_reset_required = true;
    }

    void finish_global_pending(Pending& pending, bool crash) {
        if (pending.global_segment_staged && crash) {
            global_resources->crash_install(
                pending.global_c_guid, pending.global_segment_key,
                pending.global_segment_slot);
            pending.global_segment_staged = false;
        }
        if (pending.global_staged && crash) {
            global_resources->crash_install(pending.global_c_guid,
                                            pending.global_key,
                                            pending.global_slot);
            pending.global_staged = false;
        }
        if (pending.global_staged || pending.global_segment_staged)
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
        const bool routes_quiescent = std::all_of(
            space.routes.begin(), space.routes.end(),
            [](const auto& item) {
                return !item.second.pending && !item.second.interrupted &&
                       !item.second.recovery_install;
            });
        return space.active_session == 0 &&
               !namespace_has_live_session(c_guid) &&
               routes_quiescent &&
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
        Namespace& space = namespaces.at(c_guid);
        input_records.evict_namespace(c_guid);
        space.last_input.reset();
        // Input payload eviction never destroys the authenticated outer
        // Protocol-50 cursor. P29V1's receiver dictionary does depend on its
        // charged segments, so discard only that codec and force the normal
        // HISTORY_RESET reconciliation on the next connection.
        invalidate_p29v1_codec(c_guid, space);
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

    static bool same_recovery_binding(const JobBind& left,
                                      const JobBind& right) {
        return left.reservation_id == right.reservation_id &&
               left.relationship_ordinal == right.relationship_ordinal &&
               left.wire_job_id == right.wire_job_id &&
               left.assignment_epoch == right.assignment_epoch &&
               left.assignment_nonce == right.assignment_nonce &&
               left.logical_job == right.logical_job &&
               left.compiler_attempt == right.compiler_attempt &&
               left.source_request_id == right.source_request_id &&
               left.tu_seq == right.tu_seq && left.profile == right.profile &&
               left.raw_bytes == right.raw_bytes &&
               left.raw_digest == right.raw_digest;
    }

    static bool same_recovery_input(const TxBegin& left,
                                    const TxBegin& right) {
        return left.profile == right.profile && left.tu_seq == right.tu_seq &&
               left.raw_bytes == right.raw_bytes &&
               left.raw_digest == right.raw_digest;
    }

    void discard_recovery_install(CStoreGuid c_guid, Route& route) {
        if (!route.recovery_install)
            return;
        const TxBegin retired_begin = route.recovery_install->begin;
        for (const Key64 key : route.recovery_install->global_keys)
            (void)global_resources->discard_crashed_install(c_guid, key);
        route.recovery_install.reset();
        if (route.interrupted &&
            same_recovery_input(*route.interrupted, retired_begin))
            route.interrupted.reset();
    }

    void retain_recovery_install(const Session& session, Route& route,
                                 const Pending& pending) {
        const std::optional<JobBind>& binding = pending.p51_binding
            ? pending.p51_binding : session.current_p51_binding;
        if (!session.c_guid || !binding ||
            (!pending.global_staged && !pending.global_segment_staged))
            return;
        RecoveryInstall retained;
        if (route.recovery_install &&
            same_recovery_binding(route.recovery_install->binding,
                                  *binding))
            retained = *route.recovery_install;
        retained.begin = pending.begin;
        retained.binding = *binding;
        const auto append_key = [&retained](Key64 key) {
            if (key.valid() &&
                std::find(retained.global_keys.begin(),
                          retained.global_keys.end(), key) ==
                    retained.global_keys.end())
                retained.global_keys.push_back(key);
        };
        if (pending.global_staged)
            append_key(pending.global_key);
        if (pending.global_segment_staged)
            append_key(pending.global_segment_key);
        route.recovery_install = std::move(retained);
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

    Route& require_route(const Session& session) {
        Namespace& space = require(session);
        if (!session.profile)
            throw StaleCompletion();
        Route* const route = find_route(space, *session.profile);
        if (route == nullptr)
            throw std::logic_error("F profile route has not been established");
        return *route;
    }

    const Route& require_route(const Session& session) const {
        const Namespace& space = require(session);
        if (!session.profile)
            throw StaleCompletion();
        const Route* const route = find_route(space, *session.profile);
        if (route == nullptr)
            throw std::logic_error("F profile route has not been established");
        return *route;
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
        if (!begin && session.activated && session.c_guid && session.profile) {
            const auto position = namespaces.find(*session.c_guid);
            if (position != namespaces.end()) {
                const Route* const selected =
                    find_route(position->second, *session.profile);
                if (selected == nullptr)
                    return result;
                const Route& route = *selected;
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
        if (!session.c_guid || *session.c_guid != *current.c_guid ||
            !session.profile || session.profile != current.profile ||
            !session.activated)
            throw StaleCompletion();
        const Route& route = require_route(session);
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
        if (!actions || !session.c_guid || !session.profile)
            return;
        HistoryNonce nonce{};
        RelSeq rel{};
        Digest128 state_value = state_override;
        const auto position = namespaces.find(*session.c_guid);
        if (position != namespaces.end() && session.profile) {
            const Route* const selected =
                find_route(position->second, *session.profile);
            if (selected != nullptr) {
                nonce = selected->nonce;
                rel = selected->next_rel;
                if (state_value == Digest128{})
                    state_value = selected->state;
            }
        }
        ActionRecord value = action_record(action, ActorSide::F, *session.c_guid, f_guid,
                                           *session.profile, session.serial,
                                           begin, nonce, rel, state_value);
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

    SessionState snapshot(const SessionHello& hello,
                          SessionSelection selection) const {
        const ProfileId profile =
            require_single_route_profile(selection.negotiated_profiles);
        SessionState result;
        result.wire_revision = selection.wire_revision;
        result.negotiated_profiles = selection.negotiated_profiles;
        result.limits = selection.limits;
        result.f_store_guid = f_guid;
        result.system_source_fingerprint = p29_system_source_fingerprint();
        const auto position = namespaces.find(hello.c_store_guid);
        if (position == namespaces.end())
            return result;
        const Namespace& space = position->second;
        const Route* const route = find_route(space, profile);
        result.namespace_present = space.established;
        result.route_present = route != nullptr &&
                               !route->codec_history_reset_required;
        if (result.route_present) {
            if (hello.system_source_fingerprint !=
                route->c_system_source_fingerprint)
                throw std::logic_error(
                    "SESSION_HELLO fingerprint changed on an existing route");
            result.system_source_fingerprint =
                route->f_system_source_fingerprint;
            result.history_nonce = route->nonce;
            result.next_rel_seq = route->next_rel;
            result.state_digest = route->state;
            result.last_commit = route->last_commit;
        }
        return result;
    }

    SessionState stage(Session& session, const SessionHello& hello,
                       SessionSelection selection) {
        require_incarnation(session);
        if (hello.c_store_guid == CStoreGuid{})
            throw std::invalid_argument("SESSION_HELLO C_STORE_GUID zero is reserved");
        const ProfileId profile =
            require_single_route_profile(selection.negotiated_profiles);
        const uint64_t revision = current_revision(hello.c_store_guid);
        SessionState state = snapshot(hello, selection);
        session.c_guid = hello.c_store_guid;
        session.profile = profile;
        session.candidate_revision = revision;
        session.candidate_c_fingerprint = hello.system_source_fingerprint;
        session.candidate_f_fingerprint = state.system_source_fingerprint;
        session.candidate_state = state;
        LiveSession& live = live_sessions.at(session.serial);
        live.c_guid = hello.c_store_guid;
        live.profile = profile;
        live.candidate_revision = revision;
        live.candidate_c_fingerprint = hello.system_source_fingerprint;
        live.candidate_f_fingerprint = state.system_source_fingerprint;
        if (state.route_present) {
            live.candidate_nonce = state.history_nonce;
            live.candidate_rel = state.next_rel_seq;
        }
        return state;
    }

    void activate(Session& session) {
        require_incarnation(session);
        if (!session.c_guid || !session.profile || !session.candidate_state ||
            session.activated)
            throw std::logic_error("F endpoint candidate cannot activate");
        LiveSession& live = live_sessions.at(session.serial);
        if (!live.c_guid || *live.c_guid != *session.c_guid ||
            live.profile != session.profile || live.activated)
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
        Route* replaced_route = nullptr;
        if (replaced) {
            const auto previous = live_sessions.find(space.active_session);
            if (previous != live_sessions.end() && previous->second.profile)
                replaced_route = find_route(
                    space, *previous->second.profile);
        }
        if (replaced_route != nullptr && replaced_route->pending) {
            replaced_route->interrupted = replaced_route->pending->begin;
            retain_recovery_install(session, *replaced_route,
                                    *replaced_route->pending);
            if (replaced_route->pending->dialogue)
                replaced_route->pending->dialogue->discard_tentative();
            release_pending(*replaced_route->pending);
            replaced_route->pending.reset();
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
        if (!session.c_guid || !session.profile)
            return;
        auto position = namespaces.find(*session.c_guid);
        if (position == namespaces.end() || position->second.active_session != session.serial)
            return;
        Namespace& space = position->second;
        Route* const route = find_route(space, *session.profile);
        if (route != nullptr && route->pending) {
            if (retain_interrupted) {
                route->interrupted = route->pending->begin;
                retain_recovery_install(session, *route, *route->pending);
            } else {
                route->interrupted.reset();
                discard_recovery_install(*session.c_guid, *route);
            }
            if (route->pending->dialogue)
                route->pending->dialogue->discard_tentative();
            release_pending(*route->pending);
            route->pending.reset();
        }
        if (route != nullptr && *session.profile == ProfileId::P29V1 &&
            route->dialogue_profile == ProfileId::P29V1 &&
            route->dialogue && route->dialogue->terminal())
            invalidate_p29v1_codec(*session.c_guid, space);
        record(ActionType::SESSION_DISCONNECTED, session);
        space.active_session = 0;
        touch_namespace_on_disconnect(*session.c_guid, space);
        advance_revision_on_disconnect(*session.c_guid);
    }

    SessionState session_state(const Session& session, SessionSelection selection) const {
        const Namespace& space = require(session);
        if (!session.profile ||
            *session.profile !=
                require_single_route_profile(selection.negotiated_profiles))
            throw StaleCompletion();
        const Route* const route = find_route(space, *session.profile);
        SessionState result;
        result.wire_revision = selection.wire_revision;
        result.negotiated_profiles = selection.negotiated_profiles;
        result.limits = selection.limits;
        result.f_store_guid = f_guid;
        result.system_source_fingerprint = p29_system_source_fingerprint();
        result.namespace_present = space.established;
        result.route_present = route != nullptr &&
                               !route->codec_history_reset_required;
        if (result.route_present) {
            result.system_source_fingerprint =
                route->f_system_source_fingerprint;
            result.history_nonce = route->nonce;
            result.next_rel_seq = route->next_rel;
            result.state_digest = route->state;
            result.last_commit = route->last_commit;
        }
        return result;
    }

    void validate_history_reset(CStoreGuid c_guid, ProfileId profile,
                                const Namespace* space,
                                const HistoryReset& reset) const {
        const Route* const route =
            space == nullptr ? nullptr : find_route(*space, profile);
        if (route != nullptr && (route->pending || route->interrupted))
            throw std::logic_error(
                "HISTORY_RESET arrived while F retained transaction identity");
        if (reset.initial_state_digest !=
            initial_route_digest(c_guid, reset.history_nonce))
            throw std::invalid_argument("HISTORY_RESET digest was not derived locally");
        if (space != nullptr) {
            const auto high_water = space->nonce_high_water.find(profile);
            if (high_water != space->nonce_high_water.end() &&
                reset.history_nonce.value <= high_water->second.value)
                throw std::invalid_argument(
                    "HISTORY_RESET nonce did not advance monotonically for profile");
        }
    }

    void validate_history_reset_candidate(const Session& session,
                                          const HistoryReset& reset) const {
        require_incarnation(session);
        if (!session.c_guid || !session.profile || session.activated)
            throw StaleCompletion();
        const auto position = namespaces.find(*session.c_guid);
        validate_history_reset(*session.c_guid, *session.profile,
                               position == namespaces.end() ? nullptr : &position->second,
                               reset);
    }

    void reset_history(const Session& session, const HistoryReset& reset) {
        Namespace& space = require(session);
        if (!session.profile)
            throw StaleCompletion();
        validate_history_reset(*session.c_guid, *session.profile, &space,
                               reset);
        if (*session.profile == ProfileId::P29V1)
            invalidate_p29v1_codec(*session.c_guid, space);
        std::optional<RecoveryInstall> recovery_install;
        const auto previous_route = space.routes.find(*session.profile);
        if (previous_route != space.routes.end())
            recovery_install = previous_route->second.recovery_install;
        space.established = true;
        space.nonce_high_water[*session.profile] = reset.history_nonce;
        space.routes.insert_or_assign(
            *session.profile,
            Route{.nonce = reset.history_nonce,
                            .next_rel = RelSeq{0},
                            .state = reset.initial_state_digest,
                            .c_system_source_fingerprint =
                                session.candidate_c_fingerprint,
                            .f_system_source_fingerprint =
                                session.candidate_f_fingerprint,
                            .system_source_reuse =
                                session.candidate_c_fingerprint != Digest128{} &&
                                session.candidate_c_fingerprint ==
                                    session.candidate_f_fingerprint,
                            .last_commit = std::nullopt,
                            .interrupted = std::nullopt,
                            .recovery_install = std::move(recovery_install),
                            .pending = std::nullopt,
                            .dialogue = nullptr,
                            .dialogue_profile = std::nullopt,
                            .codec_history_reset_required = false});
        record(ActionType::HISTORY_RESET, session);
    }

    PreparedBegin prepare_begin(const Route& route, ProfileId route_profile,
                                const TxBegin& begin,
                                uint32_t negotiated_profiles,
                                CStoreGuid c_store_guid) const {
        if (route.codec_history_reset_required)
            throw std::logic_error(
                "TX_BEGIN arrived before required codec HISTORY_RESET");
        if (begin.history_nonce != route.nonce || begin.rel_seq != route.next_rel ||
            begin.pre_state_digest != route.state)
            throw std::logic_error("TX_BEGIN differs from the F route cursor");
        if (route.next_rel.value == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("F endpoint REL_SEQ space exhausted");
        const uint32_t transaction_profile_bit = profile_bit(begin.profile);
        if (transaction_profile_bit == 0 ||
            (transaction_profile_bit & negotiated_profiles) != transaction_profile_bit)
            throw std::invalid_argument("TX_BEGIN profile was not negotiated");
        if (begin.profile != route_profile)
            throw std::invalid_argument(
                "TX_BEGIN profile differs from its negotiated F route");
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
                                      .system_source_reuse =
                                          route.system_source_reuse,
                                      .max_encoded_body_bytes = caps.zstd.max_encoded_body_bytes,
                                      .max_raw_bytes = caps.zstd.max_raw_bytes,
                                      .max_window_log = caps.zstd.max_window_log,
                                      .max_history_bytes = caps.zstd.max_history_bytes}));
        if ((begin.profile == ProfileId::P29V1 ||
             begin.profile == ProfileId::ZSTD_ROUTE) && route.dialogue)
            result.pending.dialogue = route.dialogue;
        return result;
    }

    PreparedBegin prepare_begin_candidate(const Session& session, const TxBegin& begin,
                                          uint32_t negotiated_profiles) const {
        require_incarnation(session);
        if (!session.c_guid || !session.profile || session.activated)
            throw StaleCompletion();
        const auto position = namespaces.find(*session.c_guid);
        if (position == namespaces.end())
            throw std::logic_error("TX_BEGIN arrived before HISTORY_RESET");
        const Route* const route =
            find_route(position->second, *session.profile);
        if (route == nullptr)
            throw std::logic_error("TX_BEGIN arrived before HISTORY_RESET");
        return prepare_begin(*route, *session.profile, begin,
                             negotiated_profiles, *session.c_guid);
    }

    bool install_begin(const Session& session, PreparedBegin prepared) {
        Route& route = require_route(session);
        if (route.pending)
            throw std::logic_error("F endpoint already has one active transaction");
        if (route.interrupted && route.interrupted != prepared.pending.begin)
            throw StaleCompletion();
        bool same_recovery_install = false;
        if (route.recovery_install) {
            same_recovery_install = session.current_p51_binding &&
                prepared.pending.begin.profile ==
                    route.recovery_install->begin.profile &&
                same_recovery_input(prepared.pending.begin,
                                    route.recovery_install->begin) &&
                same_recovery_binding(*session.current_p51_binding,
                                      route.recovery_install->binding);
            if (!same_recovery_install &&
                config.p51_source_reservation_terminal &&
                config.p51_source_reservation_terminal(
                    route.recovery_install->binding))
                discard_recovery_install(*session.c_guid, route);
            else if (!same_recovery_install)
                throw std::logic_error(
                    "F route still owns a live interrupted source reservation");
        }
        if (prepared.pending.begin.profile == ProfileId::P29V1 ||
            prepared.pending.begin.profile == ProfileId::ZSTD_ROUTE) {
            if (!route.dialogue) {
                route.dialogue = prepared.pending.dialogue;
                route.dialogue_profile = prepared.pending.begin.profile;
            } else {
                if (route.dialogue_profile != prepared.pending.begin.profile)
                    throw std::logic_error(
                        "persistent profile changed without HISTORY_RESET");
                prepared.pending.dialogue = route.dialogue;
            }
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
                const bool retry = replay || (same_recovery_install &&
                    global_resources->install_retry_required(
                        *session.c_guid, prepared.pending.global_key));
                global_resources->begin_install(
                    *session.c_guid, prepared.pending.global_key,
                    begin.raw_digest, begin.raw_bytes, *slot, retry);
                prepared.pending.global_staged = true;
            }
        } catch (...) {
            release_pending(prepared.pending);
            throw;
        }
        route.interrupted.reset();
        route.pending = std::move(prepared.pending);
        record(replay ? ActionType::ACTIVE_REPLAYED : ActionType::TX_BEGIN, session, &begin);
        return replay;
    }

    bool begin(const Session& session, const TxBegin& begin,
               uint32_t negotiated_profiles) {
        const Route& route = require_route(session);
        return install_begin(
            session, prepare_begin(route, *session.profile, begin,
                                   negotiated_profiles, *session.c_guid));
    }

    void append_body(const Session& session, BodyMessage message) {
        Route& route = require_route(session);
        if (!route.pending)
            throw std::logic_error("BODY has no F active transaction");
        Pending& pending = *route.pending;
        pending.dialogue->append_body(message);
        if (pending.dialogue->state() == ProfileDialogueState::BodyClosed)
            record(ActionType::BODY_COMPLETE, session, &pending.begin);
    }

    void receive_need(const Session& session, const NeedMessage& message) {
        Route& route = require_route(session);
        if (!route.pending)
            throw std::logic_error("NEED has no F active transaction");
        route.pending->dialogue->receive_need(message);
    }

    void receive_fill(const Session& session, const FillMessage& message) {
        Route& route = require_route(session);
        if (!route.pending)
            throw std::logic_error("FILL has no F active transaction");
        Pending& pending = *route.pending;
        const auto started = std::chrono::steady_clock::now();
        pending.dialogue->receive_fill(message);
        add_saturating(pending.fill_apply_ns,
                       elapsed_nanoseconds(started));
    }

    bool body_complete(const Session& session) const {
        const Route& route = require_route(session);
        if (!route.pending)
            throw std::logic_error("BODY has no F active transaction");
        return route.pending->dialogue->state() ==
               ProfileDialogueState::BodyClosed;
    }

    ServerMaterializationJob begin_materialization(
        const Session& session, std::function<void()> before_materialize) {
        Route& route = require_route(session);
        if (!route.pending)
            throw std::logic_error("F endpoint has no active transaction");
        Pending& pending = *route.pending;
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
        Route& route = require_route(session);
        if (!route.pending)
            throw StaleCompletion();
        Pending& pending = *route.pending;
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
        if (pending.begin.profile == ProfileId::P29V1) {
            const uint64_t segment_bytes =
                pending.dialogue->pending_segment_bytes();
            const Digest128 segment_digest =
                pending.dialogue->pending_segment_digest();
            if (segment_bytes > pending.begin.raw_bytes ||
                (segment_bytes == 0) != (segment_digest == Digest128{}))
                throw std::logic_error(
                    "P29V1 pending segment observation is invalid");
            if (segment_bytes != 0) {
                try {
                    if (!pending.global_staged)
                        throw std::logic_error(
                            "P29V1 segment lacks its staged input record");
                    space.p29v1_segments.reserve(
                        space.p29v1_segments.size() + 1);
                    const auto slot =
                        global_resources->first_free_staging_slot();
                    if (!slot)
                        throw std::length_error(
                            "F endpoint P29V1 staging-slot pool is exhausted");
                    pending.global_segment_key =
                        global_segment_key(pending.begin.tu_seq);
                    pending.global_segment_slot = *slot;
                    const bool retry =
                        global_resources->install_retry_required(
                            *session.c_guid, pending.global_segment_key);
                    global_resources->begin_install(
                        *session.c_guid, pending.global_segment_key,
                        segment_digest, segment_bytes, *slot, retry);
                    pending.global_segment_staged = true;
                } catch (...) {
                    pending.dialogue->discard_tentative();
                    release_pending(pending);
                    throw;
                }
            }
        }
        record(ActionType::INPUT_MATERIALIZED, session, &pending.begin);
        uint64_t f_apply_materialize_ns = pending.fill_apply_ns;
        add_saturating(f_apply_materialize_ns, completion.materialize_ns);
        return MaterializedInput{completion.begin, completion.commit,
                                 std::move(completion.prepared_input),
                                 f_apply_materialize_ns};
    }

    InputJobState select_materialized_job_state(
        const Session& session, const MaterializedInput& materialized) {
        const Route& route = require_route(session);
        if (!route.pending)
            throw std::logic_error("F endpoint has no active transaction");
        const Pending& pending = *route.pending;
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
        Route& route = require_route(session);
        if (!route.pending)
            throw std::logic_error("F endpoint has no active transaction");
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
        if (pending.global_segment_staged) {
            const Digest128 segment_digest =
                pending.dialogue->pending_segment_digest();
            if (std::find(space.p29v1_segments.begin(),
                          space.p29v1_segments.end(),
                          pending.global_segment_key) !=
                space.p29v1_segments.end())
                throw std::logic_error(
                    "P29V1 segment key was already published");
            if (job_state == InputJobState::Open) {
                if (!pending.global_staged)
                    throw std::logic_error(
                        "P29V1 pair publication lost its staged input");
                global_resources->preflight_publish_pair(
                    *session.c_guid, pending.global_segment_key,
                    pending.global_segment_slot, segment_digest,
                    pending.global_key, pending.global_slot,
                    materialized.begin.raw_digest);
            }
            global_resources->publish(
                *session.c_guid, pending.global_segment_key,
                pending.global_segment_slot, segment_digest);
            pending.global_segment_staged = false;
            space.p29v1_segments.push_back(pending.global_segment_key);
        }
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
        if (route.recovery_install && pending.p51_binding &&
            same_recovery_binding(route.recovery_install->binding,
                                  *pending.p51_binding))
            route.recovery_install.reset();
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
    std::map<uint64_t, std::weak_ptr<ServerIoState>> pending_r2_setup;
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
                                                        EndpointTerminalResult)> on_run_terminal,
                                     std::optional<PreparationRouteKey> route)
    : impl_(std::make_unique<Impl>(std::move(preparation), caps, first_history_nonce, completions,
                                   actions, std::move(run_identity_seed),
                                   std::move(on_run_admitted),
                                   std::move(on_run_terminal), std::move(route))) {}

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

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
boost::asio::awaitable<LinkState> P50ClientEndpoint::open_r2_link(
    tcp::socket& socket, LinkHello hello,
    std::chrono::steady_clock::time_point deadline) {
    impl_->owner.require();
    const bool reconnect = hello.start_mode == LinkStartMode::Reconnect;
    const bool initial = hello.start_mode == LinkStartMode::Initial;
    const bool prior_link_matches = reconnect && impl_->r2_link_hello &&
        impl_->r2_link_state &&
        impl_->r2_link_hello->relationship_id == hello.relationship_id &&
        impl_->r2_link_hello->profile == hello.profile &&
        impl_->r2_link_hello->c_store_guid == hello.c_store_guid &&
        impl_->r2_link_hello->c_store_generation == hello.c_store_generation &&
        impl_->r2_link_hello->f_store_guid == hello.f_store_guid &&
        impl_->r2_link_hello->f_store_generation == hello.f_store_generation &&
        impl_->r2_link_hello->c_control_generation == hello.c_control_generation &&
        impl_->r2_link_hello->c_control_attempt == hello.c_control_attempt &&
        impl_->r2_link_hello->relationship_epoch == hello.relationship_epoch &&
        hello.physical_link_generation >
            impl_->r2_link_hello->physical_link_generation &&
        hello.verified_receipt_floor ==
            (impl_->r2_recovery_operation
                 ? impl_->r2_recovery_floor
                 : impl_->r2_confirmed_ordinal);
    if ((!initial && !reconnect) || (initial && impl_->r2_link_state) ||
        (reconnect && !prior_link_matches) ||
        !socket.is_open() || deadline <= std::chrono::steady_clock::now() ||
        hello.revision != 2 || hello.c_store_guid != impl_->c_guid ||
        hello.profile != impl_->caps.profile || hello.window == 0 ||
        hello.window > 30 || hello.physical_link_generation == 0 ||
        hello.max_frame_payload < kR2MandatoryControlFramePayload ||
        (initial && hello.verified_receipt_floor != 0))
        throw std::invalid_argument("invalid initial R2 C link request");
    const auto executor = co_await asio::this_coro::executor;
    SocketDeadlineGuard deadline_guard(executor, socket, deadline);
    EndpointIoControl control;
    CompletionStamp stamp;
    stamp.actor = ActorSide::C;
    stamp.c_store_guid = impl_->c_guid;
    stamp.operation = AsyncOperationKind::WriteFragment;
    const auto verify = [this, deadline](const CompletionStamp&) {
        impl_->owner.require();
        if (std::chrono::steady_clock::now() >= deadline)
            throw boost::system::system_error(asio::error::timed_out);
    };
    const uint32_t frame_cap = std::min(hello.max_frame_payload,
                                        impl_->caps.wire.max_frame_payload);
    co_await async_write_message(socket, Message{hello}, frame_cap, stamp,
                                 impl_->completions, control, verify);
    stamp.operation = AsyncOperationKind::ReadHeader;
    Frame frame = co_await async_read_frame(socket, frame_cap, stamp,
                                            impl_->completions, verify);
    if (frame.type != MessageType::LINK_STATE)
        throw std::invalid_argument("R2 LINK_HELLO did not receive LINK_STATE");
    const LinkState state = decode_as<LinkState>(frame);
    if (state.revision != hello.revision || state.profile != hello.profile ||
        state.window != hello.window || state.reservation_id != hello.reservation_id ||
        state.relationship_id != hello.relationship_id ||
        (initial && state.relationship_epoch != hello.relationship_epoch) ||
        (reconnect && state.relationship_epoch < hello.relationship_epoch) ||
        state.physical_link_generation != hello.physical_link_generation ||
        state.c_store_guid != hello.c_store_guid ||
        state.c_store_generation != hello.c_store_generation ||
        state.c_control_generation != hello.c_control_generation ||
        state.c_control_attempt != hello.c_control_attempt ||
        state.f_store_guid != hello.f_store_guid ||
        state.f_store_generation != hello.f_store_generation ||
        state.selected_max_frame_payload < kR2MandatoryControlFramePayload ||
        state.selected_max_frame_payload > frame_cap ||
        state.selected_max_raw_bytes > hello.max_raw_bytes ||
        state.selected_max_encoded_bytes > hello.max_encoded_bytes ||
        state.selected_max_output_bytes > hello.max_output_bytes ||
        state.selected_max_raw_bytes == 0 || state.selected_max_encoded_bytes == 0 ||
        state.selected_max_output_bytes == 0 || state.history_nonce.value == 0 ||
        (initial && (state.next_rel_seq.value != 0 ||
                     state.committed_prefix_k != 0 ||
                     state.acknowledged_prefix_q != 0 ||
                     state.state_digest != initial_route_digest(
                         impl_->c_guid, state.history_nonce))) ||
        (reconnect &&
         (state.committed_prefix_k < hello.verified_receipt_floor ||
          state.acknowledged_prefix_q > state.committed_prefix_k ||
          state.committed_prefix_k > impl_->r2_staged_ordinal)))
        throw std::invalid_argument("R2 LINK_STATE differs from initial C link offer");
    impl_->f_guid = state.f_store_guid;
    impl_->route_known = true;
    impl_->history_nonce = state.history_nonce;
    impl_->next_rel = state.next_rel_seq;
    impl_->state = state.state_digest;
    impl_->r2_link_hello = std::move(hello);
    impl_->r2_link_state = state;
    if (initial) {
        impl_->r2_sent_ordinal = 0;
        impl_->r2_staged_ordinal = 0;
        impl_->r2_confirmed_ordinal = 0;
        impl_->r2_ack_sent_ordinal = 0;
        impl_->r2_speculative_rel = state.next_rel_seq.value;
        impl_->r2_speculative_state = state.state_digest;
        impl_->r2_pending_bundles.clear();
        impl_->r2_recovery_pending = false;
    } else {
        impl_->r2_recovery_pending = true;
    }
    impl_->r2_session_serial = impl_->allocate_session();
    co_return state;
}

boost::asio::awaitable<R2RecoveryResult> P50ClientEndpoint::recover_r2_link(
    tcp::socket& socket, LinkHello hello,
    std::span<const R2SentBundle> witnesses, uint64_t verified_floor_a,
    Id128 operation_id, uint64_t new_relationship_epoch,
    HistoryNonce new_history_nonce,
    std::chrono::steady_clock::time_point deadline) {
    impl_->owner.require();
    if (!impl_->r2_link_hello || hello.start_mode != LinkStartMode::Reconnect ||
        operation_id == Id128{} || new_history_nonce.value == 0 ||
        hello.verified_receipt_floor != verified_floor_a ||
        deadline <= std::chrono::steady_clock::now())
        throw std::invalid_argument("invalid C R2 reconnect/recovery request");
    LinkState state = co_await open_r2_link(socket, hello, deadline);
    if (!impl_->r2_recovery_pending ||
        (state.relationship_epoch != hello.relationship_epoch &&
         (!impl_->r2_reset_retry ||
          state.relationship_epoch !=
              impl_->r2_reset_retry->new_relationship_epoch)) ||
        (state.history_nonce != hello.history_nonce &&
         (!impl_->r2_reset_retry ||
          state.history_nonce != impl_->r2_reset_retry->new_history_nonce)) ||
        state.acknowledged_prefix_q > state.committed_prefix_k ||
        state.committed_prefix_k < verified_floor_a ||
        state.committed_prefix_k > impl_->r2_staged_ordinal ||
        (!impl_->r2_reset_retry &&
         new_relationship_epoch <= state.relationship_epoch) ||
        (!impl_->r2_reset_retry && new_history_nonce == state.history_nonce))
        throw std::invalid_argument("F R2 reconnect state cannot be reconciled");

    if (impl_->r2_recovery_operation) {
        if (*impl_->r2_recovery_operation != operation_id ||
            impl_->r2_recovery_floor != verified_floor_a ||
            !impl_->r2_reset_retry ||
            impl_->r2_reset_retry->new_relationship_epoch !=
                new_relationship_epoch ||
            impl_->r2_reset_retry->new_history_nonce != new_history_nonce)
            throw std::invalid_argument(
                "R2 recovery retry changed its stable reset operation");
    } else {
        ResetRequest retry;
        retry.relationship_id = hello.relationship_id;
        retry.old_relationship_epoch = hello.relationship_epoch;
        retry.new_relationship_epoch = new_relationship_epoch;
        retry.physical_link_generation = hello.physical_link_generation;
        retry.operation_id = operation_id;
        retry.settled_prefix_k = state.committed_prefix_k;
        retry.old_history_nonce = hello.history_nonce;
        retry.new_history_nonce = new_history_nonce;
        impl_->r2_recovery_operation = operation_id;
        impl_->r2_recovery_floor = verified_floor_a;
        impl_->r2_reset_retry = retry;
    }

    const uint64_t prepared_prefix_p = impl_->r2_staged_ordinal;
    if (prepared_prefix_p < verified_floor_a ||
        prepared_prefix_p - verified_floor_a > state.window ||
        witnesses.size() != prepared_prefix_p - verified_floor_a)
        throw std::length_error("C R2 recovery witness suffix exceeds its bounded window");
    for (size_t index = 0; index != witnesses.size(); ++index) {
        const R2SentBundle& witness = witnesses[index];
        const auto retained = impl_->r2_pending_bundles.find(
            witness.binding.relationship_ordinal);
        if (witness.binding.relationship_ordinal != verified_floor_a + index + 1 ||
            witness.begin.relationship_ordinal !=
                witness.binding.relationship_ordinal ||
            witness.binding_digest != compute_r2_binding_digest(witness.binding) ||
            witness.transaction_digest == Digest128{} ||
            retained == impl_->r2_pending_bundles.end() ||
            retained->second.binding != witness.binding ||
            retained->second.begin != witness.begin ||
            retained->second.binding_digest != witness.binding_digest ||
            retained->second.transaction_digest != witness.transaction_digest ||
            retained->second.prepared != witness.prepared)
            throw std::invalid_argument("C R2 recovery ledger is not contiguous");
    }

    const uint32_t frame_cap = state.selected_max_frame_payload;
    const auto executor = co_await asio::this_coro::executor;
    SocketDeadlineGuard deadline_guard(executor, socket, deadline);
    EndpointIoControl control;
    CompletionStamp stamp;
    stamp.actor = ActorSide::C;
    stamp.operation = AsyncOperationKind::WriteFragment;
    stamp.c_store_guid = impl_->c_guid;
    stamp.f_store_guid = state.f_store_guid;
    stamp.session_serial = impl_->r2_session_serial;
    const auto verify = [this, deadline](const CompletionStamp&) {
        impl_->owner.require();
        if (std::chrono::steady_clock::now() >= deadline)
            throw boost::system::system_error(asio::error::timed_out);
    };

    RecoverBegin begin;
    begin.relationship_id = hello.relationship_id;
    begin.relationship_epoch = hello.relationship_epoch;
    begin.physical_link_generation = hello.physical_link_generation;
    begin.operation_id = operation_id;
    begin.verified_floor_a = verified_floor_a;
    begin.prepared_prefix_p = prepared_prefix_p;
    begin.witness_count = static_cast<uint32_t>(witnesses.size());
    std::vector<RecoverWitness> recovery_witnesses;
    recovery_witnesses.reserve(witnesses.size());
    for (const R2SentBundle& sent : witnesses) {
        RecoverWitness witness;
        witness.relationship_id = hello.relationship_id;
        witness.relationship_epoch = hello.relationship_epoch;
        witness.physical_link_generation = hello.physical_link_generation;
        witness.operation_id = operation_id;
        witness.relationship_ordinal = sent.binding.relationship_ordinal;
        witness.binding_digest = sent.binding_digest;
        witness.transaction_digest = sent.transaction_digest;
        witness.inner = sent.begin.inner;
        recovery_witnesses.push_back(witness);
    }
    const RecoverEnd end{hello.relationship_id,
                         hello.relationship_epoch,
                         hello.physical_link_generation,
                         operation_id,
                         static_cast<uint32_t>(recovery_witnesses.size()),
                         compute_r2_recovery_transcript_digest(
                             begin, recovery_witnesses)};
    co_await async_write_message(socket, Message{begin}, frame_cap, stamp,
                                 impl_->completions, control, verify);
    for (const RecoverWitness& witness : recovery_witnesses)
        co_await async_write_message(socket, Message{witness}, frame_cap, stamp,
                                     impl_->completions, control, verify);
    co_await async_write_message(socket, Message{end}, frame_cap, stamp,
                                 impl_->completions, control, verify);

    std::vector<R2TxCommit> receipts;
    const uint64_t expected_count =
        state.committed_prefix_k - verified_floor_a;
    if (expected_count > recovery_witnesses.size())
        throw std::invalid_argument("F recovery K exceeds C's prepared prefix");
    receipts.reserve(static_cast<size_t>(expected_count));
    for (uint64_t index = 0; index != expected_count; ++index) {
        stamp.operation = AsyncOperationKind::ReadHeader;
        Frame receipt_frame = co_await async_read_frame(
            socket, frame_cap, stamp, impl_->completions, verify);
        if (receipt_frame.type != MessageType::RECEIPTS)
            throw std::invalid_argument("F recovery omitted an exact receipt row");
        const ReceiptRow row = decode_as<ReceiptRow>(receipt_frame);
        const R2SentBundle& sent = witnesses[static_cast<size_t>(index)];
        if (row.relationship_id != hello.relationship_id ||
            row.relationship_epoch != hello.relationship_epoch ||
            row.physical_link_generation != hello.physical_link_generation ||
            row.operation_id != operation_id ||
            row.receipt.relationship_ordinal != sent.binding.relationship_ordinal ||
            row.receipt.binding_digest != sent.binding_digest ||
            row.receipt.transaction_digest != sent.transaction_digest ||
            !same_commit(row.receipt.inner, sent.begin.inner))
            throw std::invalid_argument("F recovery receipt differs from retained witness");
        receipts.push_back(row.receipt);
    }
    stamp.operation = AsyncOperationKind::ReadHeader;
    Frame receipts_end_frame = co_await async_read_frame(
        socket, frame_cap, stamp, impl_->completions, verify);
    if (receipts_end_frame.type != MessageType::RECEIPTS)
        throw std::invalid_argument("F recovery omitted the receipt interval end");
    const ReceiptsEnd receipts_end = decode_as<ReceiptsEnd>(receipts_end_frame);
    if (receipts_end.relationship_id != hello.relationship_id ||
        receipts_end.relationship_epoch != hello.relationship_epoch ||
        receipts_end.physical_link_generation != hello.physical_link_generation ||
        receipts_end.operation_id != operation_id ||
        receipts_end.verified_floor_a != verified_floor_a ||
        receipts_end.committed_prefix_k != state.committed_prefix_k ||
        receipts_end.acknowledged_prefix_q != state.acknowledged_prefix_q ||
        receipts_end.receipt_count != receipts.size())
        throw std::invalid_argument("F recovery interval end contradicts LINK_STATE");

    // Advance the client-side accepted cursor only after the entire interval
    // has been verified against the immutable sent witness list.
    for (size_t index = 0; index != receipts.size(); ++index) {
        const R2SentBundle& sent = witnesses[index];
        const R2TxCommit& receipt = receipts[index];
        const uint64_t ordinal =
            sent.binding.relationship_ordinal;
        if (ordinal <= impl_->r2_confirmed_ordinal)
            continue;  // This exact row was applied before a lost RESET reply.
        if (impl_->next_rel.value == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("R2 C REL_SEQ space exhausted during recovery");
        impl_->record(ActionType::COMMIT_ACCEPTED, sent.begin.inner,
                      impl_->r2_session_serial, receipt.inner.post_state_digest);
        impl_->preparation->accept_commit(sent.prepared, sent.begin.inner,
                                          receipt.inner);
        impl_->state = receipt.inner.post_state_digest;
        ++impl_->next_rel.value;
    }
    impl_->r2_confirmed_ordinal = state.committed_prefix_k;
    impl_->r2_link_state->committed_prefix_k = state.committed_prefix_k;

    ResetRequest reset = *impl_->r2_reset_retry;
    reset.physical_link_generation = hello.physical_link_generation;
    stamp.operation = AsyncOperationKind::WriteFragment;
    co_await async_write_message(socket, Message{reset}, frame_cap, stamp,
                                 impl_->completions, control, verify);
    stamp.operation = AsyncOperationKind::ReadHeader;
    Frame reset_ack_frame = co_await async_read_frame(
        socket, frame_cap, stamp, impl_->completions, verify);
    if (reset_ack_frame.type != MessageType::RESET_ACK)
        throw std::invalid_argument("F recovery reset did not return RESET_ACK");
    const ResetAck reset_ack = decode_as<ResetAck>(reset_ack_frame);
    const Digest128 expected_initial =
        initial_route_digest(impl_->c_guid, new_history_nonce);
    if (reset_ack.request != reset ||
        reset_ack.initial_state_digest != expected_initial ||
        reset_ack.next_rel_seq.value != 0)
        throw std::invalid_argument("F RESET_ACK differs from exact recovery reset");
    ResetConfirm confirm{hello.relationship_id, new_relationship_epoch,
                         hello.physical_link_generation, operation_id,
                         new_history_nonce, state.committed_prefix_k};
    stamp.operation = AsyncOperationKind::WriteFragment;
    co_await async_write_message(socket, Message{confirm}, frame_cap, stamp,
                                 impl_->completions, control, verify);

    hello.relationship_epoch = new_relationship_epoch;
    hello.history_nonce = new_history_nonce;
    hello.verified_receipt_floor = state.committed_prefix_k;
    impl_->r2_link_hello = hello;
    impl_->r2_link_state->relationship_epoch = new_relationship_epoch;
    impl_->r2_link_state->history_nonce = new_history_nonce;
    impl_->r2_link_state->next_rel_seq = RelSeq{};
    impl_->r2_link_state->state_digest = expected_initial;
    impl_->r2_link_state->committed_prefix_k = state.committed_prefix_k;
    impl_->r2_link_state->acknowledged_prefix_q = state.committed_prefix_k;
    impl_->history_nonce = new_history_nonce;
    impl_->next_rel = RelSeq{};
    impl_->state = expected_initial;
    impl_->r2_sent_ordinal = state.committed_prefix_k;
    impl_->r2_staged_ordinal = state.committed_prefix_k;
    impl_->r2_confirmed_ordinal = state.committed_prefix_k;
    impl_->r2_ack_sent_ordinal = state.committed_prefix_k;
    impl_->r2_speculative_rel = 0;
    impl_->r2_speculative_state = expected_initial;
    impl_->r2_pending_bundles.clear();
    impl_->r2_recovery_pending = false;
    impl_->r2_reset_retry.reset();
    impl_->r2_recovery_operation.reset();
    impl_->r2_recovery_floor = 0;
    impl_->r2_session_serial = impl_->allocate_session();

    R2RecoveryResult result;
    result.link_state = *impl_->r2_link_state;
    result.committed_receipts = std::move(receipts);
    result.reset_request = reset;
    co_return result;
}

boost::asio::awaitable<R2SentBundle> P50ClientEndpoint::write_r2_bundle(
    tcp::socket& socket, JobBind binding, PreparedTuHandle prepared,
    std::chrono::steady_clock::time_point deadline,
    EndpointIoControl control) {
    impl_->owner.require();
    if (!impl_->r2_link_hello || !impl_->r2_link_state ||
        impl_->r2_recovery_pending || !socket.is_open() ||
        deadline <= std::chrono::steady_clock::now())
        throw std::logic_error("R2 writer has no idle live link");
    const LinkHello& link = *impl_->r2_link_hello;
    const LinkState& link_state = *impl_->r2_link_state;
    if (binding.relationship_ordinal != impl_->r2_sent_ordinal + 1 ||
        impl_->r2_sent_ordinal - impl_->r2_ack_sent_ordinal >= link.window ||
        impl_->r2_pending_bundles.contains(binding.relationship_ordinal) ||
        binding.physical_link_generation != link.physical_link_generation ||
        binding.reservation_id == Id128{} ||
        binding.profile != link.profile ||
        binding.wire_job_id == 0 || binding.assignment_epoch == 0 ||
        binding.assignment_nonce == 0 || binding.logical_job == 0 ||
        binding.compiler_attempt == 0 || binding.source_request_id == 0)
        throw std::invalid_argument("R2 JOB_BIND is outside its live link");
    const PreparedInputPtr input = impl_->preparation->resolve(prepared);
    if (input->begin.tu_seq != binding.tu_seq ||
        input->begin.profile != binding.profile ||
        input->begin.raw_bytes != binding.raw_bytes ||
        input->begin.raw_digest != binding.raw_digest ||
        binding.raw_bytes > link_state.selected_max_raw_bytes ||
        binding.raw_bytes > link_state.selected_max_output_bytes ||
        input->begin.body.encoded_bytes > link_state.selected_max_encoded_bytes ||
        input->begin.body.decoded_bytes > link_state.selected_max_output_bytes)
        throw std::invalid_argument("R2 binding differs from retained TU");
    const auto executor = co_await asio::this_coro::executor;
    SocketDeadlineGuard deadline_guard(executor, socket, deadline);
    if (binding.profile == ProfileId::P29V1 &&
        impl_->preparation->p29v1_system_source_fingerprint(prepared) !=
            link.system_source_fingerprint)
        throw std::invalid_argument("R2 LINK_HELLO has wrong C P29 fingerprint");
    if (binding.profile == ProfileId::P29V1)
        impl_->preparation->pin_p29v1_system_source_reuse(
            prepared, impl_->r2_link_state->f_system_source_fingerprint);

    TxBegin begin = impl_->preparation->r2_staged_begin(
        prepared, link_state.history_nonce,
        RelSeq{impl_->r2_speculative_rel}, impl_->r2_speculative_state);
    if (begin.tu_seq != input->begin.tu_seq ||
        begin.profile != input->begin.profile ||
        begin.raw_bytes != input->begin.raw_bytes ||
        begin.raw_digest != input->begin.raw_digest ||
        begin.body != input->begin.body)
        throw std::invalid_argument("R2 staged begin differs from prepared envelope");
    impl_->record(ActionType::TX_BEGIN, begin, impl_->r2_session_serial,
                  begin.pre_state_digest);
    const TuBegin tu_begin{binding.relationship_ordinal, begin};
    const uint32_t frame_cap = link_state.selected_max_frame_payload;
    std::vector<R2FillMessage> fills;
    if (binding.profile == ProfileId::P29V1) {
        codec::P29WireLimits wire_limits;
        wire_limits.max_tu_bytes = static_cast<size_t>(impl_->caps.zstd.max_raw_bytes);
        wire_limits.max_region_bytes = wire_limits.max_tu_bytes;
        const std::vector<uint8_t> predicted =
            impl_->preparation->predicted_p29v1_need(prepared);
        const std::span<const uint8_t> fill =
            impl_->preparation->answer_p29v1_need(prepared, predicted);
        const std::vector<FillMessage> encoded = encode_p29v1_fill_messages(
            fill, frame_cap, codec::p29v1_fill_inner_bound(wire_limits));
        fills.reserve(encoded.size());
        for (const FillMessage& value : encoded)
            fills.push_back(R2FillMessage{value.bytes});
    }
    const Digest128 binding_digest = compute_r2_binding_digest(binding);
    icecc::Digest128Builder outer_digest;
    outer_digest.append("R2-transaction-v1");
    outer_digest.append_digest(binding_digest);
    const auto append_outer_frame = [&outer_digest](MessageType type,
                                                    std::span<const uint8_t> bytes) {
        outer_digest.append_u8(static_cast<uint8_t>(type));
        outer_digest.append_u64(static_cast<uint64_t>(bytes.size()));
        outer_digest.append(bytes);
    };
    const std::vector<uint8_t> tu_begin_payload =
        encode_payload(Message{tu_begin});
    append_outer_frame(MessageType::TU_BEGIN, tu_begin_payload);
    for (size_t offset = 0; offset < input->body.size();) {
        const size_t count = std::min<size_t>(
            frame_cap, input->body.size() - offset);
        append_outer_frame(
            MessageType::R2_BODY,
            std::span<const uint8_t>(input->body).subspan(offset, count));
        offset += count;
    }
    if (input->body.empty())
        append_outer_frame(MessageType::R2_BODY, std::span<const uint8_t>{});
    for (const R2FillMessage& fill : fills)
        append_outer_frame(MessageType::R2_FILL,
                           std::span<const uint8_t>(fill.bytes));
    const Digest128 transaction_digest = outer_digest.finish();
    uint64_t aggregate_encoded_bytes = 0;
    const auto account_component = [&aggregate_encoded_bytes,
                                    &link_state](uint64_t bytes) {
        if (bytes > link_state.selected_max_encoded_bytes -
                        std::min<uint64_t>(aggregate_encoded_bytes,
                                           link_state.selected_max_encoded_bytes))
            throw std::length_error("R2 encoded transaction exceeds link budget");
        aggregate_encoded_bytes += bytes;
    };
    account_component(input->body.size());
    for (const R2FillMessage& fill : fills)
        account_component(fill.bytes.size());
    const TuBegin wire_tu_begin = tu_begin;
    const TuEnd end{binding.relationship_ordinal, binding_digest,
                    transaction_digest};
    R2SentBundle pending{binding, tu_begin, binding_digest,
                         transaction_digest, prepared};
    const auto [pending_position, pending_inserted] =
        impl_->r2_pending_bundles.emplace(binding.relationship_ordinal,
                                          pending);
    if (!pending_inserted)
        throw std::logic_error("R2 ordinal already owns a pending receipt row");
    struct PendingRowGuard {
        Impl& impl;
        uint64_t ordinal;
        bool keep = false;
        ~PendingRowGuard() {
            if (!keep)
                impl.r2_pending_bundles.erase(ordinal);
        }
    } pending_guard{*impl_, binding.relationship_ordinal};
    // From this point even a failure of the first async write is ambiguous:
    // retain the exact witness and include it in P during recovery. F will
    // report whether END reached its commit boundary; otherwise the suffix is
    // reset and rebuilt from the retained raw source.
    impl_->r2_staged_ordinal = binding.relationship_ordinal;
    pending_guard.keep = true;
    CompletionStamp stamp;
    stamp.actor = ActorSide::C;
    stamp.operation = AsyncOperationKind::WriteFragment;
    stamp.c_store_guid = impl_->c_guid;
    stamp.f_store_guid = link.f_store_guid;
    stamp.session_serial = impl_->r2_session_serial;
    stamp.history_nonce = begin.history_nonce;
    stamp.rel_seq = begin.rel_seq;
    stamp.tu_seq = begin.tu_seq;
    stamp.transaction_digest = transaction_digest;
    stamp.raw_digest = begin.raw_digest;
    stamp.transaction_bound = true;
    const auto verify = [this, deadline](const CompletionStamp&) {
        impl_->owner.require();
        if (std::chrono::steady_clock::now() >= deadline)
            throw boost::system::system_error(asio::error::timed_out);
    };
    co_await async_write_message(socket, Message{binding}, frame_cap, stamp,
                                 impl_->completions, control, verify);
    co_await async_write_message(socket, Message{wire_tu_begin}, frame_cap, stamp,
                                 impl_->completions, control, verify);
    if (input->body.empty()) {
        co_await async_write_message(socket, Message{R2BodyMessage{}}, frame_cap,
                                     stamp, impl_->completions, control, verify);
    } else {
        for (size_t offset = 0; offset < input->body.size();) {
            const size_t count = std::min<size_t>(
                frame_cap, input->body.size() - offset);
            R2BodyMessage body;
            body.bytes.assign(input->body.begin() +
                                  static_cast<std::ptrdiff_t>(offset),
                              input->body.begin() +
                                  static_cast<std::ptrdiff_t>(offset + count));
            co_await async_write_message(socket, Message{std::move(body)},
                                         frame_cap, stamp, impl_->completions,
                                         control, verify);
            offset += count;
        }
    }
    for (const R2FillMessage& fill : fills)
        co_await async_write_message(socket, Message{fill}, frame_cap, stamp,
                                     impl_->completions, control, verify);
    co_await async_write_message(socket, Message{end}, frame_cap, stamp,
                                 impl_->completions, control, verify);
    impl_->preparation->advance_speculative(prepared);
    impl_->r2_speculative_state = compute_post_state_digest(
        begin.pre_state_digest, begin.history_nonce, begin.rel_seq,
        begin.tu_seq, begin.transaction_digest);
    if (impl_->r2_speculative_rel == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("R2 speculative REL_SEQ space exhausted");
    ++impl_->r2_speculative_rel;
    impl_->r2_sent_ordinal = binding.relationship_ordinal;
    co_return pending_position->second;
}

boost::asio::awaitable<ClientRunResult> P50ClientEndpoint::read_r2_receipt(
    tcp::socket& socket, const R2SentBundle& sent,
    std::chrono::steady_clock::time_point deadline) {
    impl_->owner.require();
    ClientRunResult result;
    if (!impl_->r2_link_hello || !impl_->r2_link_state || !socket.is_open() ||
        sent.binding.relationship_ordinal !=
                              impl_->r2_confirmed_ordinal + 1 ||
        sent.binding.relationship_ordinal > impl_->r2_sent_ordinal ||
        !impl_->r2_pending_bundles.contains(
            sent.binding.relationship_ordinal) ||
        deadline <= std::chrono::steady_clock::now())
        throw std::logic_error("R2 receipt has no matching sent transaction");
    const R2SentBundle& pending = impl_->r2_pending_bundles.at(
        sent.binding.relationship_ordinal);
    if (pending.binding != sent.binding ||
        pending.binding_digest != sent.binding_digest ||
        pending.transaction_digest != sent.transaction_digest ||
        pending.prepared != sent.prepared)
        throw std::logic_error("R2 receipt does not match the reserved ledger row");
    const LinkHello& link = *impl_->r2_link_hello;
    const uint32_t frame_cap = impl_->r2_link_state->selected_max_frame_payload;
    const auto executor = co_await asio::this_coro::executor;
    SocketDeadlineGuard deadline_guard(executor, socket, deadline);
    EndpointIoControl control;
    CompletionStamp stamp;
    stamp.actor = ActorSide::C;
    stamp.operation = AsyncOperationKind::ReadHeader;
    stamp.c_store_guid = impl_->c_guid;
    stamp.f_store_guid = link.f_store_guid;
    stamp.session_serial = impl_->r2_session_serial;
    stamp.history_nonce = sent.begin.inner.history_nonce;
    stamp.rel_seq = sent.begin.inner.rel_seq;
    stamp.tu_seq = sent.begin.inner.tu_seq;
    stamp.transaction_digest = sent.transaction_digest;
    stamp.raw_digest = sent.begin.inner.raw_digest;
    stamp.transaction_bound = true;
    const auto verify = [this, deadline](const CompletionStamp&) {
        impl_->owner.require();
        if (std::chrono::steady_clock::now() >= deadline)
            throw boost::system::system_error(asio::error::timed_out);
    };
    Frame frame = co_await async_read_frame(socket, frame_cap, stamp,
                                            impl_->completions, verify);
    if (frame.type != MessageType::R2_TX_COMMIT)
        throw std::invalid_argument("R2 bundle did not receive TX_COMMIT");
    const R2TxCommit receipt = decode_as<R2TxCommit>(frame);
    if (receipt.relationship_ordinal != sent.binding.relationship_ordinal ||
        receipt.binding_digest != sent.binding_digest ||
        receipt.transaction_digest != sent.transaction_digest ||
        !same_commit(receipt.inner, sent.begin.inner))
        throw std::invalid_argument("R2 TX_COMMIT differs from exact sent witness");
    if (impl_->next_rel.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("R2 C REL_SEQ space exhausted");
    impl_->record(ActionType::COMMIT_ACCEPTED, sent.begin.inner,
                  impl_->r2_session_serial, receipt.inner.post_state_digest);
    impl_->preparation->accept_commit(sent.prepared, sent.begin.inner,
                                      receipt.inner);
    impl_->state = receipt.inner.post_state_digest;
    ++impl_->next_rel.value;
    impl_->r2_confirmed_ordinal = sent.binding.relationship_ordinal;
    impl_->r2_link_state->committed_prefix_k = impl_->r2_confirmed_ordinal;
    impl_->r2_link_state->next_rel_seq = impl_->next_rel;
    impl_->r2_link_state->state_digest = impl_->state;
    result.status = ClientRunStatus::Committed;
    result.observation = ClientRunObservation::ExactCommitObserved;
    result.committed_commit = receipt.inner;
    result.committed_input = InputRecordKey{impl_->c_guid, receipt.inner.tu_seq};
    impl_->r2_pending_bundles.erase(sent.binding.relationship_ordinal);
    co_return result;
}

boost::asio::awaitable<void> P50ClientEndpoint::write_r2_ack(
    tcp::socket& socket, uint64_t cumulative_ordinal,
    std::chrono::steady_clock::time_point deadline) {
    impl_->owner.require();
    if (!impl_->r2_link_hello || !impl_->r2_link_state || !socket.is_open() ||
        cumulative_ordinal == 0 ||
        cumulative_ordinal > impl_->r2_confirmed_ordinal ||
        cumulative_ordinal < impl_->r2_ack_sent_ordinal ||
        deadline <= std::chrono::steady_clock::now())
        throw std::logic_error("R2 cumulative ACK is outside the confirmed prefix");
    if (cumulative_ordinal == impl_->r2_ack_sent_ordinal)
        co_return;
    const LinkHello& link = *impl_->r2_link_hello;
    const uint32_t frame_cap = impl_->r2_link_state->selected_max_frame_payload;
    const auto executor = co_await asio::this_coro::executor;
    SocketDeadlineGuard deadline_guard(executor, socket, deadline);
    EndpointIoControl control;
    CompletionStamp stamp;
    stamp.actor = ActorSide::C;
    stamp.operation = AsyncOperationKind::WriteFragment;
    stamp.c_store_guid = impl_->c_guid;
    stamp.f_store_guid = link.f_store_guid;
    stamp.session_serial = impl_->r2_session_serial;
    stamp.history_nonce = impl_->history_nonce;
    stamp.rel_seq = impl_->next_rel;
    const auto verify = [this, deadline](const CompletionStamp&) {
        impl_->owner.require();
        if (std::chrono::steady_clock::now() >= deadline)
            throw boost::system::system_error(asio::error::timed_out);
    };
    const CommitAck ack{link.relationship_id, link.relationship_epoch,
                        link.physical_link_generation, cumulative_ordinal};
    co_await async_write_message(socket, Message{ack}, frame_cap, stamp,
                                 impl_->completions, control, verify);
    impl_->r2_ack_sent_ordinal = cumulative_ordinal;
}

boost::asio::awaitable<void> P50ClientEndpoint::flush_r2_ack(
    tcp::socket& socket, std::chrono::steady_clock::time_point deadline) {
    impl_->owner.require();
    if (!impl_->r2_link_state)
        throw std::logic_error("R2 ACK flush has no negotiated link");
    if (impl_->r2_ack_sent_ordinal < impl_->r2_confirmed_ordinal)
        co_await write_r2_ack(socket, impl_->r2_confirmed_ordinal, deadline);
}

bool P50ClientEndpoint::r2_window_available() const noexcept {
    if (!impl_->r2_link_hello ||
        impl_->r2_sent_ordinal < impl_->r2_ack_sent_ordinal)
        return false;
    return impl_->r2_sent_ordinal - impl_->r2_ack_sent_ordinal <
           impl_->r2_link_hello->window;
}

uint64_t P50ClientEndpoint::r2_confirmed_prefix() const noexcept {
    return impl_->r2_confirmed_ordinal;
}

std::vector<R2SentBundle> P50ClientEndpoint::r2_pending_witnesses(
    uint64_t after_ordinal) const {
    impl_->owner.require();
    if (after_ordinal > impl_->r2_staged_ordinal)
        throw std::invalid_argument("R2 witness floor exceeds staged prefix");
    std::vector<R2SentBundle> result;
    result.reserve(static_cast<size_t>(impl_->r2_staged_ordinal - after_ordinal));
    for (uint64_t ordinal = after_ordinal + 1;
         ordinal <= impl_->r2_staged_ordinal; ++ordinal) {
        const auto position = impl_->r2_pending_bundles.find(ordinal);
        if (position == impl_->r2_pending_bundles.end())
            throw std::logic_error("R2 staged prefix has a missing witness row");
        result.push_back(position->second);
    }
    return result;
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
#pragma GCC diagnostic pop
#endif

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
    if (admitted && admitted->begin.profile != impl_->caps.profile)
        throw std::invalid_argument(
            "prepared transaction profile differs from endpoint capabilities");
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
        if (impl_->caps.profile == ProfileId::P29V1) {
            const PreparedTuHandle fingerprint_handle =
                impl_->active ? impl_->active->handle : impl_->queued_handle;
            hello.system_source_fingerprint =
                impl_->preparation->p29v1_system_source_fingerprint(
                    fingerprint_handle);
        }
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
        Digest128 negotiated_f_fingerprint =
            peer.system_source_fingerprint;

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
                if (received.wire_revision != peer.wire_revision ||
                    received.negotiated_profiles != peer.negotiated_profiles ||
                    received.limits != peer.limits || !received.namespace_present ||
                    !received.route_present || received.f_store_guid != peer.f_store_guid ||
                    received.system_source_fingerprint !=
                        peer.system_source_fingerprint ||
                    received.history_nonce != replacement_nonce ||
                    received.next_rel_seq != replacement_rel ||
                    received.state_digest != replacement_state || received.last_commit)
                    throw std::invalid_argument(
                        "HISTORY_RESET acknowledgement differs from the new route");
                return received;
            });
            negotiated_f_fingerprint =
                reset_ack.system_source_fingerprint;
            PreparedInputPtr reset_retry = retry;
            if (impl_->caps.profile == ProfileId::P29V1 &&
                established_relationship)
                reset_retry = impl_->preparation->reset_p29v1_route(
                    impl_->queued_handle, peer.f_store_guid,
                    replacement_nonce);
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
            impl_->queued = std::move(reset_retry);
            impl_->f_guid = peer.f_store_guid;
            impl_->history_nonce = replacement_nonce;
            impl_->next_rel = replacement_rel;
            impl_->state = replacement_state;
            impl_->route_known = true;
            session.provisional_nonce.reset();
        }

        if (exact && impl_->active &&
            impl_->active->begin.profile == ProfileId::P29V1 &&
            impl_->active->p29v1_transport_retry) {
            impl_->preparation->restart_p29v1_transport_retry(
                impl_->active->handle);
            impl_->active->p29v1_transport_retry = false;
        }

        if (impl_->caps.profile == ProfileId::P29V1) {
            const PreparedTuHandle fingerprint_handle =
                impl_->active ? impl_->active->handle : impl_->queued_handle;
            impl_->preparation->pin_p29v1_system_source_reuse(
                fingerprint_handle, negotiated_f_fingerprint);
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
            begin = control.outbound_begin_transform(
                begin, impl_->active->prepared->body);
        require_outbound_profile_negotiated(peer.negotiated_profiles, begin);
        const uint32_t frame_cap = peer.limits.max_frame_payload;
        co_await async_write_message(socket, begin, frame_cap,
                                     impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                     impl_->completions, control, verify);

        if (begin.profile == ProfileId::P29V1) {
            co_await async_write_component<BodyMessage>(
                socket, impl_->active->prepared->body, frame_cap,
                impl_->stamp(session, AsyncOperationKind::WriteFragment),
                impl_->completions, control, verify);
            codec::P29WireLimits wire_limits;
            wire_limits.max_tu_bytes = static_cast<size_t>(
                impl_->caps.zstd.max_raw_bytes);
            wire_limits.max_region_bytes = wire_limits.max_tu_bytes;
            P29V1NeedStreamDecoder need_decoder(
                codec::p29v1_need_inner_bound(wire_limits));
            for (;;) {
                Frame need_frame = co_await async_read_frame(
                    socket, frame_cap,
                    impl_->stamp(session, AsyncOperationKind::ReadHeader),
                    impl_->completions, verify);
                if (need_frame.type == MessageType::ERROR)
                    throw ClientTerminalResult(
                        decode_as<ErrorMessage>(need_frame), frame_cap);
                if (need_frame.type != MessageType::NEED)
                    throw std::invalid_argument(
                        "P29V1 expected NEED before FILL");
                need_decoder.push(decode_as<NeedMessage>(need_frame));
                if (need_decoder.complete())
                    break;
            }
            const std::span<const uint8_t> inner_fill =
                impl_->preparation->answer_p29v1_need(
                    impl_->active->handle, need_decoder.inner_frames());
            const std::vector<FillMessage> fills =
                encode_p29v1_fill_messages(
                    inner_fill, frame_cap,
                    codec::p29v1_fill_inner_bound(wire_limits));
            for (const FillMessage& fill : fills)
                co_await async_write_message(
                    socket, fill, frame_cap,
                    impl_->stamp(session, AsyncOperationKind::WriteFragment),
                    impl_->completions, control, verify);
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
    if (!io->committed && impl_->active &&
        impl_->active->begin.profile == ProfileId::P29V1 &&
        (result.status == ClientRunStatus::Disconnected ||
         result.status == ClientRunStatus::DeadlineExceeded))
        impl_->active->p29v1_transport_retry = true;
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
#if !defined(ICECC_P29V1_MUTANT_NO_NODELAY)
        socket.set_option(tcp::no_delay(true), error);
        if (error) {
            close_now(socket);
            return std::nullopt;
        }
#endif
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

boost::asio::awaitable<ServerRunResult> P50ServerEndpoint::run_adopted_r2(
    tcp::socket socket, EndpointIoControl control) {
    impl_->owner.require();
    if (!validate_adopted_socket(socket)) {
        close_now(socket);
        co_return ServerRunResult{};
    }
    std::optional<daemon::P50FSessionOperationId> operation;
    std::optional<sidecar::AbsoluteMonotonicDeadline> link_deadline;
    if (impl_->config.sidecar_launch) {
        if (impl_->next_run_sequence == 0 ||
            impl_->next_run_sequence == UINT64_MAX)
            throw std::overflow_error("R2 link operation sequence exhausted");
        daemon::P50FSessionOperationId created;
        created.sidecar_launch = {
            impl_->config.sidecar_launch->identity.generation,
            impl_->config.sidecar_launch->identity.attempt};
        created.role = daemon::P50SessionOperationRole::FSession;
        created.operation_sequence = impl_->next_run_sequence++;
        operation = created;
        const auto clock = sidecar::process_monotonic_clock_identity();
        link_deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::hours(24),
            clock.clock_domain_id, clock.time_namespace_id);
    }
    const Impl::Session session = impl_->allocate_session(operation, link_deadline);
    SessionRegistration registration(*impl_, session);
    co_return co_await run_r2_connected(std::move(socket),
                                        std::move(registration),
                                        std::move(control));
}

bool P50ServerEndpoint::retire_p51_recovery_install(
    const JobBind& binding) {
    impl_->owner.require();
    for (auto& [c_guid, space] : impl_->namespaces) {
        for (auto& [profile, route] : space.routes) {
            (void)profile;
            if (!route.recovery_install ||
                !Impl::same_recovery_binding(
                    route.recovery_install->binding, binding))
                continue;
            impl_->discard_recovery_install(c_guid, route);
            return true;
        }
    }
    return false;
}

bool P50ServerEndpoint::retire_p51_recovery_install(
    Id128 reservation_id) {
    impl_->owner.require();
    if (reservation_id == Id128{})
        return false;
    for (auto& [c_guid, space] : impl_->namespaces) {
        for (auto& [profile, route] : space.routes) {
            (void)profile;
            if (!route.recovery_install ||
                route.recovery_install->binding.reservation_id !=
                    reservation_id)
                continue;
            impl_->discard_recovery_install(c_guid, route);
            return true;
        }
    }
    return false;
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
            negotiate_session(hello, kP50WireRevision,
                              impl_->caps.supported_profiles, impl_->caps.wire);
        SessionState state = impl_->stage(session, hello, selection);
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
            // owns BODY/NEED/FILL semantics. ZSTD_TU deliberately rejects
            // the interactive messages through its adapter; future profiles
            // can implement the same bidirectional dialogue without a new
            // concrete-profile branch here.
            switch (component.type) {
            case MessageType::BODY:
                impl_->append_body(session, decode_as<BodyMessage>(component));
                {
                    auto& pending = *impl_->require_route(session).pending;
                    const std::vector<NeedMessage> needs =
                        pending.dialogue->need_messages(
                            selection.limits.max_frame_payload);
                    if (!needs.empty()) {
                        impl_->record(ActionType::BODY_COMPLETE, session,
                                      &pending.begin);
                        impl_->record(ActionType::NEED_RECORDED, session,
                                      &pending.begin);
                    }
                    for (const NeedMessage& need : needs)
                    co_await async_write_message(
                        socket, need, selection.limits.max_frame_payload,
                        impl_->stamp(session, AsyncOperationKind::WriteFragment),
                        impl_->completions, control, verify);
                }
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
        result.f_apply_materialize_ns =
            materialized.f_apply_materialize_ns;
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
    } catch (const ProtocolError& error) {
        terminal_error = bounded_error(static_cast<uint16_t>(error.code()),
                                       error.what(), reply_cap);
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

boost::asio::awaitable<ServerRunResult> P50ServerEndpoint::run_r2_connected(
    tcp::socket socket_value, SessionRegistration registration,
    EndpointIoControl control) {
    impl_->owner.require();
    Impl::Session session = registration.session();
    const std::optional<sidecar::AbsoluteMonotonicDeadline> link_deadline =
        session.deadline;
    ServerRunResult result;
    result.session_serial = session.serial;
    auto io = std::make_shared<ServerIoState>(
        std::move(socket_value), session.operation, std::nullopt);
    tcp::socket& socket = io->socket;
    if (impl_->pending_r2_setup.size() >=
        impl_->config.owner_limits.max_live_sessions)
        throw std::length_error("R2 setup socket bound is exhausted");
    const auto [setup_position, setup_inserted] =
        impl_->pending_r2_setup.emplace(session.serial, io);
    if (!setup_inserted)
        throw std::logic_error("duplicate R2 setup socket session serial");
    struct PendingSetupGuard {
        Impl& owner;
        uint64_t session_serial;
        bool active = true;
        void remove() noexcept {
            if (active) {
                owner.pending_r2_setup.erase(session_serial);
                active = false;
            }
        }
        ~PendingSetupGuard() { remove(); }
    } setup_guard{*impl_, session.serial};
    (void)setup_position;
    if (control.after_r2_setup_registered)
        control.after_r2_setup_registered();
    std::optional<LinkHello> active_link;
    uint32_t frame_cap = std::min(impl_->caps.wire.max_frame_payload,
                                  kInitialMaxFramePayload);
    std::optional<sidecar::AbsoluteMonotonicDeadline> job_deadline;
    bool activated = false;
    bool pending_job = false;
    uint64_t committed_ordinal = 0;
    uint64_t acknowledged_ordinal = 0;
    struct EndpointRunLeaseGuard {
        EndpointRunRegistry* registry = nullptr;
        std::optional<EndpointRunIdentity> identity;
        std::optional<EndpointCancelPermit> permit;
        std::function<void(EndpointCancelPermit, EndpointTerminalResult)> terminal;
        EndpointTerminalResult result{EndpointTerminalResultState::Failed, 0};
        ~EndpointRunLeaseGuard() {
            if (!registry || !identity || !permit)
                return;
            if (registry->mark_terminal(*identity, result)) {
                if (terminal) {
                    try {
                        terminal(*permit, result);
                    } catch (...) {
                    }
                }
                (void)registry->consume_terminal(*identity);
            }
        }
    } run_lease{&impl_->endpoint_runs, std::nullopt, std::nullopt,
                impl_->config.on_run_terminal,
                EndpointTerminalResult{EndpointTerminalResultState::Failed, 0}};
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

    const auto deadline_crossed = [&]() {
        if (!job_deadline)
            return false;
        sidecar::SystemMonotonicObservationSource observations;
        const auto observed = observations.observe();
        return !observed || !observed->valid() ||
               !job_deadline->matches_clock(observed->clock) ||
               observed->now_ns >= job_deadline->expires_at_ns;
    };
    const auto require_operation = [&]() {
        impl_->owner.require();
        if (io->cancelled)
            throw boost::system::system_error(asio::error::operation_aborted);
        if (deadline_crossed())
            throw boost::system::system_error(asio::error::timed_out);
    };
    const auto verify = [&](const CompletionStamp& expected) {
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
        require_operation();
    };
    const auto stamp = [&](AsyncOperationKind operation,
                           const TxBegin* begin = nullptr) {
        return impl_->stamp(session, operation, begin);
    };
    const auto set_completion_deadline = [&session, this](
        std::optional<sidecar::AbsoluteMonotonicDeadline> deadline) {
        session.deadline = deadline;
        const auto live = impl_->live_sessions.find(session.serial);
        if (live == impl_->live_sessions.end())
            throw StaleCompletion();
        live->second.deadline = deadline;
    };
    const auto arm_job_deadline = [&](
        const sidecar::AbsoluteMonotonicDeadline& deadline) {
        if (io->deadline_timer.is_open()) {
            boost::system::error_code ignored;
            io->deadline_timer.cancel(ignored);
            io->deadline_timer.close(ignored);
        }
        io->expired = false;
        if (io->deadline_generation == UINT64_MAX)
            throw std::overflow_error("R2 link deadline generation exhausted");
        const uint64_t timer_generation = ++io->deadline_generation;
        job_deadline = deadline;
        require_operation();
        boost::system::error_code error;
        if (!arm_absolute_deadline_timer(*io, deadline, error))
            throw boost::system::system_error(error);
        io->deadline_timer.async_wait(
            asio::posix::stream_descriptor::wait_read,
            [io, timer_generation](const boost::system::error_code& wait_error) {
                if (wait_error || io->deadline_generation != timer_generation)
                    return;
                uint64_t expirations = 0;
                const ssize_t drained = ::read(
                    io->deadline_timer.native_handle(), &expirations,
                    sizeof(expirations));
                (void)drained;
                io->expired = true;
                if (auto materialization = io->materialization.lock())
                    cancel_materialization_notification(materialization);
                close_now(io->socket);
            });
    };
    const auto deadline_after = [](std::chrono::seconds duration) {
        const auto identity = sidecar::process_monotonic_clock_identity();
        return sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + duration,
            identity.clock_domain_id, identity.time_namespace_id);
    };
    const auto disarm_job_deadline = [&]() {
        if (io->deadline_generation != UINT64_MAX)
            ++io->deadline_generation;
        if (io->deadline_timer.is_open()) {
            boost::system::error_code ignored;
            io->deadline_timer.cancel(ignored);
            io->deadline_timer.close(ignored);
        }
        job_deadline.reset();
    };
    const auto append_outer_frame = [](icecc::Digest128Builder& digest,
                                       const Frame& frame) {
        digest.append_u8(static_cast<uint8_t>(frame.type));
        digest.append_u64(static_cast<uint64_t>(frame.payload.size()));
        digest.append(frame.payload);
    };
    const auto clear_link = [&]() {
        disarm_job_deadline();
        if (active_link && impl_->config.on_p51_link_terminal) {
            try {
                impl_->config.on_p51_link_terminal(*active_link);
            } catch (...) {
                // Endpoint teardown must still close the socket and disconnect
                // the exact F session if a diagnostic callback throws.
            }
        }
        if (activated)
            impl_->disconnect(session, pending_job);
    };

    try {
        arm_job_deadline(deadline_after(std::chrono::seconds(30)));
        require_operation();
        Frame hello_frame = co_await async_read_frame(
            socket, frame_cap, stamp(AsyncOperationKind::ReadHeader),
            impl_->completions, verify);
        if (hello_frame.type != MessageType::LINK_HELLO)
            throw std::invalid_argument("R2 link did not begin with LINK_HELLO");
        LinkHello hello = decode_as<LinkHello>(hello_frame);
        if ((hello.start_mode == LinkStartMode::Initial &&
             hello.verified_receipt_floor != 0) ||
            (hello.start_mode != LinkStartMode::Initial &&
             hello.start_mode != LinkStartMode::Reconnect) ||
            hello.window == 0 || hello.window > 30 ||
            hello.f_store_guid != impl_->f_guid ||
            hello.max_frame_payload < kR2MandatoryControlFramePayload ||
            (impl_->caps.supported_profiles & profile_bit(hello.profile)) == 0)
            throw std::invalid_argument("R2 LINK_HELLO exceeds F admission");
        if (!impl_->config.lookup_p51_link_reservation)
            throw std::invalid_argument("R2 link reservation lookup is unavailable");
        const auto link_lease =
            impl_->config.lookup_p51_link_reservation(hello);
        if (!link_lease || !link_lease->initial_armed.valid() ||
            link_lease->reconnect !=
                (hello.start_mode == LinkStartMode::Reconnect) ||
            link_lease->initial_armed.reservation_id != hello.reservation_id.bytes ||
            link_lease->initial_armed.logical_relationship_id !=
                hello.relationship_id.bytes ||
            link_lease->relationship_epoch == 0 ||
            (!link_lease->reconnect &&
             link_lease->relationship_epoch != hello.relationship_epoch) ||
            !link_lease->absolute_deadline.valid())
            throw std::invalid_argument("R2 LINK_HELLO lacks its exact F reservation");
        active_link = hello;
        if (impl_->config.sidecar_launch && session.operation &&
            impl_->config.endpoint_generation != 0 &&
            impl_->next_socket_ownership_generation != 0) {
            EndpointRunIdentity identity;
            identity.sidecar_launch = *impl_->config.sidecar_launch;
            identity.c_store_guid = hello.c_store_guid;
            identity.f_store_guid = impl_->f_guid;
            identity.f_session_operation = *session.operation;
            identity.endpoint_generation = impl_->config.endpoint_generation;
            identity.endpoint_session_serial = session.serial;
            identity.run_sequence = session.operation->operation_sequence;
            identity.socket_ownership_generation =
                impl_->next_socket_ownership_generation++;
            const auto identity_clock = sidecar::process_monotonic_clock_identity();
            const auto registry_deadline =
                sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
                    std::chrono::steady_clock::now() + std::chrono::hours(24),
                    identity_clock.clock_domain_id,
                    identity_clock.time_namespace_id);
            auto handle = impl_->endpoint_runs.admit(
                identity, link_deadline.value_or(registry_deadline),
                std::make_shared<ServerRunSocketTarget>(io));
            if (!handle)
                throw std::length_error("R2 endpoint run admission failed");
            run_lease.identity = identity;
            run_lease.permit =
                handle->permit(EndpointCancelReason::CallerRequested);
            (void)impl_->endpoint_runs.set_phase(identity,
                                                 EndpointRunPhase::CacheWire);
            if (impl_->config.on_run_admitted)
                impl_->config.on_run_admitted(*run_lease.permit);
        }
        setup_guard.remove();

        frame_cap = std::min({frame_cap, hello.max_frame_payload,
                              impl_->caps.wire.max_frame_payload});
        const uint64_t raw_cap = std::min<uint64_t>(
            hello.max_raw_bytes, impl_->caps.zstd.max_raw_bytes);
        const uint64_t encoded_cap = std::min<uint64_t>(
            hello.max_encoded_bytes, impl_->caps.zstd.max_encoded_body_bytes);
        const uint64_t output_cap = std::min<uint64_t>(
            hello.max_output_bytes, impl_->caps.zstd.max_raw_bytes);
        if (frame_cap < kR2MandatoryControlFramePayload || raw_cap == 0 ||
            encoded_cap == 0 || output_cap == 0)
            throw std::invalid_argument("R2 LINK_HELLO has no usable budget");

        SessionHello ordinary_hello;
        ordinary_hello.wire_revision = kP50WireRevision;
        ordinary_hello.c_store_guid = hello.c_store_guid;
        ordinary_hello.system_source_fingerprint =
            hello.system_source_fingerprint;
        ordinary_hello.supported_profiles = profile_bit(hello.profile);
        ordinary_hello.limits.max_frame_payload = frame_cap;
        ordinary_hello.limits.max_fill_record_bytes = encoded_cap;
        const SessionSelection selection = negotiate_session(
            ordinary_hello, kP50WireRevision,
            impl_->caps.supported_profiles,
            SessionLimits{frame_cap, encoded_cap});
        const SessionState staged = impl_->stage(session, ordinary_hello, selection);
        if (!link_lease->reconnect && staged.route_present)
            throw std::invalid_argument(
                "R2 initial link requires fresh history; recovery is not enabled");
        if (link_lease->reconnect && !staged.namespace_present)
            throw std::invalid_argument(
                "R2 recovery requires the retained F namespace");
        impl_->activate(session);
        activated = true;
        if (link_lease->reconnect) {
            if (!impl_->config.settle_p51_interrupted_job)
                throw std::logic_error(
                    "F cannot determine whether the interrupted R2 job is live");
            const bool interrupted_job_live =
                impl_->config.settle_p51_interrupted_job(hello);
            const auto active_namespace =
                impl_->namespaces.find(hello.c_store_guid);
            const auto* active_route =
                active_namespace == impl_->namespaces.end()
                    ? nullptr
                    : impl_->find_route(active_namespace->second, hello.profile);
            if (!interrupted_job_live) {
                if (active_namespace != impl_->namespaces.end() && active_route &&
                    active_route->recovery_install &&
                    impl_->config.p51_source_reservation_terminal &&
                    impl_->config.p51_source_reservation_terminal(
                        active_route->recovery_install->binding))
                    impl_->discard_recovery_install(
                        hello.c_store_guid,
                        *impl_->find_route(active_namespace->second,
                                           hello.profile));
                throw std::logic_error(
                    "F interrupted source reservation is terminal or expired");
            }
            if (active_route == nullptr || active_route->pending)
                throw std::logic_error(
                    "F retained pending decode work after link replacement");
        }
        if (!link_lease->reconnect) {
            const HistoryReset initial{
                hello.history_nonce,
                initial_route_digest(hello.c_store_guid, hello.history_nonce)};
            impl_->reset_history(session, initial);
        }
        SessionState route_state = impl_->session_state(session, selection);
        if (link_lease->reconnect) {
            const auto name_space = impl_->namespaces.find(hello.c_store_guid);
            const auto* retained_route = name_space == impl_->namespaces.end()
                ? nullptr : impl_->find_route(name_space->second, hello.profile);
            if (retained_route == nullptr)
                throw std::invalid_argument(
                    "R2 recovery has no retained codec route");
            route_state.route_present = true;
            route_state.history_nonce = retained_route->nonce;
            route_state.next_rel_seq = retained_route->next_rel;
            route_state.state_digest = retained_route->state;
            route_state.last_commit = retained_route->last_commit;
        }
        LinkState state;
        state.profile = hello.profile;
        state.window = link_lease->initial_armed.selected_window;
        state.reservation_id = hello.reservation_id;
        state.relationship_id = hello.relationship_id;
        state.relationship_epoch = link_lease->relationship_epoch;
        state.physical_link_generation = hello.physical_link_generation;
        state.c_store_guid = hello.c_store_guid;
        state.c_store_generation = hello.c_store_generation;
        state.f_store_guid = impl_->f_guid;
        state.f_store_generation =
            link_lease->initial_armed.f_store_generation;
        state.c_control_generation = hello.c_control_generation;
        state.c_control_attempt = hello.c_control_attempt;
        state.selected_max_frame_payload = frame_cap;
        state.selected_max_raw_bytes = raw_cap;
        state.selected_max_encoded_bytes = encoded_cap;
        state.selected_max_output_bytes = output_cap;
        state.f_system_source_fingerprint =
            route_state.system_source_fingerprint;
        state.history_nonce = route_state.history_nonce;
        state.next_rel_seq = route_state.next_rel_seq;
        state.state_digest = route_state.state_digest;
        state.committed_prefix_k = link_lease->committed_prefix_k;
        state.acknowledged_prefix_q = link_lease->acknowledged_prefix_q;
        committed_ordinal = link_lease->committed_prefix_k;
        acknowledged_ordinal = link_lease->acknowledged_prefix_q;
        co_await async_write_message(
            socket, Message{state}, frame_cap,
            stamp(AsyncOperationKind::WriteFragment), impl_->completions,
            control, verify);
        arm_job_deadline(deadline_after(std::chrono::seconds(60)));

        if (link_lease->reconnect) {
            Frame recovery_frame = co_await async_read_frame(
                socket, frame_cap, stamp(AsyncOperationKind::ReadHeader),
                impl_->completions, verify);
            bool reset_applied = false;
            ResetAck reset_ack;
            if (recovery_frame.type == MessageType::RESET_CONFIRM) {
                const ResetConfirm confirm = decode_as<ResetConfirm>(recovery_frame);
                if (confirm.settled_prefix_k !=
                        link_lease->committed_prefix_k ||
                    confirm.settled_prefix_k !=
                        link_lease->acknowledged_prefix_q ||
                    !impl_->config.confirm_p51_reset ||
                    !impl_->config.confirm_p51_reset(hello, confirm))
                    throw std::invalid_argument(
                        "R2 RESET_CONFIRM does not match retained reset result");
                hello.relationship_epoch = confirm.new_relationship_epoch;
                hello.history_nonce = confirm.new_history_nonce;
                hello.verified_receipt_floor = confirm.settled_prefix_k;
                active_link = hello;
                committed_ordinal = link_lease->committed_prefix_k;
                acknowledged_ordinal = link_lease->acknowledged_prefix_q;
            } else {
                ResetRequest reset_request;
                if (recovery_frame.type == MessageType::RECOVER) {
                    const RecoverBegin recover_begin =
                        decode_as<RecoverBegin>(recovery_frame);
                    if (recover_begin.witness_count > hello.window)
                        throw std::length_error(
                            "R2 RECOVER witness interval exceeds link window");
                    std::vector<RecoverWitness> witnesses;
                    witnesses.reserve(recover_begin.witness_count);
                    for (uint32_t index = 0;
                         index != recover_begin.witness_count; ++index) {
                        Frame witness_frame = co_await async_read_frame(
                            socket, frame_cap,
                            stamp(AsyncOperationKind::ReadHeader),
                            impl_->completions, verify);
                        if (witness_frame.type != MessageType::RECOVER)
                            throw std::invalid_argument(
                                "R2 RECOVER witness stream is incomplete");
                        witnesses.push_back(
                            decode_as<RecoverWitness>(witness_frame));
                    }
                    Frame end_frame = co_await async_read_frame(
                        socket, frame_cap, stamp(AsyncOperationKind::ReadHeader),
                        impl_->completions, verify);
                    if (end_frame.type != MessageType::RECOVER)
                        throw std::invalid_argument(
                            "R2 RECOVER stream lacks its end marker");
                    const RecoverEnd recover_end =
                        decode_as<RecoverEnd>(end_frame);
                    if (!impl_->config.recover_p51_receipts)
                        throw std::invalid_argument(
                            "R2 recovery receipt service is unavailable");
                    const auto interval = impl_->config.recover_p51_receipts(
                        hello, recover_begin, witnesses, recover_end);
                    if (!interval || interval->rows.size() !=
                                         interval->end.receipt_count)
                        throw std::invalid_argument(
                            "R2 retained receipt interval does not match witnesses");
                    for (const ReceiptRow& row : interval->rows) {
                        if (row.relationship_id != recover_begin.relationship_id ||
                            row.relationship_epoch != recover_begin.relationship_epoch ||
                            row.physical_link_generation !=
                                recover_begin.physical_link_generation ||
                            row.operation_id != recover_begin.operation_id)
                            throw std::logic_error(
                                "F recovery service returned a foreign receipt row");
                        co_await async_write_message(
                            socket, Message{row}, frame_cap,
                            stamp(AsyncOperationKind::WriteFragment),
                            impl_->completions, control, verify);
                    }
                    co_await async_write_message(
                        socket, Message{interval->end}, frame_cap,
                        stamp(AsyncOperationKind::WriteFragment),
                        impl_->completions, control, verify);
                    Frame reset_frame = co_await async_read_frame(
                        socket, frame_cap, stamp(AsyncOperationKind::ReadHeader),
                        impl_->completions, verify);
                    if (reset_frame.type != MessageType::RESET)
                        throw std::invalid_argument(
                            "R2 recovery did not continue with RESET");
                    reset_request = decode_as<ResetRequest>(reset_frame);
                } else if (recovery_frame.type == MessageType::RESET) {
                    // RESET replay after a lost RESET_ACK is legal across a
                    // physical reconnect; the service deduplicates by its
                    // stable operation identity before old-epoch checks.
                    reset_request = decode_as<ResetRequest>(recovery_frame);
                } else {
                    throw std::invalid_argument(
                        "R2 reconnect expected RECOVER, RESET replay, or RESET_CONFIRM");
                }
                if (!impl_->config.validate_p51_reset)
                    throw std::invalid_argument(
                        "R2 reset validator is unavailable");
                const auto validated =
                    impl_->config.validate_p51_reset(hello, reset_request);
                if (!validated)
                    throw std::invalid_argument(
                        "R2 RESET does not match the reconciled prefix");
                reset_ack = *validated;
                reset_applied = reset_ack.request.new_relationship_epoch ==
                                    link_lease->relationship_epoch &&
                                reset_ack.request.new_history_nonce ==
                                    route_state.history_nonce;
                if (!reset_applied) {
                    if (reset_request.old_relationship_epoch !=
                            link_lease->relationship_epoch ||
                        reset_request.old_history_nonce != route_state.history_nonce)
                        throw std::invalid_argument(
                            "R2 RESET does not continue the active codec history");
                    const auto name_space =
                        impl_->namespaces.find(hello.c_store_guid);
                    if (name_space == impl_->namespaces.end())
                        throw StaleCompletion();
                    auto* retained_route =
                        impl_->find_route(name_space->second, hello.profile);
                    if (retained_route == nullptr || retained_route->pending)
                        throw std::logic_error(
                            "R2 RESET cannot settle pending F decode work");
                    retained_route->interrupted.reset();
                    const HistoryReset reset{
                        reset_request.new_history_nonce,
                        reset_ack.initial_state_digest};
                    impl_->reset_history(session, reset);
                    if (!impl_->config.commit_p51_reset ||
                        !impl_->config.commit_p51_reset(
                            hello, reset_request, reset_ack))
                        throw std::logic_error(
                            "F failed to commit the validated R2 reset outcome");
                }
                co_await async_write_message(
                    socket, Message{reset_ack}, frame_cap,
                    stamp(AsyncOperationKind::WriteFragment),
                    impl_->completions, control, verify);
                Frame confirm_frame = co_await async_read_frame(
                    socket, frame_cap, stamp(AsyncOperationKind::ReadHeader),
                    impl_->completions, verify);
                if (confirm_frame.type != MessageType::RESET_CONFIRM)
                    throw std::invalid_argument(
                        "R2 RESET_ACK was not followed by RESET_CONFIRM");
                const ResetConfirm confirm = decode_as<ResetConfirm>(confirm_frame);
                if (confirm.new_relationship_epoch !=
                        reset_request.new_relationship_epoch ||
                    confirm.new_history_nonce !=
                        reset_request.new_history_nonce ||
                    confirm.settled_prefix_k != reset_request.settled_prefix_k ||
                    confirm.operation_id != reset_request.operation_id ||
                    !impl_->config.confirm_p51_reset ||
                    !impl_->config.confirm_p51_reset(hello, confirm))
                    throw std::invalid_argument(
                        "R2 RESET_CONFIRM does not match RESET_ACK");
                hello.relationship_epoch = confirm.new_relationship_epoch;
                hello.history_nonce = confirm.new_history_nonce;
                hello.verified_receipt_floor = confirm.settled_prefix_k;
                active_link = hello;
                committed_ordinal = confirm.settled_prefix_k;
                acknowledged_ordinal = confirm.settled_prefix_k;
            }
            disarm_job_deadline();
            set_completion_deadline(link_deadline);
            arm_job_deadline(deadline_after(std::chrono::seconds(60)));
        }

        for (;;) {
            Frame bind_frame = co_await async_read_frame(
                socket, frame_cap, stamp(AsyncOperationKind::ReadHeader),
                impl_->completions, verify);
            if (bind_frame.type == MessageType::RESET_CONFIRM) {
                const ResetConfirm confirm = decode_as<ResetConfirm>(bind_frame);
                if (pending_job || !impl_->config.confirm_p51_reset ||
                    !impl_->config.confirm_p51_reset(hello, confirm))
                    throw std::invalid_argument(
                        "R2 duplicate RESET_CONFIRM does not match cached outcome");
                continue;
            }
            if (bind_frame.type == MessageType::CLOSE) {
                (void)decode_as<CloseMessage>(bind_frame);
                if (pending_job || acknowledged_ordinal != committed_ordinal)
                    throw std::invalid_argument(
                        "R2 CLOSE arrived with an unsettled receipt prefix");
                clear_link();
                result.status = ServerRunStatus::Completed;
                run_lease.result = EndpointTerminalResult{
                    EndpointTerminalResultState::Committed, 0};
                close_now(socket);
                co_return result;
            }
            if (bind_frame.type == MessageType::COMMIT_ACK) {
                const CommitAck ack = decode_as<CommitAck>(bind_frame);
                if (ack.relationship_id != hello.relationship_id ||
                    ack.relationship_epoch != hello.relationship_epoch ||
                    ack.physical_link_generation !=
                        hello.physical_link_generation ||
                    ack.contiguous_verified_ordinal <= acknowledged_ordinal ||
                    ack.contiguous_verified_ordinal > committed_ordinal ||
                    !impl_->config.acknowledge_p51_receipt ||
                    !impl_->config.acknowledge_p51_receipt(hello, ack))
                    throw std::invalid_argument(
                        "R2 COMMIT_ACK identity or cumulative prefix mismatch");
                acknowledged_ordinal = ack.contiguous_verified_ordinal;
                continue;
            }
            if (bind_frame.type != MessageType::JOB_BIND)
                throw std::invalid_argument(
                    "R2 link expected JOB_BIND, COMMIT_ACK, or CLOSE");
            const JobBind binding = decode_as<JobBind>(bind_frame);
            if (pending_job ||
                binding.relationship_ordinal != committed_ordinal + 1 ||
                binding.relationship_ordinal - acknowledged_ordinal > hello.window ||
                binding.physical_link_generation !=
                    hello.physical_link_generation ||
                binding.profile != hello.profile ||
                binding.raw_bytes > raw_cap ||
                binding.raw_bytes > output_cap)
                throw std::invalid_argument("R2 JOB_BIND is outside the link window");
            if (!impl_->config.consume_p51_job_reservation)
                throw std::invalid_argument("R2 job reservation consumer is unavailable");
            const auto job_lease =
                impl_->config.consume_p51_job_reservation(hello, binding);
            if (!job_lease) {
                Impl::Route& route = impl_->require_route(session);
                if (route.recovery_install &&
                    Impl::same_recovery_binding(
                        route.recovery_install->binding, binding) &&
                    impl_->config.p51_source_reservation_terminal &&
                    impl_->config.p51_source_reservation_terminal(
                        route.recovery_install->binding) &&
                    Impl::same_recovery_input(
                        route.recovery_install->begin,
                        TxBegin{.tu_seq = binding.tu_seq,
                                .profile = binding.profile,
                                .raw_bytes = binding.raw_bytes,
                                .raw_digest = binding.raw_digest}))
                    impl_->discard_recovery_install(hello.c_store_guid, route);
                throw std::invalid_argument(
                    "R2 JOB_BIND source reservation is no longer live");
            }
            if (job_lease->binding != binding ||
                job_lease->input_key.c_store_guid != hello.c_store_guid ||
                job_lease->input_key.tu_seq != binding.tu_seq ||
                job_lease->armed.reservation_id != binding.reservation_id.bytes)
                throw std::invalid_argument("R2 JOB_BIND lacks its exact source lease");
            session.current_p51_binding = binding;
            arm_job_deadline(job_lease->absolute_deadline);
            set_completion_deadline(job_lease->absolute_deadline);
            pending_job = true;

            const Digest128 binding_digest = job_lease->binding_digest;
            Frame begin_frame = co_await async_read_frame(
                socket, frame_cap, stamp(AsyncOperationKind::ReadHeader),
                impl_->completions, verify);
            if (begin_frame.type != MessageType::TU_BEGIN)
                throw std::invalid_argument("R2 JOB_BIND was not followed by TU_BEGIN");
            const TuBegin tu_begin = decode_as<TuBegin>(begin_frame);
            if (tu_begin.relationship_ordinal != binding.relationship_ordinal ||
                tu_begin.inner.tu_seq != binding.tu_seq ||
                tu_begin.inner.profile != binding.profile ||
                tu_begin.inner.raw_bytes != binding.raw_bytes ||
                tu_begin.inner.raw_digest != binding.raw_digest)
                throw std::invalid_argument("R2 TU_BEGIN differs from JOB_BIND");
            if (binding.raw_bytes > raw_cap ||
                tu_begin.inner.body.encoded_bytes > encoded_cap ||
                tu_begin.inner.body.decoded_bytes > output_cap)
                throw std::length_error("R2 TU exceeds selected link budgets");
            const uint32_t one_profile = profile_bit(hello.profile);
            impl_->begin(session, tu_begin.inner, one_profile);
            impl_->require_route(session).pending->p51_binding = binding;
            icecc::Digest128Builder outer_digest;
            outer_digest.append("R2-transaction-v1");
            outer_digest.append_digest(binding_digest);
            append_outer_frame(outer_digest, begin_frame);
            uint64_t aggregate_encoded_bytes = 0;
            Digest128 outer_transaction_digest{};

            for (;;) {
                Frame component = co_await async_read_frame(
                    socket, frame_cap, stamp(AsyncOperationKind::ReadHeader),
                    impl_->completions, verify);
                if (component.type == MessageType::TU_END) {
                    const TuEnd end = decode_as<TuEnd>(component);
                    outer_transaction_digest = outer_digest.finish();
                    if (end.relationship_ordinal != binding.relationship_ordinal)
                        throw std::invalid_argument(
                            "R2 TU_END relationship ordinal mismatch");
                    if (end.binding_digest != binding_digest)
                        throw std::invalid_argument(
                            "R2 TU_END binding digest mismatch");
                    if (end.transaction_digest != outer_transaction_digest)
                        throw std::invalid_argument(
                            "R2 TU_END transaction digest mismatch");
                    break;
                }
                if (component.type == MessageType::R2_BODY) {
                    if (component.payload.size() > encoded_cap -
                                                       std::min<uint64_t>(
                                                           aggregate_encoded_bytes,
                                                           encoded_cap))
                        throw std::length_error(
                            "R2 aggregate BODY/FILL budget exceeded");
                    aggregate_encoded_bytes += component.payload.size();
                    outer_digest.append_u8(static_cast<uint8_t>(component.type));
                    outer_digest.append_u64(component.payload.size());
                    outer_digest.append(component.payload);
                    impl_->append_body(session,
                                       BodyMessage{component.payload});
                    // The R2 sender predicted this exact NEED locally; F
                    // derives the same profile obligations after every BODY
                    // chunk but intentionally emits no NEED frame on wire.
                    // The dialogue returns an empty vector until the encoded
                    // BODY is complete, so this is safe before the first FILL.
                    (void)impl_->require_route(session)
                        .pending->dialogue->need_messages(frame_cap);
                    continue;
                }
                if (component.type == MessageType::R2_FILL) {
                    if (component.payload.size() > encoded_cap -
                                                       std::min<uint64_t>(
                                                           aggregate_encoded_bytes,
                                                           encoded_cap))
                        throw std::length_error(
                            "R2 aggregate BODY/FILL budget exceeded");
                    aggregate_encoded_bytes += component.payload.size();
                    outer_digest.append_u8(static_cast<uint8_t>(component.type));
                    outer_digest.append_u64(component.payload.size());
                    outer_digest.append(component.payload);
                    impl_->receive_fill(session,
                                        FillMessage{component.payload});
                    continue;
                }
                throw std::invalid_argument("unexpected R2 transaction frame");
            }
            if (!impl_->body_complete(session))
                throw std::invalid_argument("R2 TU_END arrived before profile completion");

            ServerMaterializationJob materialization =
                impl_->begin_materialization(
                    session, std::move(control.before_materialize_on_worker));
            ServerMaterializationCompletion materialization_completion =
                co_await async_materialize(std::move(materialization), io);
            require_operation();
            Impl::MaterializedInput materialized = impl_->finish_materialization(
                session, std::move(materialization_completion));
            if (materialized.begin.tu_seq != job_lease->input_key.tu_seq ||
                materialized.begin.raw_bytes != binding.raw_bytes ||
                materialized.begin.raw_digest != binding.raw_digest)
                throw StaleCompletion();
            result.f_apply_materialize_ns = materialized.f_apply_materialize_ns;
            const InputJobState job_state =
                impl_->select_materialized_job_state(session, materialized);
            require_operation();
            if (impl_->config.authorize_p51_job_publication &&
                !impl_->config.authorize_p51_job_publication(hello, binding)) {
                if (impl_->config.settle_p51_cancelled_job)
                    impl_->config.settle_p51_cancelled_job(hello, binding);
                throw StaleCompletion();
            }
            const TxBegin committed_begin = materialized.begin;
            const TxCommit commit = impl_->commit_materialized(
                session, std::move(materialized), job_state,
                result.candidate_input, result.completed_input,
                result.committed_input);
            const R2TxCommit r2_commit{binding.relationship_ordinal,
                                      binding_digest,
                                      outer_transaction_digest,
                                      commit};
            if (!impl_->config.record_p51_job_commit ||
                !impl_->config.record_p51_job_commit(hello, binding,
                                                     r2_commit))
                throw std::logic_error(
                    "F could not retain the exact R2 receipt before delivery");
            committed_ordinal = binding.relationship_ordinal;
            co_await async_write_message(
                socket, Message{r2_commit}, frame_cap,
                stamp(AsyncOperationKind::WriteFragment, &committed_begin),
                impl_->completions, control, verify);
            pending_job = false;
            set_completion_deadline(link_deadline);
            arm_job_deadline(deadline_after(std::chrono::seconds(60)));
        }
    } catch (const StaleCompletion&) {
        clear_link();
        close_now(socket);
        result.status = ServerRunStatus::Disconnected;
        co_return result;
    } catch (const boost::system::system_error&) {
        const bool expired = io->expired || deadline_crossed();
        clear_link();
        close_now(socket);
        result.status = expired ? ServerRunStatus::DeadlineExceeded
                                : ServerRunStatus::Disconnected;
        co_return result;
    } catch (const std::exception& error) {
        clear_link();
        result.terminal_error = bounded_error(
            impl_->config.protocol_error_code, error.what(), frame_cap);
    }
    if (result.terminal_error) {
        // Do not send the R1 ERROR control frame on an R2 link.  Until the
        // bounded R2 ERROR codec is enabled, fail closed by closing the link.
    }
    close_now(socket);
    result.status = ServerRunStatus::TerminalError;
    co_return result;
}

EndpointCancelResult P50ServerEndpoint::request_cancel(
    const EndpointCancelPermit& permit) noexcept {
    return impl_->endpoint_runs.request_cancel(permit);
}

size_t P50ServerEndpoint::cancel_all_for_incarnation(
    const SidecarLaunchIdentity& incarnation) noexcept {
    size_t cancelled = impl_->endpoint_runs.cancel_all_for_incarnation(incarnation);
    if (impl_->config.sidecar_launch &&
        *impl_->config.sidecar_launch == incarnation) {
        for (const auto& [serial, weak] : impl_->pending_r2_setup) {
            (void)serial;
            if (const auto io = weak.lock()) {
                io->cancelled = true;
                close_now(io->socket);
                ++cancelled;
            }
        }
    }
    return cancelled;
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

std::optional<std::string>
P50ServerEndpoint::global_resource_invariant_for_test() const {
    impl_->owner.check();
    return impl_->global_resources->check_invariants();
}
#endif

void P50ServerEndpoint::reset_store(FStoreGuid new_guid) {
    impl_->owner.require();
    if (new_guid == FStoreGuid{})
        throw std::invalid_argument("F store reset GUID zero is reserved");
    if (new_guid == impl_->f_guid)
        throw std::invalid_argument("F store reset requires a fresh GUID");
    for (auto& [guid, space] : impl_->namespaces) {
        for (auto& [profile, route] : space.routes) {
            (void)profile;
            if (!route.pending)
                continue;
            if (route.pending->dialogue)
                route.pending->dialogue->discard_tentative();
            impl_->release_pending(*route.pending);
        }
        impl_->invalidate_p29v1_codec(guid, space);
        if (space.active_session != 0) {
            const auto live = impl_->live_sessions.find(space.active_session);
            Impl::Session invalidated{.serial = space.active_session,
                                      .f_guid = impl_->f_guid,
                                      .operation = std::nullopt,
                                      .deadline = std::nullopt,
                                      .c_guid = guid,
                                      .profile = live == impl_->live_sessions.end()
                                                     ? std::nullopt
                                                     : live->second.profile,
                                      .candidate_revision = 0,
                                      .candidate_state = std::nullopt,
                                      .current_p51_binding = std::nullopt,
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
