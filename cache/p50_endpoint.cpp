#include "p50_endpoint.h"
#include "p50_input_backing_reaper.h"
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
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <latch>
#include <limits>
#include <map>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace icecc::p50 {
namespace {

// Early-FILL admission bound (P50_ROUTE_PIPELINING.md section 1.1 errata
// item 4): a would-be successor whose NEED could list more than this many
// missing regions must not open a successor dialogue (the NEED could
// outgrow a socket buffer); it waits for the window to empty and runs the
// unchanged head path instead. 4096 is the production value (~20 KiB NEED);
// ICECC_P50_EARLY_NEED_REGION_BOUND exists only so the unit test for that
// fallback (T7) can force the boundary deterministically instead of
// depending on interner region shapes. Read once per process.
size_t early_need_region_bound() noexcept {
    // Read per call (never cached): the unit test flips it between suites.
    const char* raw = ::getenv("ICECC_P50_EARLY_NEED_REGION_BOUND");
    if (raw == nullptr || *raw == '\0')
        return size_t{4096};
    const long parsed = ::atol(raw);
    return parsed < 0 ? size_t{4096} : static_cast<size_t>(parsed);
}

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

// Boost 1.87 dropped the error_code overload of the timer's cancel().
void cancel_quietly(asio::steady_timer& timer) noexcept {
    try {
        timer.cancel();
    } catch (...) {
    }
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

    EndpointCodecPool() {
        // Diagnostic-only: name this pool's worker threads so a freeze
        // snapshot can tell them apart from unnamed pool threads elsewhere
        // in the process. std::latch blocks the constructor until every
        // worker has run its naming task, so the names are guaranteed set
        // before any real codec work is posted to the pool.
        std::latch named{kWorkerCount};
        for (size_t i = 0; i < kWorkerCount; ++i) {
            asio::post(pool_, [&named] {
                ::pthread_setname_np(::pthread_self(), "p50-codec");
                named.count_down();
            });
        }
        named.wait();
    }

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
        // Bounded by authority_limits.route_pipeline_depth (default 1,
        // reproducing the old single-optional behavior exactly). FIFO:
        // commit() only ever retires the front entry. A non-tail release
        // conservatively clears the whole deque (mirroring
        // CRoute::abandon_active()'s own uniform invalidation of its
        // active_queue_) per P50_ROUTE_PIPELINING.md Step 4's settled
        // "invalidate all outstanding successors uniformly" decision.
        std::deque<uint64_t> uncommitted_route_entries;
        std::unique_ptr<CRoute> p29_route;
        Digest128 p29v1_system_source_fingerprint{};
    };

    // One request/input record is C-wide.  A route view below carries only
    // relationship cursor state and its own prepared envelope.
    struct Shared {
        PrepareRequestKey request{};
        uint64_t raw_bytes = 0;
        Digest128 raw_digest{};
        std::shared_ptr<const std::vector<uint8_t>> raw;
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
            authority_limits.max_route_state_bytes == 0)
            throw std::invalid_argument("preparation-authority limits must be nonzero");
        if (profile != ProfileId::P29V1 &&
            profile != ProfileId::ZSTD_TU &&
            profile != ProfileId::ZSTD_ROUTE)
            throw std::invalid_argument("preparation authority profile is unsupported");
        p29_authority = std::make_unique<CAuthority>(
            c_guid, p29::OnlineS1::Config{}, first_tu_seq,
            fault_injection);
        if (profile == ProfileId::P29V1)
            ensure_p29v1_locked();
    }

    void ensure_p29v1() {
        std::lock_guard lock(source_mutex);
        ensure_p29v1_locked();
    }

    void ensure_p29v1_locked() {
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
        return {CStoreGuid{}, FStoreGuid::from_u64(UINT64_C(0x503239434c49454e)), 1,
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
        if (key.profile == ProfileId::P29V1) {
            ensure_p29v1();
            // A relationship that starts before the asynchronous daemon
            // fingerprint is ready remains reuse-off for its lifetime.  A
            // later relationship may capture the completed fingerprint;
            // neither relationship can change its reuse decision mid-route.
            state->p29v1_system_source_fingerprint =
                p29_system_source_fingerprint();
        }
        if (key.profile == ProfileId::P29V1)
            state->p29_route = std::make_unique<CRoute>(
                *p29_authority, key.f_store_guid, HistoryNonce{1}, nullptr,
                authority_limits.route_pipeline_depth);
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
    // Serializes TU_SEQ allocation and interning between the owner and
    // prepare_source callers.
    std::mutex source_mutex;
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
    std::shared_ptr<Impl::Shared> shared;
    std::optional<TuSeq> reserved_tu_seq;
    if (const auto request_position = impl_->requests.find(request);
        request_position != impl_->requests.end()) {
        shared = request_position->second;
        if (shared->raw_bytes != exact_input.size())
            throw std::invalid_argument("PrepareRequestKey was reused for different input");
        if (shared->raw->data() != exact_input.data() &&
            !std::equal(shared->raw->begin(), shared->raw->end(), exact_input.begin()))
            throw std::invalid_argument("PrepareRequestKey was reused for different input");
        if (const auto route_entry = shared->entries.find(route_key);
            route_entry != shared->entries.end())
            return PreparedTuHandle(impl_->identity, route_entry->second);
    }

    // Do not allocate route state or pin an asynchronous P29V1 fingerprint for
    // a malformed request.  Once admitted, the route captures its fingerprint
    // exactly once and cannot change its reuse decision mid-relationship.
    Impl::RouteState& route = impl_->route_state(route_key);

    // Profile-scoped depth (design doc §7(1)): only a P29V1 route may hold
    // more than one uncommitted successor; ZSTD_ROUTE stays at one.
    if ((route.profile == ProfileId::ZSTD_ROUTE ||
         route.profile == ProfileId::P29V1) &&
        route.uncommitted_route_entries.size() >=
            (route.profile == ProfileId::P29V1
                 ? std::max<uint32_t>(1, impl_->authority_limits.route_pipeline_depth)
                 : 1u))
        throw std::logic_error(
            "selected route profile already has its full pipeline of successors outstanding");

    if (impl_->entries.size() >= impl_->authority_limits.max_live_entries)
        throw std::length_error("C preparation authority reached its live-entry bound");
    const uint64_t retained_room =
        impl_->authority_limits.max_retained_encoded_bytes - impl_->retained_bytes;
    if (retained_room == 0)
        throw std::length_error("C preparation authority reached its retained-byte bound");

    std::unique_lock source_lock(impl_->source_mutex, std::defer_lock);
    if (!shared) {
        const Digest128 raw_digest = digest128(exact_input);
        source_lock.lock();
        shared = std::make_shared<Impl::Shared>();
        shared->request = request;
        shared->raw_bytes = exact_input.size();
        shared->raw_digest = raw_digest;
        shared->raw = std::make_shared<const std::vector<uint8_t>>(
            exact_input.begin(), exact_input.end());
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
    bool p29_active_started = false;
    try {
        if (route.profile == ProfileId::ZSTD_TU) {
            const ZstdTuEnvelope envelope = impl_->codec.encode(
                HistoryNonce{1}, RelSeq{0}, tu_seq, Digest128{}, exact_input,
                admission_limits);
            prepared = std::make_shared<const PreparedInputEnvelope>(
                PreparedInputEnvelope{envelope.begin, envelope.body});
        } else if (route.profile == ProfileId::ZSTD_ROUTE) {
            const ZstdRouteEnvelope envelope = impl_->route_codec.encode(
                HistoryNonce{1}, RelSeq{0}, tu_seq, Digest128{},
                std::span<const uint8_t>(route.committed_route_history), exact_input,
                admission_limits);
            prepared = std::make_shared<const PreparedInputEnvelope>(
                PreparedInputEnvelope{envelope.begin, envelope.body});
        } else if (route.profile == ProfileId::P29V1) {
            if (!shared->p29_source) {
                if (!source_lock.owns_lock())
                    source_lock.lock();
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
            // A TU interned before another TU's failure cannot begin either:
            // the same typed failure, not a route logic error (which the
            // sender would escalate to a sticky sidecar replacement).
            if (!impl_->p29_authority->p29v1_runnable())
                throw P29V1CapabilityUnavailable(
                    "P29V1 interner is not runnable until READY lease replacement");
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
                          false};
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
        if (route.profile == ProfileId::ZSTD_ROUTE ||
            route.profile == ProfileId::P29V1)
            route.uncommitted_route_entries.push_back(entry_id);
        impl_->consume_entry();
        if (reserved_tu_seq)
            impl_->p29_authority->commit_tu_seq(*reserved_tu_seq);
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
        if ((route.profile == ProfileId::ZSTD_ROUTE ||
             route.profile == ProfileId::P29V1) &&
            !route.uncommitted_route_entries.empty() &&
            route.uncommitted_route_entries.back() == entry_id)
            route.uncommitted_route_entries.pop_back();
        throw;
    }
}

class P50PreparationAuthority::PreparedSource {
public:
    std::shared_ptr<Impl::Shared> shared;
};

std::shared_ptr<const P50PreparationAuthority::PreparedSource>
P50PreparationAuthority::prepare_source(
    PrepareRequestKey request,
    std::shared_ptr<const std::vector<uint8_t>> exact_input,
    Digest128 raw_digest) {
    if (!exact_input || request.producer_session == 0 || request.request_token == 0 ||
        exact_input->size() > impl_->zstd_limits.max_raw_bytes)
        return nullptr;
    auto shared = std::make_shared<Impl::Shared>();
    shared->request = request;
    shared->raw_bytes = exact_input->size();
    shared->raw_digest = raw_digest;
    shared->raw = std::move(exact_input);
    std::lock_guard lock(impl_->source_mutex);
    impl_->ensure_p29v1_locked();
    const TuSeq tu_seq = impl_->p29_authority->reserve_tu_seq();
    shared->tu_seq = tu_seq;
    shared->p29_source = impl_->p29_authority->prepare_p29v1_at_seq(
        *shared->raw, tu_seq, raw_digest);
    impl_->p29_authority->commit_tu_seq(tu_seq);
    auto prepared = std::make_shared<PreparedSource>();
    prepared->shared = std::move(shared);
    return prepared;
}

void P50PreparationAuthority::install_source(
    PrepareRequestKey request, std::shared_ptr<const PreparedSource> prepared) {
    impl_->owner.require();
    if (prepared && prepared->shared->request == request)
        impl_->requests.emplace(request, prepared->shared);
}

void P50PreparationAuthority::release_source(PrepareRequestKey request) noexcept {
    const auto position = impl_->requests.find(request);
    if (position != impl_->requests.end() && position->second->entries.empty())
        impl_->requests.erase(position);
}

CRoute& P50PreparationAuthority::p29v1_successor(PreparedTuHandle handle) {
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
    if (!route.p29_route ||
        std::find(route.uncommitted_route_entries.begin(),
                  route.uncommitted_route_entries.end(),
                  handle.entry_id_) == route.uncommitted_route_entries.end())
        throw std::logic_error("P29V1 NEED does not identify the route successor");
    return *route.p29_route;
}

std::span<const uint8_t> P50PreparationAuthority::answer_p29v1_need(
    PreparedTuHandle handle, std::span<const uint8_t> inner_need) {
    return p29v1_successor(handle).build_fill_v1(inner_need);
}

std::optional<std::span<const uint8_t>>
P50PreparationAuthority::fill_p29v1_before_need(PreparedTuHandle handle) {
    // C reads nothing until FILL is written, so F's STATE and NEED must fit
    // in C's receive buffer: a NEED inner stream is at most 20 + 5 bytes per
    // missing Region, i.e. about 20 KiB here.
    return p29v1_successor(handle).fill_v1_before_need(
        early_need_region_bound());
}

bool P50PreparationAuthority::p29v1_fill_before_need_possible(
    PreparedTuHandle handle) {
    return p29v1_successor(handle).fill_before_need_possible(
        early_need_region_bound());
}

void P50PreparationAuthority::confirm_p29v1_need(
    PreparedTuHandle handle, std::span<const uint8_t> inner_need) {
    // Route pipelining (P50_ROUTE_PIPELINING.md section 1.1, 2026-10-08
    // errata): route the confirm through the per-entry path so a
    // predict-committed entry (serializer pending already gone) validates
    // against its recorded snapshot; the newest not-yet-predict-committed
    // entry keeps the exact legacy behavior.  p29v1_successor() re-checks
    // the handle/entry/route membership exactly as every other NEED-side
    // accessor.
    p29v1_successor(handle).confirm_need_v1(
        resolve(handle)->begin.rel_seq, inner_need);
}

void P50PreparationAuthority::commit_predicted_p29v1(PreparedTuHandle handle) {
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
    if (!route.p29_route ||
        std::find(route.uncommitted_route_entries.begin(),
                  route.uncommitted_route_entries.end(),
                  handle.entry_id_) == route.uncommitted_route_entries.end())
        throw std::logic_error(
            "P29V1 predict-commit does not identify the route successor");
    route.p29_route->commit_predicted_v1();
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
    if (!route.p29_route ||
        std::find(route.uncommitted_route_entries.begin(),
                  route.uncommitted_route_entries.end(),
                  handle.entry_id_) == route.uncommitted_route_entries.end())
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
    if (!route.p29_route ||
        std::find(route.uncommitted_route_entries.begin(),
                  route.uncommitted_route_entries.end(),
                  handle.entry_id_) == route.uncommitted_route_entries.end())
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
    if (!route.p29_route ||
        std::find(route.uncommitted_route_entries.begin(),
                  route.uncommitted_route_entries.end(),
                  handle.entry_id_) == route.uncommitted_route_entries.end() ||
        !entry.shared->p29_source)
        throw std::logic_error(
            "P29V1 route reset does not identify its prepared successor");

    if (!impl_->p29_authority->p29v1_runnable())
        throw P29V1CapabilityUnavailable(
            "P29V1 interner is not runnable until READY lease replacement");
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
    if (--entry.references != 0)
        return entry.references;
    if ((entry.route.profile == ProfileId::ZSTD_ROUTE ||
         entry.route.profile == ProfileId::P29V1) &&
        !entry.committed &&
        std::find(route.uncommitted_route_entries.begin(),
                  route.uncommitted_route_entries.end(),
                  handle.entry_id_) != route.uncommitted_route_entries.end())
    {
        // Conservative by design (P50_ROUTE_PIPELINING.md Step 4): releasing
        // any uncommitted successor before it commits invalidates every
        // other outstanding successor on this route too, not just this one.
        // CRoute::abandon_active() already clears its whole active_queue_
        // uniformly; mirror that here so the two structures never diverge.
        route.uncommitted_route_entries.clear();
        if (entry.route.profile == ProfileId::P29V1 && route.p29_route)
            route.p29_route->abandon_active();
    }
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

void P50PreparationAuthority::commit(PreparedTuHandle handle) {
    impl_->owner.require();
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    auto& entry = position->second;
    Impl::RouteState& route = impl_->route_state(entry.route);
    if (entry.route.profile == ProfileId::ZSTD_ROUTE && !entry.committed) {
        if (route.uncommitted_route_entries.empty() ||
            route.uncommitted_route_entries.front() != handle.entry_id_)
            throw std::logic_error("ZSTD_ROUTE commit is not its prepared successor");
        const size_t limit = static_cast<size_t>(std::min<uint64_t>(
            impl_->zstd_limits.max_history_bytes,
            uint64_t{1} << impl_->zstd_limits.max_window_log));
        const std::vector<uint8_t>& raw = *entry.shared->raw;
        if (raw.size() >= limit) {
            route.committed_route_history.assign(
                raw.end() - static_cast<std::ptrdiff_t>(limit), raw.end());
        } else {
            const size_t excess = route.committed_route_history.size() + raw.size() > limit
                                      ? route.committed_route_history.size() + raw.size() - limit
                                      : 0;
            if (excess != 0)
                route.committed_route_history.erase(
                    route.committed_route_history.begin(),
                    route.committed_route_history.begin() + excess);
            route.committed_route_history.insert(route.committed_route_history.end(),
                                                  raw.begin(), raw.end());
        }
        route.committed_route_history_digest = digest128(
            std::span<const uint8_t>(route.committed_route_history));
        entry.committed = true;
        route.uncommitted_route_entries.pop_front();
    }
    if (entry.route.profile == ProfileId::P29V1 && !entry.committed) {
        if (route.uncommitted_route_entries.empty() ||
            route.uncommitted_route_entries.front() != handle.entry_id_)
            throw std::logic_error("P29V1 commit is not its prepared successor");
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
        route.p29_route->accept_commit(commit);
        entry.committed = true;
        route.uncommitted_route_entries.pop_front();
    }
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

// See the design-note comment above P50CRouteOwner::reset_f_store_exact in
// client/p50_route_owner.cpp for the full safety argument. In short: a route
// key only reaches this function after its caller has proven the key's F
// incarnation is dead, so every Entry still filed under it is orphaned
// (its sender already reached a terminal, non-Committed result and simply
// never released the handle) rather than actively owned. This mirrors
// release()'s uncommitted/retained-bytes bookkeeping above, but erases
// unconditionally instead of requiring the caller to hold the handle.
bool P50PreparationAuthority::discard_route(PreparationRouteKey route_key) noexcept {
    try {
        impl_->owner.require();
        const auto route_position = impl_->routes.find(route_key);
        Impl::RouteState* route =
            route_position != impl_->routes.end() ? route_position->second.get() : nullptr;
        // Adapted to the route-pipelining successor deque (formerly the
        // single scalar optional uncommitted_route_entry): the whole
        // outstanding-successor chain is invalidated uniformly, the same
        // uniform clear release()'s non-tail path and abandon_route()
        // apply. abandon_active() is called at most once per discard_route
        // call -- the deque clear below makes every later membership check
        // fail, and CRoute::abandon_active() is not idempotent-safe to lean
        // on for repeated calls.
        bool abandoned = false;
        for (auto position = impl_->entries.begin(); position != impl_->entries.end();) {
            if (position->second.route != route_key) {
                ++position;
                continue;
            }
            Impl::Entry& entry = position->second;
            if (route && !entry.committed &&
                std::find(route->uncommitted_route_entries.begin(),
                          route->uncommitted_route_entries.end(),
                          position->first) !=
                    route->uncommitted_route_entries.end()) {
                route->uncommitted_route_entries.clear();
                if (!abandoned && route_key.profile == ProfileId::P29V1 &&
                    route->p29_route)
                    route->p29_route->abandon_active();
                abandoned = true;
            }
            impl_->retained_bytes -= entry.retained_bytes;
            const PrepareRequestKey request = entry.request;
            const std::shared_ptr<Impl::Shared> shared = entry.shared;
            position = impl_->entries.erase(position);
            shared->entries.erase(route_key);
            if (shared->entries.empty())
                impl_->requests.erase(request);
        }
        impl_->routes.erase(route_key);
        return true;
    } catch (...) {
        return false;
    }
}

void P50PreparationAuthority::abandon_route(PreparedTuHandle handle) {
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
    if (!route.p29_route ||
        std::find(route.uncommitted_route_entries.begin(),
                  route.uncommitted_route_entries.end(),
                  handle.entry_id_) == route.uncommitted_route_entries.end())
        throw std::logic_error(
            "abandon_route does not identify the route successor");
    route.p29_route->abandon_active();
    // Step 4d core (P50_ROUTE_PIPELINING.md section 1.1 errata): a pipeline
    // poison invalidates the WHOLE outstanding chain uniformly --
    // CRoute::abandon_active() clears its active_queue_, and the authority's
    // own uncommitted-successor deque must not survive either, or the next
    // TX's commit() finds a stale front ("is not its prepared successor")
    // and the route is stranded. Same uniform clear the release path applies.
    route.uncommitted_route_entries.clear();
}

ProfileId P50PreparationAuthority::profile() const {
    impl_->owner.check();
    return impl_->profile;
}

uint32_t P50PreparationAuthority::route_pipeline_depth() const {
    impl_->owner.check();
    return std::max<uint32_t>(1, impl_->authority_limits.route_pipeline_depth);
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
        // route->c_store_guid carries a ROUTE_LANES lane GUID (or the base
        // GUID at lane 0). This endpoint's own identity -- and therefore the
        // SessionHello it sends F -- must follow the same lane GUID: F then
        // treats each lane as its own session/namespace, which is what gives
        // every lane an independent "one uncommitted successor" slot on F's
        // side too, with no wire-format change (the field's content is still
        // a plain CStoreGuid, just a derived one). Without this, F would
        // always echo back the base GUID while the sender compares against
        // the lane GUID, so every lane above 0 would fail
        // committed_from_witness() permanently.
        c_guid = (route && route->c_store_guid != CStoreGuid{})
                     ? route->c_store_guid
                     : preparation->c_store_guid();
        validate_caps(caps);
        if (caps.zstd != preparation->zstd_limits())
            throw std::invalid_argument(
                "C endpoint and preparation authority use different ZSTD_TU caps");
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
        // Step 4d core: the head dialogue always has session.serial ==
        // active_session (it stores its own serial there), so this is
        // byte-identical for it. A successor dialogue deliberately does
        // NOT touch active_session (it is not the head); stamping its own
        // serial here keeps its completions fresh instead of failing the
        // live-identity check against the head's serial.
        result.session_serial = session.serial;
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

    // Step 4d core: dialogue-scoped completion identity for a successor.
    // The shared stamp()/live_identity() bind `active` -- the HEAD -- and
    // re-read it AFTER the wire operation's suspension, so a successor's
    // verify races the head's retirement (its accept resets `active`
    // mid-window) and fails with StaleCompletion. A successor binds its
    // OWN frozen begin instead, which is also the identity it keeps after
    // promotion. The head path is untouched (byte-identical at depth 1).
    CompletionStamp stamp_bound(const Session& session,
                                AsyncOperationKind operation,
                                const TxBegin& bound) const {
        CompletionStamp result;
        result.actor = ActorSide::C;
        result.operation = operation;
        result.c_store_guid = session.c_guid;
        result.f_store_guid = session.f_guid.value_or(FStoreGuid{});
        result.session_serial = session.serial;
        result.history_nonce = bound.history_nonce;
        result.rel_seq = bound.rel_seq;
        result.tu_seq = bound.tu_seq;
        result.transaction_digest = bound.transaction_digest;
        result.raw_digest = bound.raw_digest;
        result.transaction_bound = true;
        return result;
    }
    CompletionLiveIdentity live_identity_bound(
        const Session& session, const TxBegin& bound) const {
        CompletionLiveIdentity result;
        result.actor = ActorSide::C;
        result.c_store_guid = c_guid;
        result.f_store_guid = session.f_guid.value_or(FStoreGuid{});
        result.session_serial = session.serial;
        bind_live_identity(result, bound);
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
        // Step 4d core (brief point 4): retiring the head frees both the
        // successor parked on it and a window slot. Both waits are
        // level-triggered, so every waiter is woken and each re-checks its
        // own predicate. At depth 1 both structures are empty and this is
        // a no-op.
        wake_retire_waiters();
        wake_window_slot_waiters();
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
    bool pipelined_begin = false;
    // Step 4d (research/designs/P50_ROUTE_PIPELINING.md): set when any
    // outstanding dialogue deviates while the window holds a successor;
    // cleared LAST by reset_after_pipeline_poison(), which also bumps
    // pipeline_epoch. While set, no successor dialogue is opened and every
    // waiter wakes to fail.
    bool poisoned = false;
    // Step 4d core: the outstanding-dialogue window. `active` above stays
    // the HEAD (the oldest outstanding dialogue, exactly today's single
    // slot at depth 1); successors below are ordered by rel_seq and never
    // touch the confirmed cursor fields until promoted into `active`.
    struct Successor {
        RelSeq rel_seq{};
        PreparedTuHandle handle;
        PreparedInputPtr prepared;
        std::shared_ptr<ClientIoState> io;  // nullopt-equivalent: null until
                                           // its socket is opened/registered
    };
    std::deque<Successor> successors;
    // A prepared TX waiting for an empty window before taking the head
    // path (fill_p29v1_before_need returned nullopt, so it must not open a
    // successor dialogue). At most one per endpoint: it holds the route
    // transfer lock while waiting, and one endpoint serves one route. It
    // counts toward the window so the endpoint's bound never diverges from
    // the authority's uncommitted-successor bound.
    bool awaiting_head = false;
    // Level-triggered retire waiters: a successor parked on
    // wait_for_predecessor_retired(predecessor) registers its timer under
    // that predecessor rel_seq; accept() commits the head, then cancels the
    // waiter for the just-committed rel_seq. Empty at depth 1.
    std::map<uint64_t, std::shared_ptr<asio::steady_timer>> retire_waiters;
    // Level-triggered window-slot waiters (transfer_bytes before
    // prepare_for_route). accept(), poison, and epoch changes cancel them
    // all; each re-checks its own predicate. Empty at depth 1.
    std::vector<std::shared_ptr<asio::steady_timer>> window_slot_waiters;
    // Bumped by reset_after_pipeline_poison(); a successor that wakes from
    // a wait into a different epoch must fail, not resume.
    uint64_t pipeline_epoch = 0;
    [[nodiscard]] uint32_t route_pipeline_depth() const {
        // Profile-scoped: only a P29V1 route can pipeline (design doc §7(1)).
        return preparation && caps.profile == ProfileId::P29V1
                   ? preparation->route_pipeline_depth()
                   : 1;
    }
    [[nodiscard]] size_t window_size() const {
        return (active ? 1u : 0u) + (awaiting_head ? 1u : 0u) +
               successors.size();
    }
    // C1 (2026-10-09 admission-race fix): anything outstanding on this route
    // -- a live head dialogue, any admitted-but-not-yet-promoted successor,
    // or a head session still winding down (active_session is cleared only
    // at the very end of the head's run coroutine, AFTER accept() already
    // reset `active`, so the commit-to-promotion window is observable as
    // active_session != 0 with nothing else set). The entry dispatch takes
    // the pipelined branch on this, not on active_session alone.
    [[nodiscard]] bool outstanding() const {
        return active.has_value() || !successors.empty() || active_session != 0;
    }
    // Deviation (P50_ROUTE_PIPELINING.md section 1.1, 2026-10-08 errata):
    // stop opening successors, tear down every outstanding successor socket
    // (F bulk-releases its parked BEGINs through its own mismatch path),
    // and wake every waiter so each fails through its existing status path.
    void poison_pipeline() {
        if (poisoned)
            return;
        poisoned = true;
        for (Successor& successor : successors) {
            if (successor.io)
                close_now(successor.io->socket);
        }
        for (const auto& [ignored, timer] : retire_waiters)
            cancel_quietly(*timer);
        for (const auto& timer : window_slot_waiters)
            cancel_quietly(*timer);
    }
    void wake_retire_waiters() {
        for (const auto& [ignored, timer] : retire_waiters)
            cancel_quietly(*timer);
    }
    void wake_window_slot_waiters() {
        for (const auto& timer : window_slot_waiters)
            cancel_quietly(*timer);
    }
    // F's fingerprint and limits from the STATE that last admitted a TX_BEGIN.
    std::optional<Digest128> route_f_fingerprint;
    SessionLimits route_limits{};
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
        // step 5c (route pipelining): session that installed this active TX.
        // A route may now have other live sessions on it at the same time --
        // a committed predecessor lingering in its post-commit EOF wait while
        // a released successor runs, or a second released connection whose
        // own install_begin lost the single-slot race -- and only the
        // installing session's disconnect may clear the active TX (and wake
        // the connections parked behind it); anyone else's disconnect must
        // leave both alone.
        uint64_t owner_serial = 0;
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

    // step 5c: a connection parked in front of the live dialogue because its
    // peeked TX_BEGIN rel_seq is ahead of the route's cursor. `io` is a
    // weak_ptr purely so a route-level release can reach the socket
    // (`io.lock()->socket.cancel()`) without itself extending that
    // connection's lifetime -- the owning run_connected coroutine's own
    // shared_ptr<ServerIoState> is the only thing keeping it alive.
    struct ParkedWaiter {
        std::weak_ptr<ServerIoState> io;
        bool released = false;
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
        std::optional<Pending> pending;
        std::shared_ptr<ProfileDialogue> dialogue;
        std::optional<ProfileId> dialogue_profile;
        // The authenticated Protocol-50 cursor survives payload eviction, but
        // a discarded continuing codec cannot honestly mirror that cursor.
        // Advertise no route until HISTORY_RESET replaces it.
        bool codec_history_reset_required = false;
        // step 5c: connections parked ahead of next_rel, keyed by the
        // rel_seq they are waiting for. See research/designs/
        // P50_ROUTE_PIPELINING.md §1.2 point 5 for the release triggers.
        std::map<uint64_t, std::vector<std::shared_ptr<ParkedWaiter>>> parked;
    };

    // step 5c: register `io` as parked on `route` until `target` becomes the
    // route's cursor. The caller's park loop must call forget_parked() with
    // the returned handle once it stops waiting (released or not), so a
    // connection that disconnects while parked never leaves a dangling entry
    // behind for a later release to iterate over.
    static std::shared_ptr<ParkedWaiter> park(
        Route& route, RelSeq target, const std::shared_ptr<ServerIoState>& io) {
        auto waiter = std::make_shared<ParkedWaiter>();
        waiter->io = io;
        route.parked[target.value].push_back(waiter);
        return waiter;
    }

    static void forget_parked(Route& route, RelSeq target,
                              const std::shared_ptr<ParkedWaiter>& waiter) {
        const auto found = route.parked.find(target.value);
        if (found == route.parked.end())
            return;
        std::vector<std::shared_ptr<ParkedWaiter>>& waiters = found->second;
        waiters.erase(std::remove(waiters.begin(), waiters.end(), waiter),
                     waiters.end());
        if (waiters.empty())
            route.parked.erase(found);
    }

    static void release_one(const std::shared_ptr<ParkedWaiter>& waiter) {
        waiter->released = true;
        if (const std::shared_ptr<ServerIoState> io = waiter->io.lock()) {
            boost::system::error_code ignored;
            io->socket.cancel(ignored);
        }
    }

    // step 5c: wake every waiter parked on `route` at exactly `target` --
    // the ordinary, single-successor release when the predecessor commits.
    // Does not erase the registry entry itself; the woken park loop does
    // that via forget_parked() so a waiter racing its own disconnect
    // cleanup is never double-erased from here.
    static void release_parked_at(Route& route, RelSeq target) {
        const auto found = route.parked.find(target.value);
        if (found == route.parked.end())
            return;
        for (const std::shared_ptr<ParkedWaiter>& waiter : found->second)
            release_one(waiter);
    }

    // step 5c: bulk-release every connection parked anywhere on `route`,
    // regardless of which rel_seq it is waiting for. Used by every release
    // trigger other than "the exact predecessor committed" (abort,
    // disconnect, HISTORY_RESET, cold adoption, sidecar stop/whole-namespace
    // eviction) -- in each of those cases route.next_rel either changes
    // discontinuously or the route itself is going away, so every parked
    // connection must re-run stage()/the live path to see the real, current
    // outcome rather than sleep waiting for a predecessor that will never
    // commit.
    static void release_all_parked(Route& route) {
        for (auto& [rel_seq_value, waiters] : route.parked) {
            for (const std::shared_ptr<ParkedWaiter>& waiter : waiters)
                release_one(waiter);
        }
        route.parked.clear();
    }

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
        // Wrote TX_COMMIT and only waits for C's EOF.
        bool committed = false;
        // step 5c (route pipelining): this session's OWN final commit, set
        // together with `committed` right before the TX_COMMIT write. A
        // committed session lingers in its post-commit write/EOF wait while
        // a released successor may already be running the next TX on the same
        // route, so post-commit completion checks must bind to this
        // session-local identity -- the route's `pending`/`last_commit` now
        // belong to the successor and would (spuriously) go stale.
        std::optional<TxCommit> final_commit;
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
                return !item.second.pending && !item.second.interrupted;
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

    // Hands a batch of just-released input backings to input_backing_reaper.
    // Best effort: if the reaper's bounded queue is full or it has been torn
    // down, submit() leaves `graveyard` untouched and it is freed right
    // here, inline, when this function returns -- exactly the pre-reaper
    // behavior for that one batch, never worse. Callers never need to know
    // which case happened.
    void submit_backing_graveyard(InputBackingBatch&& graveyard) {
        (void)input_backing_reaper.submit(std::move(graveyard));
    }

    void evict_namespace_inputs(CStoreGuid c_guid) {
        if (!namespace_is_evictable(c_guid))
            throw std::logic_error(
                "F endpoint selected a live namespace for LRU eviction");
        for (const InputRecordKey key : input_records.namespace_keys(c_guid))
            global_resources->release(c_guid, global_key(key.tu_seq));
        Namespace& space = namespaces.at(c_guid);
        InputBackingBatch graveyard;
        input_records.evict_namespace(c_guid, &graveyard);
        submit_backing_graveyard(std::move(graveyard));
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
        // step 5c: this namespace's routes are about to disappear out from
        // under any parked connection; wake every one of them now so each
        // re-runs the live path (which will see the namespace gone) instead
        // of sleeping forever on a route that no longer exists.
        const auto position = namespaces.find(c_guid);
        if (position != namespaces.end()) {
            for (auto& [profile, route] : position->second.routes)
                release_all_parked(route);
        }
        global_resources->evict(c_guid);
        InputBackingBatch graveyard;
        input_records.evict_namespace(c_guid, &graveyard);
        submit_backing_graveyard(std::move(graveyard));
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
        // step 5c: a committed session binds its OWN final commit and touches
        // no route-shared state at all. At K=1 this is byte-identical to the
        // route-bound branches below (this session's begin/commit ARE the
        // route's pending/last, and its serial IS the namespace's
        // active_session); under route pipelining the released successor
        // takes the namespace's active_session (require_route's guard would
        // throw here) and the route's pending / last_commit as soon as it
        // stages, which would otherwise rebind this finished session's
        // post-commit completion checks to the successor's TX and spuriously
        // stale it long after its own commit landed.
        if (current.final_commit) {
            bind_live_identity(result, *current.final_commit);
            return result;
        }
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
        retire_committed_session(hello.c_store_guid);
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

    // C sends a new HELLO only once it is done with its previous session.  If
    // that session has committed, its EOF may still be in flight on the other
    // connection; retire it now so the EOF cannot advance the revision under
    // the new candidate.
    void retire_committed_session(CStoreGuid c_guid) {
        const auto space = namespaces.find(c_guid);
        if (space == namespaces.end() || space->second.active_session == 0)
            return;
        const auto previous = live_sessions.find(space->second.active_session);
        if (previous == live_sessions.end() || !previous->second.committed)
            return;
        Session retired;
        retired.serial = previous->first;
        retired.c_guid = previous->second.c_guid;
        retired.profile = previous->second.profile;
        disconnect(retired, false);
    }

    void disconnect(const Session& session, bool retain_interrupted) {
        if (!session.c_guid || !session.profile)
            return;
        auto position = namespaces.find(*session.c_guid);
        if (position == namespaces.end() || position->second.active_session != session.serial)
            return;
        Namespace& space = position->second;
        Route* const route = find_route(space, *session.profile);
        // step 5c: only the session that installed the route's active TX
        // may clear it. At K=1 this is always the disconnecting session itself
        // (only one session is ever live on a route), so the check is
        // byte-identical; under route pipelining other live sessions exist --
        // a committed predecessor lingering in its EOF wait, or a released
        // connection whose install lost the single-slot race -- and their
        // disconnect must not abort someone else's active transaction.
        const bool owns_active_tx =
            route != nullptr && route->pending &&
            route->pending->owner_serial == session.serial;
        if (owns_active_tx) {
            if (retain_interrupted)
                route->interrupted = route->pending->begin;
            else
                route->interrupted.reset();
            if (route->pending->dialogue)
                route->pending->dialogue->discard_tentative();
            release_pending(*route->pending);
            route->pending.reset();
        }
        // step 5c: this connection was the route's active (unparked) TX;
        // its disconnect means whatever cursor it would have produced on
        // commit is never coming, so every connection parked behind it must
        // wake and re-run the live path against the real outcome
        // (interrupted/pending cleared above) instead of waiting on a
        // commit that will never arrive. A session that did NOT own the
        // active TX (committed predecessor, or a released connection that
        // never installed) has no effect on the cursor's future: its
        // disconnect must leave parked successors asleep -- their release
        // comes from the real predecessor commit (release_parked_at) or from
        // the genuine bulk triggers (HISTORY_RESET/cold adoption/eviction),
        // and a premature wake here would push them through the ordinary
        // mismatch rejection instead of their intended commit.
        if (owns_active_tx)
            release_all_parked(*route);
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
        // step 5c: insert_or_assign below wholesale-replaces the Route
        // object. Any connection parked on the OLD route would otherwise be
        // stranded -- nothing would ever call socket.cancel() on it again
        // once the registry copy it is parked in is destroyed. Wake them
        // all now so each re-runs the live path and sees the new,
        // reset route (which will reject a stale-epoch BEGIN through the
        // ordinary history_nonce mismatch path).
        if (Route* const old_route = find_route(space, *session.profile))
            release_all_parked(*old_route);
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
        route.pending->owner_serial = session.serial;
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
        // step 5c: wake exactly the successor (if any) parked waiting for
        // this rel_seq to become the route's cursor. Ordinary, single-
        // successor release path -- every other trigger is a bulk release
        // via release_all_parked() elsewhere.
        release_parked_at(route, route.next_rel);
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
    // Drains the multi-MB-to-GB backing buffers that collect_input_garbage()
    // and the namespace/whole-store eviction paths below detach from
    // input_records, off this struct's single OWNER thread. See
    // p50_input_backing_reaper.h; falls back to an inline (synchronous) free
    // whenever its bounded queue is full or it has been torn down, which
    // cannot happen here since it is destroyed along with the rest of Impl.
    InputBackingReaper input_backing_reaper;
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
                                                        EndpointTerminalResult)> on_run_terminal,
                                     std::optional<PreparationRouteKey> route)
    : impl_(std::make_unique<Impl>(std::move(preparation), caps, first_history_nonce, completions,
                                   actions, std::move(run_identity_seed),
                                   std::move(on_run_admitted),
                                   std::move(on_run_terminal), std::move(route))) {}

P50ClientEndpoint::~P50ClientEndpoint() = default;

void P50ClientEndpoint::enable_pipelined_begin() noexcept {
    impl_->pipelined_begin = true;
}

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
    // Step 4d core (research/designs/P50_ROUTE_PIPELINING.md section 1.1,
    // 2026-10-08 errata; 2026-10-09 admission-race fixes) -- entry dispatch.
    // The single-dialogue guards below stay exactly today's behavior at
    // depth 1; above depth 1 the pipelined branch admits a successor
    // dialogue instead. `prepared` has already been admitted by the
    // authority (the sender waits for a window slot BEFORE
    // prepare_for_route), so reaching here with anything outstanding means
    // this TX is the route's next successor in rel_seq order.
    //
    // C1 (2026-10-09): the dispatch takes the pipelined branch on
    // route_pipeline_depth() > 1 && outstanding() -- NOT on
    // active_session != 0 alone. The commit-to-promotion gap is real: when
    // the head commits, Impl::accept resets `active` and wakes every
    // waiter, but the successor is promoted into `active` only when its
    // own coroutine resumes. A third TX dispatched in that gap used to see
    // active_session == 0 (head path -- then the later promotion threw
    // "promoted over a live head"), or active_session != 0 with no
    // `active` and a queued successor (the old full-window throw). Both
    // poison the route through the sender's catch(std::logic_error).
    // Taking `active || !successors.empty() || active_session != 0`
    // closes it: a not-yet-promoted successor is as outstanding as a live
    // head. Depth 1 keeps today's exact checks and throws.
    if (impl_->route_pipeline_depth() > 1 && impl_->outstanding()) {
        // A poisoned route line never opens new successors: the cold reset
        // (transfer_bytes's abandon_route + reset_after_pipeline_poison)
        // must land before this endpoint admits more route work.
        if (impl_->poisoned)
            throw std::logic_error("C endpoint route is pipeline-poisoned");
        PreparedInputPtr successor_input;
        if (prepared)
            successor_input = impl_->preparation->resolve(prepared);
        if (!successor_input)
            throw std::invalid_argument(
                "C endpoint successor run has no prepared transaction");
        // C2 (2026-10-09): the T7 fallback must not recurse through the
        // pipelined branch (the old tail-call fell back into this same
        // entry with active_session still set, which could spin without
        // suspending, and at depth > 2 the window_size() < depth wait
        // predicate admitted it into the successor path). Instead: keep
        // the route lock, wait level-triggered on the NEW predicate
        // "window empty" (!active && successors.empty() && active_session
        // == 0) with the same deadline, poison/epoch handling, and
        // window_slot_waiters wake list as wait_for_window_slot, then run
        // the UNCHANGED head path directly below -- once, from here.
        const bool eager_fill =
            impl_->caps.profile != ProfileId::P29V1 ||
            impl_->preparation->p29v1_fill_before_need_possible(prepared);
        if (!eager_fill) {
            impl_->awaiting_head = true;
            const bool slot = co_await wait_for_empty_window(deadline);
            impl_->awaiting_head = false;
            if (!slot) {
                ClientRunResult result;
                result.status = ClientRunStatus::DeadlineExceeded;
                result.observation = ClientRunObservation::DeadlineExpired;
                co_return result;
            }
            // Same pre_state_digest-vs-state poison gate the old tail
            // recursion ran, now in front of the direct head path.
            if (impl_->route_known &&
                successor_input->begin.pre_state_digest != impl_->state) {
                impl_->poison_pipeline();
                ClientRunResult result;
                result.status = ClientRunStatus::Disconnected;
                result.observation = ClientRunObservation::Disconnected;
                result.reconnect = EndpointReconnectOutcome::RouteHistoryReset;
                co_return result;
            }
            // The window emptied while waiting: this TX is the head now
            // (T7 fallback, section 1.1 errata item 4). Its begun
            // pre_state_digest equals the confirmed route state once the
            // old head retired as predicted; anything else is a
            // deviation of the predicted chain, not a reconcile case.
            // Fall through to the head path below -- no recursion.
        } else {
            // C2 admission: admit as a successor whenever the window has a
            // free slot. Do NOT require `active`: a not-yet-promoted
            // successor is a valid predecessor. The full-window case is
            // unreachable (the sender waited in wait_for_window_slot
            // before prepare_for_route), so keep the guard as a
            // logic_error invariant.
            if ((impl_->active ? 1u : 0u) + impl_->successors.size() >=
                impl_->route_pipeline_depth())
                throw std::logic_error(
                    "C endpoint already has one active dialogue");
            // Guard: this TX's predecessor must be the newest outstanding
            // rel_seq -- the successor deque's back, or the active head's
            // own begin, whichever is newer. With neither outstanding the
            // predecessor has already retired (next_rel == own rel) and
            // run_connected_successor's retire-wait loop falls straight
            // through; anything else is a dispatch-order deviation.
            {
                const RelSeq own_rel = successor_input->begin.rel_seq;
                std::optional<RelSeq> newest;
                if (!impl_->successors.empty())
                    newest = impl_->successors.back().rel_seq;
                else if (impl_->active)
                    newest = impl_->active->begin.rel_seq;
                if (newest && newest->value + 1 != own_rel.value)
                    throw std::logic_error(
                        "C endpoint successor dispatch order deviated from "
                        "rel_seq order");
            }
            co_return co_await run_connected_successor(
                remote, std::move(adopted), std::move(prepared),
                std::move(successor_input), std::move(control), deadline);
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
    // Diagnostic-only timestamps for ICECC_P50_SOURCE_RESULT_TRACE. Each is
    // populated only when the corresponding wire step is actually reached on
    // this run; unreached steps leave the related result field at zero.
    std::optional<std::chrono::steady_clock::time_point> trace_write_phase_start;
    std::optional<std::chrono::steady_clock::time_point> trace_last_write_done;
    std::optional<std::chrono::steady_clock::time_point> trace_state_read_done;
    std::optional<std::chrono::steady_clock::time_point> trace_commit_read_done;
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
        trace_write_phase_start = std::chrono::steady_clock::now();
        trace_last_write_done = *trace_write_phase_start;
        {
            struct timespec wall_ts{};
            if (::clock_gettime(CLOCK_REALTIME, &wall_ts) == 0)
                result.c_begin_write_wall_ns =
                    static_cast<uint64_t>(wall_ts.tv_sec) * 1000000000ULL +
                    static_cast<uint64_t>(wall_ts.tv_nsec);
        }

        // Only fresh work on an exact retained route: anything else needs
        // STATE first.  STATE must then confirm that route or the run fails.
        const bool pipelined =
            impl_->pipelined_begin && impl_->caps.profile == ProfileId::P29V1 &&
            !impl_->active && impl_->queued && impl_->route_known &&
            impl_->f_guid && impl_->route_f_fingerprint && !impl_->run_identity_seed;
        codec::P29WireLimits wire_limits;
        wire_limits.max_tu_bytes = static_cast<size_t>(impl_->caps.zstd.max_raw_bytes);
        wire_limits.max_region_bytes = wire_limits.max_tu_bytes;
        bool fill_sent = false;
        if (pipelined) {
            impl_->preparation->pin_p29v1_system_source_reuse(
                impl_->queued_handle, *impl_->route_f_fingerprint);
            impl_->start_active(impl_->queued, impl_->queued_handle, serial);
            impl_->queued.reset();
            impl_->queued_handle = {};
            co_await async_write_message(socket, impl_->active->begin,
                                         impl_->route_limits.max_frame_payload,
                                         impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                         impl_->completions, control, verify);
            if (control.stall_stage)
                control.stall_stage->store(ClientStallStage::BeginWritten,
                                           std::memory_order_relaxed);
            trace_last_write_done = std::chrono::steady_clock::now();
            co_await async_write_component<BodyMessage>(
                socket, impl_->active->prepared->body, impl_->route_limits.max_frame_payload,
                impl_->stamp(session, AsyncOperationKind::WriteFragment),
                impl_->completions, control, verify);
            trace_last_write_done = std::chrono::steady_clock::now();
            // FILL never depends on NEED's bytes, so send it now too; NEED is
            // still read and checked before TX_COMMIT is accepted.
            const auto trace_fill_lookup_start = std::chrono::steady_clock::now();
            const auto inner_fill = impl_->preparation->fill_p29v1_before_need(
                impl_->active->handle);
            result.c_fill_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - trace_fill_lookup_start)
                    .count());
            if (inner_fill) {
                for (const FillMessage& fill : encode_p29v1_fill_messages(
                         *inner_fill, impl_->route_limits.max_frame_payload,
                         codec::p29v1_fill_inner_bound(wire_limits)))
                    co_await async_write_message(
                        socket, fill, impl_->route_limits.max_frame_payload,
                        impl_->stamp(session, AsyncOperationKind::WriteFragment),
                        impl_->completions, control, verify);
                fill_sent = true;
                trace_last_write_done = std::chrono::steady_clock::now();
            }
        }

        result.c_write_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                *trace_last_write_done - *trace_write_phase_start)
                .count());
        // Step 4 sub-step 4c: fill_sent here means the pipelined fast path
        // above already wrote BEGIN+BODY+FILL before this (the first) STATE
        // read, so this TX's last write has already happened.
        // Step 4d core (section 1.1, 2026-10-08 errata): at depth > 1 the
        // head predict-commits locally here -- after its eager FILL write,
        // BEFORE the lock releases at write-complete -- so its NEED is
        // confirmed later through its recorded snapshot and the next
        // successor's begin_tu is admitted. At depth 1 none of this runs
        // and write_complete fires exactly where 4c landed it.
        if (fill_sent) {
            if (impl_->route_pipeline_depth() > 1)
                impl_->preparation->commit_predicted_p29v1(
                    impl_->active->handle);
            if (control.write_complete)
                control.write_complete();
        }
        Frame state_frame = co_await async_read_frame(
            socket, impl_->caps.wire.max_frame_payload,
            impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions, verify);
        if (control.stall_stage)
            control.stall_stage->store(ClientStallStage::StateRead,
                                       std::memory_order_relaxed);
        trace_state_read_done = std::chrono::steady_clock::now();
        result.c_state_wait_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                *trace_state_read_done - *trace_last_write_done)
                .count());

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
        if (pipelined && same_f && !exact) {
            // This F refused a begin for a route it no longer holds.  Abort
            // it; the retry reads STATE first and resets the route.
            // Step 4d core: with successors outstanding this is a head
            // deviation -- poison the window before returning, exactly as
            // the reconcile guard below.
            if (!impl_->successors.empty())
                impl_->poison_pipeline();
            impl_->record(ActionType::TX_ABORTED, impl_->active->begin, serial,
                          impl_->active->begin.pre_state_digest);
            impl_->queued = impl_->active->prepared;
            impl_->queued_handle = impl_->active->handle;
            impl_->active.reset();
            impl_->route_f_fingerprint.reset();
            result.status = ClientRunStatus::Disconnected;
            result.observation = ClientRunObservation::Disconnected;
            result.reconnect = EndpointReconnectOutcome::RouteHistoryReset;
            close_now(socket);
            cancel_quietly(io->timer);
            impl_->active_session = 0;
            if (impl_->route_pipeline_depth() > 1)
                impl_->wake_window_slot_waiters();
            co_return result;
        }
        if (pipelined && (!exact || negotiated_f_fingerprint != *impl_->route_f_fingerprint ||
                          peer.limits != impl_->route_limits)) {
            const ErrorMessage terminal = bounded_error(
                2, "pipelined TX_BEGIN met a different F route",
                peer.limits.max_frame_payload);
            co_await async_report_client_terminal(
                socket, terminal, peer.limits.max_frame_payload,
                impl_->stamp(session, AsyncOperationKind::WriteFragment),
                impl_->completions, control, verify);
        }

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
            if (impl_->route_pipeline_depth() > 1)
                impl_->wake_window_slot_waiters();
            co_return result;
        }
        if (!pipelined && same_f && impl_->active && peer.last_commit && peer.namespace_present &&
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
            cancel_quietly(io->timer);
            impl_->active_session = 0;
            if (impl_->route_pipeline_depth() > 1)
                impl_->wake_window_slot_waiters();
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
        } else if (!impl_->successors.empty()) {
            // Step 4d core (brief point 5 / section 1.1 errata): the head
            // deviated while successors are outstanding. No reconcile
            // branch runs -- the successors' predictions are rooted at this
            // deviating chain, so the whole window is invalid. Tear down
            // the successor sockets (F bulk-releases their parked BEGINs
            // through its own mismatch path), wake every waiter, and return
            // this TX's own failure; the bridge's
            // abandon_route/reset_after_pipeline_poison does the cold reset
            // once this run returns.
            impl_->poison_pipeline();
            result.status = ClientRunStatus::Disconnected;
            result.observation = ClientRunObservation::Disconnected;
            result.reconnect = same_f ? EndpointReconnectOutcome::RouteHistoryReset
                                      : EndpointReconnectOutcome::ColdFStore;
            close_now(socket);
            cancel_quietly(io->timer);
            if (impl_->active_session == serial) {
                impl_->active_session = 0;
            if (impl_->route_pipeline_depth() > 1)
                impl_->wake_window_slot_waiters();
            }
            co_return result;
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

        if (!pipelined && impl_->caps.profile == ProfileId::P29V1) {
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
        const uint32_t frame_cap = peer.limits.max_frame_payload;
        ProfileId sent_profile = impl_->active->begin.profile;
        if (pipelined) {
            require_outbound_profile_negotiated(peer.negotiated_profiles,
                                                impl_->active->begin);
        } else {
            TxBegin begin = impl_->active->begin;
            if (control.outbound_begin_transform)
                begin = control.outbound_begin_transform(
                    begin, impl_->active->prepared->body);
            require_outbound_profile_negotiated(peer.negotiated_profiles, begin);
            sent_profile = begin.profile;
            impl_->route_f_fingerprint = negotiated_f_fingerprint;
            impl_->route_limits = peer.limits;
            co_await async_write_message(socket, begin, frame_cap,
                                         impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                         impl_->completions, control, verify);
            co_await async_write_component<BodyMessage>(
                socket, impl_->active->prepared->body, frame_cap,
                impl_->stamp(session, AsyncOperationKind::WriteFragment),
                impl_->completions, control, verify);
            if (control.stall_stage)
                control.stall_stage->store(ClientStallStage::BeginWritten,
                                           std::memory_order_relaxed);
            // Step 4 sub-step 4c: non-P29V1 profiles have no further FILL
            // round trip, so BODY above is this TX's last write. P29V1 is
            // deferred to the NEED/FILL exchange below (Site B).
            if (sent_profile != ProfileId::P29V1 && control.write_complete)
                control.write_complete();
        }

        if (sent_profile == ProfileId::P29V1) {
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
            if (fill_sent) {
                impl_->preparation->confirm_p29v1_need(
                    impl_->active->handle, need_decoder.inner_frames());
            } else {
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
                // Step 4 sub-step 4c: this is this TX's last write -- Site A
                // above did not fire (fill_sent was still false on entry).
                // Step 4d core: at depth > 1 the fresh head predict-commits
                // here, after its NEED was consumed by the answer above
                // (commit_predicted_v1 records need_confirmed) and before
                // the lock releases at write-complete. Depth 1: unchanged.
                if (impl_->route_pipeline_depth() > 1)
                    impl_->preparation->commit_predicted_p29v1(
                        impl_->active->handle);
                if (control.write_complete)
                    control.write_complete();
            }
        }
        Frame commit_frame = co_await async_read_frame(
            socket, frame_cap, impl_->stamp(session, AsyncOperationKind::ReadHeader),
            impl_->completions, verify);
        if (control.stall_stage)
            control.stall_stage->store(ClientStallStage::CommitRead,
                                       std::memory_order_relaxed);
        trace_commit_read_done = std::chrono::steady_clock::now();
        {
            struct timespec wall_ts{};
            if (::clock_gettime(CLOCK_REALTIME, &wall_ts) == 0)
                result.c_commit_read_wall_ns =
                    static_cast<uint64_t>(wall_ts.tv_sec) * 1000000000ULL +
                    static_cast<uint64_t>(wall_ts.tv_nsec);
        }
        if (trace_state_read_done)
            result.c_state_to_commit_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    *trace_commit_read_done - *trace_state_read_done)
                    .count());
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
        if (impl_->active_session == serial) {
            impl_->active_session = 0;
        if (impl_->route_pipeline_depth() > 1)
            impl_->wake_window_slot_waiters();
        }
        cancel_quietly(io->timer);
        throw;
    } catch (const std::exception& error) {
        close_now(socket);
        set_client_terminal_result(result, bounded_error(3, error.what(), terminal_cap));
    } catch (...) {
        close_now(socket);
        if (impl_->active_session == serial) {
            impl_->active_session = 0;
        if (impl_->route_pipeline_depth() > 1)
            impl_->wake_window_slot_waiters();
        }
        cancel_quietly(io->timer);
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
    // Step 4d core (brief point 5): a head that failed with successors still
    // outstanding deviated -- the successors' predictions are rooted at this
    // chain, so the whole window is torn down (never a reconcile branch),
    // exactly as a successor-side deviation would. No-op at depth 1 and on
    // every success path.
    if (result.status != ClientRunStatus::Committed && !impl_->successors.empty())
        impl_->poison_pipeline();
    if (result.status == ClientRunStatus::Committed && trace_commit_read_done)
        result.c_post_commit_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - *trace_commit_read_done)
                .count());
    cancel_quietly(io->timer);
    if (impl_->active_session == serial) {
        impl_->active_session = 0;
        if (impl_->route_pipeline_depth() > 1)
            impl_->wake_window_slot_waiters();
    }
    co_return result;
}

boost::asio::awaitable<ClientRunResult>
P50ClientEndpoint::run_connected_successor(
    std::optional<tcp::endpoint> remote, std::optional<tcp::socket> adopted,
    PreparedTuHandle prepared, PreparedInputPtr admitted,
    EndpointIoControl control,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    // Step 4d core (research/designs/P50_ROUTE_PIPELINING.md section 1.1,
    // 2026-10-08 errata). Everything here is unreachable at depth 1: the
    // entry dispatch only routes a caller here while a head dialogue is
    // outstanding and the window is not full.
    impl_->owner.require();
    ClientRunResult result;
    if (!admitted)
        throw std::invalid_argument(
            "C endpoint successor run has no prepared transaction");
    const TxBegin begin = admitted->begin;
    const RelSeq own_rel = begin.rel_seq;
    const RelSeq predecessor_rel{own_rel.value - 1};
    const uint64_t epoch_at_open = impl_->pipeline_epoch;
    // NOTE: deliberately no dispatch-time comparison of begin.pre_state_digest
    // against impl_->state -- while the head is outstanding, impl_->state is
    // the head's PRE-state and this TX's pre_state_digest is the PREDICTED
    // post-state of the head; they differ by construction. The witness is
    // validated later, against this TX's own STATE frame, after the head
    // retires (and the promotion-path fallback keeps its own
    // pre_state_digest == impl_->state check, which does hold there).
    const auto clear_slot = [this, own_rel]() {
        const auto position = std::find_if(
            impl_->successors.begin(), impl_->successors.end(),
            [&](const Impl::Successor& entry) {
                return entry.rel_seq == own_rel;
            });
        if (position != impl_->successors.end())
            impl_->successors.erase(position);
    };
    uint32_t terminal_cap = impl_->caps.wire.max_frame_payload;

    const auto deadline_crossed = [&]() {
        return deadline.has_value() &&
               std::chrono::steady_clock::now() >= *deadline;
    };
    const auto require_deadline = [&]() {
        if (deadline_crossed())
            throw boost::system::system_error(asio::error::timed_out);
    };
    // Deviation (brief point 5): stop opening successors, close every
    // successor socket, wake every waiter, and return this transfer's own
    // failure status. Never a reconcile branch for a successor.
    const auto deviate = [&](ClientRunStatus status,
                             ClientRunObservation observation,
                             EndpointReconnectOutcome reconnect) {
        impl_->poison_pipeline();
        result.status = status;
        result.observation = observation;
        result.reconnect = reconnect;
    };

    Impl::Successor& entry = [&]() -> Impl::Successor& {
        impl_->successors.push_back(
            Impl::Successor{own_rel, prepared, admitted, nullptr});
        return impl_->successors.back();
    }();
    const auto executor = co_await asio::this_coro::executor;
    auto io = std::make_shared<ClientIoState>(executor);
    entry.io = io;
    if (adopted)
        io->socket = std::move(*adopted);
    tcp::socket& socket = io->socket;
    Impl::Session session{impl_->c_guid, impl_->f_guid,
                           impl_->allocate_session(), std::nullopt, RelSeq{}};
    const uint64_t serial = session.serial;
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
    const auto verify = [&](const CompletionStamp& expected) {
        impl_->owner.require();
        CompletionStamp observed = expected;
        if (control.before_completion_check)
            control.before_completion_check(observed);
        observed = completion_for_test(observed, control);
        require_observed_completion(expected, observed);
        CompletionLiveIdentity live = impl_->live_identity_bound(session, begin);
        if (control.before_live_identity_check)
            control.before_live_identity_check(expected, live);
        require_live_completion(expected, live);
    };
    try {
        require_deadline();
        if (remote)
            co_await async_connect(socket, *remote,
                                   impl_->stamp_bound(session, AsyncOperationKind::Connect, begin),
                                   impl_->completions, verify);
        SessionHello hello;
        hello.c_store_guid = impl_->c_guid;
        hello.supported_profiles = profile_bit(impl_->caps.profile);
        hello.limits = impl_->caps.wire;
        if (impl_->caps.profile == ProfileId::P29V1)
            hello.system_source_fingerprint =
                impl_->preparation->p29v1_system_source_fingerprint(prepared);
        co_await async_write_message(
            socket, hello, impl_->caps.wire.max_frame_payload,
            impl_->stamp_bound(session, AsyncOperationKind::WriteFragment, begin),
            impl_->completions, control, verify);
        // The pipelined-begin fast path, exactly as the head's: write
        // BEGIN/BODY/FILL before reading STATE, against the route's
        // retained negotiated limits.
        const uint32_t frame_cap = impl_->route_limits.max_frame_payload;
        impl_->preparation->pin_p29v1_system_source_reuse(
            prepared, *impl_->route_f_fingerprint);
        co_await async_write_message(
            socket, begin, frame_cap,
            impl_->stamp_bound(session, AsyncOperationKind::WriteFragment, begin),
            impl_->completions, control, verify);
        if (control.stall_stage)
            control.stall_stage->store(ClientStallStage::BeginWritten,
                                       std::memory_order_relaxed);
        co_await async_write_component<BodyMessage>(
            socket, admitted->body, frame_cap,
            impl_->stamp_bound(session, AsyncOperationKind::WriteFragment, begin),
            impl_->completions, control, verify);
        codec::P29WireLimits wire_limits;
        wire_limits.max_tu_bytes =
            static_cast<size_t>(impl_->caps.zstd.max_raw_bytes);
        wire_limits.max_region_bytes = wire_limits.max_tu_bytes;
        const auto inner_fill =
            impl_->preparation->fill_p29v1_before_need(prepared);
        if (!inner_fill) {
            // fill_p29v1_before_need refused an eager FILL (NEED could list
            // more regions than the bound). A successor must not build an
            // answer-FILL path: deviate conservatively. This TX's own
            // transfer takes the cold-replacement route.
            deviate(ClientRunStatus::Disconnected,
                    ClientRunObservation::Disconnected,
                    EndpointReconnectOutcome::RouteHistoryReset);
            close_now(socket);
            cancel_quietly(io->timer);
            clear_slot();
            co_return result;
        }
        bool fill_sent = false;
        for (const FillMessage& fill : encode_p29v1_fill_messages(
                 *inner_fill, frame_cap,
                 codec::p29v1_fill_inner_bound(wire_limits)))
            co_await async_write_message(
                socket, fill, frame_cap,
                impl_->stamp_bound(session, AsyncOperationKind::WriteFragment, begin),
                impl_->completions, control, verify);
        fill_sent = true;
        // Predict-commit this TX's serializer locally, symmetric with the
        // head's Site A: frees the route serializer for the next successor's
        // begin BEFORE the route lock releases at write-complete.
        impl_->preparation->commit_predicted_p29v1(prepared);
        if (control.write_complete)
            control.write_complete();

        // Its own STATE frame on its own socket, compared against the
        // values captured at open. F defers stage()+STATE until park
        // release (design section 1.2 point 4), so a released successor
        // sees the post-predecessor cursor: its own rel_seq with its own
        // predicted pre-state.
        Frame state_frame = co_await async_read_frame(
            socket, impl_->caps.wire.max_frame_payload,
            impl_->stamp_bound(session, AsyncOperationKind::ReadHeader, begin),
            impl_->completions, verify);
        if (control.stall_stage)
            control.stall_stage->store(ClientStallStage::StateRead,
                                       std::memory_order_relaxed);
        client_checked(impl_->caps.wire.max_frame_payload, [&] {
            if (state_frame.type == MessageType::ERROR)
                throw ClientTerminalResult(decode_as<ErrorMessage>(state_frame),
                                           impl_->caps.wire.max_frame_payload);
            SessionState received = decode_as<SessionState>(state_frame);
            validate_session_state(hello, received);
            const bool exact =
                received.f_store_guid == impl_->f_guid &&
                received.namespace_present && received.route_present &&
                impl_->route_known &&
                received.history_nonce == impl_->history_nonce &&
                received.next_rel_seq == own_rel &&
                received.state_digest == begin.pre_state_digest &&
                received.system_source_fingerprint == *impl_->route_f_fingerprint &&
                received.limits == impl_->route_limits &&
                (received.negotiated_profiles &
                 profile_bit(impl_->caps.profile)) != 0;
            if (!exact)
                throw std::invalid_argument(
                    "pipelined successor STATE differs from the predicted "
                    "route witness");
        });

        // NEED, confirmed through this TX's recorded snapshot (its
        // serializer pending ended at predict-commit above).
        if (begin.profile == ProfileId::P29V1) {
            P29V1NeedStreamDecoder need_decoder(
                codec::p29v1_need_inner_bound(wire_limits));
            for (;;) {
                Frame need_frame = co_await async_read_frame(
                    socket, frame_cap,
                    impl_->stamp_bound(session, AsyncOperationKind::ReadHeader, begin),
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
            if (!fill_sent)
                throw std::logic_error(
                    "P29V1 successor confirmed NEED without an eager FILL");
            impl_->preparation->confirm_p29v1_need(
                prepared, need_decoder.inner_frames());
        }

        // Level-triggered wait for the predecessor to retire, bounded by
        // this TX's own deadline. Re-checked before arming and after every
        // wake, spurious included: predicate is the confirmed cursor
        // passing the predecessor, the route not being poisoned, and the
        // pipeline epoch this successor opened under.
        while (impl_->next_rel.value <= predecessor_rel.value) {
            if (deadline_crossed()) {
                deviate(ClientRunStatus::DeadlineExceeded,
                        ClientRunObservation::DeadlineExpired,
                        EndpointReconnectOutcome::RouteHistoryReset);
                close_now(socket);
                cancel_quietly(io->timer);
                clear_slot();
                co_return result;
            }
            if (impl_->poisoned ||
                impl_->pipeline_epoch != epoch_at_open) {
                deviate(ClientRunStatus::Disconnected,
                        ClientRunObservation::Disconnected,
                        EndpointReconnectOutcome::RouteHistoryReset);
                close_now(socket);
                cancel_quietly(io->timer);
                clear_slot();
                co_return result;
            }
            const auto arm =
                deadline
                    ? std::chrono::duration_cast<std::chrono::milliseconds>(
                          *deadline - std::chrono::steady_clock::now())
                    : std::chrono::milliseconds(10);
            auto timer = std::make_shared<asio::steady_timer>(executor, arm);
            // Successors are strictly rel_seq-ordered, so this predecessor
            // key is exclusively ours: emplace cannot collide.
            const auto [position, inserted] = impl_->retire_waiters.emplace(
                predecessor_rel.value, timer);
            if (!inserted)
                throw std::logic_error(
                    "pipelined successor predecessor already has a retire "
                    "waiter");
            boost::system::error_code error;
            co_await timer->async_wait(
                asio::redirect_error(asio::use_awaitable, error));
            if (position->second == timer)
                impl_->retire_waiters.erase(position);
            if (error && error != asio::error::operation_aborted) {
                deviate(ClientRunStatus::Disconnected,
                        ClientRunObservation::Disconnected,
                        EndpointReconnectOutcome::RouteHistoryReset);
                close_now(socket);
                cancel_quietly(io->timer);
                clear_slot();
                co_return result;
            }
        }

        // Promoted: now the head. Its STATE is already in hand (never read
        // STATE twice); install the frozen begin and read this TX's COMMIT
        // through the shared accept() path.
        if (impl_->poisoned || impl_->pipeline_epoch != epoch_at_open) {
            deviate(ClientRunStatus::Disconnected,
                    ClientRunObservation::Disconnected,
                    EndpointReconnectOutcome::RouteHistoryReset);
            close_now(socket);
            cancel_quietly(io->timer);
            clear_slot();
            co_return result;
        }
        clear_slot();
        Impl::Active promoted;
        promoted.prepared = admitted;
        promoted.handle = prepared;
        promoted.begin = begin;
        if (impl_->active)
            throw std::logic_error(
                "pipelined successor promoted over a live head dialogue");
        impl_->active = std::move(promoted);
        impl_->record(ActionType::TX_BEGIN, begin, serial,
                      begin.pre_state_digest);
        Frame commit_frame = co_await async_read_frame(
            socket, frame_cap,
            impl_->stamp_bound(session, AsyncOperationKind::ReadHeader, begin),
            impl_->completions, verify);
        if (control.stall_stage)
            control.stall_stage->store(ClientStallStage::CommitRead,
                                       std::memory_order_relaxed);
        client_checked(frame_cap, [&] {
            if (commit_frame.type == MessageType::ERROR)
                throw ClientTerminalResult(decode_as<ErrorMessage>(commit_frame),
                                           frame_cap);
            const TxCommit commit = decode_as<TxCommit>(commit_frame);
            require_deadline();
            impl_->accept(commit, ActionType::COMMIT_ACCEPTED, serial);
            io->committed = true;
            result.committed_commit = commit;
            result.committed_input =
                InputRecordKey{impl_->c_guid, commit.tu_seq};
            result.observation = ClientRunObservation::ExactCommitObserved;
        });
        result.status = ClientRunStatus::Committed;
        close_now(socket);
    } catch (const ClientTerminalResult& terminal) {
        close_now(socket);
        impl_->poison_pipeline();
        set_client_terminal_result(result, terminal.error());
    } catch (const StaleCompletion&) {
        close_now(socket);
        impl_->poison_pipeline();
        result.status = ClientRunStatus::Disconnected;
        result.observation = ClientRunObservation::Disconnected;
    } catch (const boost::system::system_error&) {
        close_now(socket);
        impl_->poison_pipeline();
        result.status = io->expired || deadline_crossed()
                            ? ClientRunStatus::DeadlineExceeded
                            : ClientRunStatus::Disconnected;
        result.observation = io->expired || deadline_crossed()
                                 ? ClientRunObservation::DeadlineExpired
                                 : ClientRunObservation::Disconnected;
    } catch (const std::exception& error) {
        // Same result contract as the head's generic catch: the failure
        // detail rides on terminal_error, the route is poisoned, and the
        // status is the ordinary disconnected observation.
        close_now(socket);
        impl_->poison_pipeline();
        set_client_terminal_result(
            result, bounded_error(3, error.what(), terminal_cap));
        result.status = ClientRunStatus::Disconnected;
        result.observation = ClientRunObservation::Disconnected;
        result.reconnect = EndpointReconnectOutcome::RouteHistoryReset;
    } catch (...) {
        close_now(socket);
        impl_->poison_pipeline();
        result.status = ClientRunStatus::Disconnected;
        result.observation = ClientRunObservation::Disconnected;
        result.reconnect = EndpointReconnectOutcome::RouteHistoryReset;
    }
    clear_slot();
    cancel_quietly(io->timer);
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

size_t P50ClientEndpoint::pipeline_window_size_for_test() const {
    impl_->owner.check();
    return impl_->window_size();
}

size_t P50ClientEndpoint::pipeline_retire_waiter_count_for_test() const {
    impl_->owner.check();
    return impl_->retire_waiters.size();
}

size_t P50ClientEndpoint::pipeline_window_slot_waiter_count_for_test() const {
    impl_->owner.check();
    return impl_->window_slot_waiters.size();
}

uint64_t P50ClientEndpoint::pipeline_epoch_for_test() const {
    impl_->owner.check();
    return impl_->pipeline_epoch;
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

bool P50ClientEndpoint::has_pipeline_poison() const noexcept {
    impl_->owner.check();
    return impl_->poisoned;
}

void P50ClientEndpoint::reset_after_pipeline_poison() {
    impl_->owner.require();
    if (!impl_->poisoned)
        throw std::logic_error(
            "reset_after_pipeline_poison() called without an outstanding "
            "pipeline poison");
    // Cold local reset: fresh history/rel-seq/state, deliberately leaving
    // f_guid untouched so the next head dialogue's same_f check still takes
    // the existing RouteHistoryReset wire branch unmodified.
    impl_->history_nonce = impl_->allocate_nonce();
    impl_->next_rel = RelSeq{0};
    impl_->state = Digest128{};
    impl_->active.reset();
    impl_->queued.reset();
    impl_->queued_handle = {};
    impl_->route_f_fingerprint.reset();
    // Step 4d core (brief point 5): any successor still parked on the old
    // route line must fail, not resume into the fresh cold route. Bump the
    // epoch so each parked wait re-checks into a mismatch, drop the window
    // (the successor coroutines own their own teardown once woken), and
    // clear the poison flag LAST.
    ++impl_->pipeline_epoch;
    impl_->successors.clear();
    impl_->wake_retire_waiters();
    impl_->wake_window_slot_waiters();
    impl_->poisoned = false;
}

boost::asio::awaitable<bool> P50ClientEndpoint::wait_for_window_slot(
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    impl_->owner.require();
    // Depth 1: the window can never hold more than the head, so the wait is
    // invisible -- no timer, no suspension, byte-identical to today.
    if (impl_->route_pipeline_depth() <= 1)
        co_return true;
    const auto executor = co_await asio::this_coro::executor;
    // (C2 shares the body below through wait_for_empty_window; this legacy
    // predicate is kept verbatim for the sender's pre-prepare wait.)
    // Level-triggered: re-check the predicate before arming and after every
    // wake (spurious included). A cancel from accept() /
    // reset_after_pipeline_poison() only ever means "re-check now". The
    // predicate is exactly the negation of prepare_for_route's admission
    // throw (p50_endpoint.cpp:1107-1110) as seen from the endpoint: a slot
    // free means the (K+1)-th transfer never reaches that throw.
    for (;;) {
        if (impl_->window_size() < impl_->route_pipeline_depth())
            co_return true;
        if (deadline && *deadline <= std::chrono::steady_clock::now())
            co_return false;
        // No deadline: the historical unbounded-wait behavior of run()
        // itself -- poll level-triggered instead of failing a caller that
        // never asked for a bound.
        const auto arm =
            deadline
                ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      *deadline - std::chrono::steady_clock::now())
                : std::chrono::milliseconds(10);
        auto timer = std::make_shared<asio::steady_timer>(executor, arm);
        if (deadline)
            impl_->window_slot_waiters.push_back(timer);
        boost::system::error_code error;
        co_await timer->async_wait(
            asio::redirect_error(asio::use_awaitable, error));
        const auto position = std::find(
            impl_->window_slot_waiters.begin(),
            impl_->window_slot_waiters.end(), timer);
        if (position != impl_->window_slot_waiters.end())
            impl_->window_slot_waiters.erase(position);
        if (error && error != asio::error::operation_aborted)
            co_return false;
        // Poisoned or a crossed epoch in the window means the route line
        // this transfer prepared against is gone: fail as a slot wait
        // timeout would, through the ordinary route-local path.
        if (impl_->poisoned)
            co_return false;
    }
}

boost::asio::awaitable<bool> P50ClientEndpoint::wait_for_empty_window(
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    impl_->owner.require();
    // C2 (2026-10-09 admission-race fix): level-triggered wait for the NEW
    // predicate "window empty" -- !active && successors.empty() &&
    // active_session == 0. Used only by the T7 (no eager FILL) fallback,
    // which must not open a successor dialogue and must instead run the
    // unchanged head path -- but only once NOTHING is outstanding: a
    // not-yet-promoted successor counts exactly like a live head (see
    // Impl::outstanding), and active_session != 0 means the previous head
    // session is still winding down. Same deadline / poison / wake-list
    // mechanics as wait_for_window_slot: woken by accept(), by
    // reset_after_pipeline_poison(), and by the C3 wake added to every
    // head-path active_session reset. Unreachable at depth 1 (the entry
    // dispatch never routes here), so it never changes K=1 bytes.
    const auto executor = co_await asio::this_coro::executor;
    for (;;) {
        if (!impl_->active && impl_->successors.empty() &&
            impl_->active_session == 0)
            co_return true;
        if (deadline && *deadline <= std::chrono::steady_clock::now())
            co_return false;
        const auto arm =
            deadline
                ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      *deadline - std::chrono::steady_clock::now())
                : std::chrono::milliseconds(10);
        auto timer = std::make_shared<asio::steady_timer>(executor, arm);
        if (deadline)
            impl_->window_slot_waiters.push_back(timer);
        boost::system::error_code error;
        co_await timer->async_wait(
            asio::redirect_error(asio::use_awaitable, error));
        const auto position = std::find(
            impl_->window_slot_waiters.begin(),
            impl_->window_slot_waiters.end(), timer);
        if (position != impl_->window_slot_waiters.end())
            impl_->window_slot_waiters.erase(position);
        if (error && error != asio::error::operation_aborted)
            co_return false;
        if (impl_->poisoned)
            co_return false;
    }
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
    // Diagnostic-only; see ServerRunResult's field comments. Populated
    // unconditionally (cheap steady_clock samples); only the gated JSONL
    // writer in p50_cache_service.cpp decides whether these are observed.
    const auto f_run_start = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> f_hello_done;
    std::optional<std::chrono::steady_clock::time_point> f_begin_done;
    std::optional<std::chrono::steady_clock::time_point> f_body_complete_done;
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
        f_hello_done = std::chrono::steady_clock::now();
        result.f_hello_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                *f_hello_done - f_run_start)
                .count());
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
        // Step 5b (route pipelining, F side): a kernel-level MSG_PEEK of
        // whatever bytes have already arrived on this connection, taken
        // immediately before stage()/the SESSION_STATE write per
        // research/designs/P50_ROUTE_PIPELINING.md §2/§5.1. A client using
        // the pre-existing enable_pipelined_begin() pattern (or, in the
        // future, any outstanding leg of a K>1 route) may have written
        // TX_BEGIN (and more) immediately after SESSION_HELLO without
        // waiting for SESSION_STATE, so those bytes can already be sitting
        // in the socket's receive buffer here. MSG_PEEK does not remove
        // anything from that buffer, so every later read on this connection
        // (the HISTORY_RESET branch, the direct-BEGIN branch, and the main
        // dialogue loop below) observes exactly the same wire bytes it
        // would have observed without this block: no behavior change at
        // K=1, where this is always nullopt or an at-cursor rel_seq that
        // step 5c's sequencer will let fall straight through. The decoded
        // value is recorded only in the diagnostic, non-wire
        // ServerRunResult field below; step 5c is what makes an admission
        // decision from it.
        {
            boost::system::error_code peek_avail_error;
            const std::size_t peek_available = socket.available(peek_avail_error);
            if (!peek_avail_error && peek_available > 0) {
                std::vector<uint8_t> peek_buffer(peek_available);
                boost::system::error_code peek_recv_error;
                const std::size_t peeked_bytes = socket.receive(
                    asio::buffer(peek_buffer), tcp::socket::message_peek, peek_recv_error);
                if (!peek_recv_error) {
                    FrameParser peek_parser(selection.limits.max_frame_payload);
                    const std::vector<Frame> peeked_frames = peek_parser.feed(
                        std::span<const uint8_t>(peek_buffer.data(), peeked_bytes));
                    for (const Frame& peeked_frame : peeked_frames) {
                        if (peeked_frame.type == MessageType::TX_BEGIN) {
                            result.f_hello_time_peeked_begin_rel_seq =
                                decode_as<TxBegin>(peeked_frame).rel_seq;
                            break;
                        }
                    }
                }
            }
        }
        // step 5c (route pipelining, F side): admission / park gate. Decide,
        // from the step-5b peek, whether this connection's first TX_BEGIN is
        // the route's next TX (K=1-identical fall-through) or an
        // already-known-ahead-of-cursor successor. If ahead, park here --
        // idle, opportunistically draining whatever further bytes land on
        // the raw socket into a local buffer, never blocking on anything
        // but the socket itself -- until a release trigger wakes it, then
        // fall through to the completely unchanged stage()/HISTORY_RESET/
        // BEGIN/BODY path below, reading from the drained buffer first and
        // the live socket after, exactly as if those bytes had arrived only
        // now. See research/designs/P50_ROUTE_PIPELINING.md §1.2 points
        // 1-5.
        //
        // Everything above this point (the HELLO read, the step-5b peek)
        // is read-only with respect to shared route state. Everything
        // below (stage()'s STATE write, HISTORY_RESET, BEGIN, BODY/NEED/
        // FILL) reads or mutates the route's shared, scalar cursor and
        // must not run early for a successor TX -- that is the whole
        // reason this gate exists.
        std::vector<Frame> buffered_frames;
        std::size_t buffered_frame_index = 0;
        if (result.f_hello_time_peeked_begin_rel_seq) {
            const RelSeq peeked_rel = *result.f_hello_time_peeked_begin_rel_seq;
            const auto namespace_position =
                impl_->namespaces.find(hello.c_store_guid);
            Impl::Route* route = nullptr;
            if (namespace_position != impl_->namespaces.end()) {
                const ProfileId profile =
                    require_single_route_profile(selection.negotiated_profiles);
                route = Impl::find_route(namespace_position->second, profile);
            }
            // Only an established route with a cursor defines "ahead of
            // next_rel". A brand-new/pre-HISTORY_RESET route, or a peeked
            // rel_seq at or behind the cursor, falls through unchanged and
            // lets the existing mismatch logic in FStore::begin /
            // Impl::prepare_begin decide (desync, stale replay, replay of
            // an already-applied TX, etc. -- no new rejection path is
            // introduced here). F has no visibility into C's pipeline
            // depth K -- that admission gate is entirely C-side
            // (cache/p50_cache_service.cpp's
            // SidecarRuntime::RouteGate) -- and does not need it: treating
            // every ahead-of-cursor rel_seq as parkable is a safe
            // superset, since C itself already bounds how many successors
            // it ever opens concurrently to K-1.
            if (route != nullptr && peeked_rel.value > route->next_rel.value) {
                // step 5c: a HISTORY_RESET cold-starts the epoch -- the TLA
                // model (research/designs/P50_ROUTE_PIPELINING.md's formal
                // companion) treats it as a cold epoch change, and this
                // route's pre-reset cursor must never gate a post-reset
                // BEGIN. Re-peek (nothing has been consumed since the
                // step-5b peek; this observes the identical bytes) and scan
                // frame order: if a HISTORY_RESET precedes the TX_BEGIN in
                // this batch, skip parking entirely so the unchanged
                // HISTORY_RESET-vs-BEGIN dispatch below processes the reset
                // -- and only then the BEGIN, against the fresh epoch's own
                // cursor -- exactly as it would without this gate. The same
                // pass derives this park's anti-DoS drain ceiling from the
                // TX's own self-declared wire size (raw_bytes covers the
                // P29V1 inner stream; body.encoded_bytes covers the outer
                // Protocol-50 BODY component) when parking does proceed;
                // that ceiling bounds memory only -- it is not a
                // protocol-correctness check, and the unchanged decode path
                // downstream still verifies every digest once this TX is
                // actually processed.
                bool history_reset_precedes_begin = false;
                bool begin_seen = false;
                std::size_t drain_ceiling = 1u << 20;
                {
                    boost::system::error_code order_avail_error;
                    const std::size_t order_available =
                        socket.available(order_avail_error);
                    if (!order_avail_error && order_available > 0) {
                        std::vector<uint8_t> order_buffer(order_available);
                        boost::system::error_code order_recv_error;
                        const std::size_t order_peeked = socket.receive(
                            asio::buffer(order_buffer),
                            tcp::socket::message_peek, order_recv_error);
                        if (!order_recv_error) {
                            FrameParser order_parser(
                                selection.limits.max_frame_payload);
                            for (const Frame& order_frame : order_parser.feed(
                                     std::span<const uint8_t>(
                                         order_buffer.data(), order_peeked))) {
                                if (order_frame.type == MessageType::HISTORY_RESET) {
                                    history_reset_precedes_begin = true;
                                    break;
                                }
                                if (order_frame.type == MessageType::TX_BEGIN) {
                                    const TxBegin order_begin =
                                        decode_as<TxBegin>(order_frame);
                                    drain_ceiling =
                                        static_cast<std::size_t>(
                                            order_begin.raw_bytes +
                                            order_begin.body.encoded_bytes) *
                                            2 +
                                        (1u << 20);
                                    begin_seen = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!history_reset_precedes_begin && begin_seen) {
                    const std::shared_ptr<Impl::ParkedWaiter> waiter =
                        Impl::park(*route, peeked_rel, io);
                    FrameParser drain_parser(selection.limits.max_frame_payload);
                    std::size_t drained_bytes_total = 0;
                    bool drain_ceiling_exceeded = false;
                    for (;;) {
                        if (waiter->released)
                            break;
                        boost::system::error_code wait_error;
                        co_await socket.async_wait(
                            tcp::socket::wait_read,
                            asio::redirect_error(asio::use_awaitable, wait_error));
                        require_operation();
                        if (waiter->released)
                            break;
                        if (wait_error) {
                            Impl::forget_parked(*route, peeked_rel, waiter);
                            throw boost::system::system_error(wait_error);
                        }
                        boost::system::error_code avail_error;
                        const std::size_t available_bytes =
                            socket.available(avail_error);
                        if (avail_error) {
                            Impl::forget_parked(*route, peeked_rel, waiter);
                            throw boost::system::system_error(avail_error);
                        }
                        if (available_bytes == 0) {
                            // A graceful close while parked surfaces here as a
                            // readable-but-empty socket; treat it exactly like
                            // any other mid-dialogue EOF.
                            Impl::forget_parked(*route, peeked_rel, waiter);
                            throw boost::system::system_error(asio::error::eof);
                        }
                        std::vector<uint8_t> drain_buffer(available_bytes);
                        boost::system::error_code recv_error;
                        const std::size_t drained_now = socket.receive(
                            asio::buffer(drain_buffer), 0, recv_error);
                        if (recv_error) {
                            Impl::forget_parked(*route, peeked_rel, waiter);
                            throw boost::system::system_error(recv_error);
                        }
                        drained_bytes_total += drained_now;
                        if (drained_bytes_total > drain_ceiling) {
                            drain_ceiling_exceeded = true;
                            break;
                        }
                        for (Frame& drained_frame : drain_parser.feed(
                                 std::span<const uint8_t>(drain_buffer.data(),
                                                          drained_now)))
                            buffered_frames.push_back(std::move(drained_frame));
                    }
                    Impl::forget_parked(*route, peeked_rel, waiter);
                    if (drain_ceiling_exceeded)
                        throw std::length_error(
                            "F endpoint parked connection exceeded its declared TX size");
                }
            }
        }
        // step 5c pre-park (route pipelining, F side; 2026-10-09): the
        // level-triggered companion of the instantaneous peek above. C
        // writes SESSION_HELLO and TX_BEGIN as two separate async writes, so
        // a pipelined successor's BEGIN routinely lands in this socket's
        // receive buffer a moment AFTER this coroutine has already read
        // HELLO -- the step-5b peek then records nothing and the
        // fall-through below would run stage() (writing a SESSION_STATE that
        // carries the still-outstanding head's cursor, which the C
        // successor's exact-witness check rejects) and then activate()
        // (whose replacement branch drops the live head's pending TX).
        // Pre-park instead whenever the peek saw no TX_BEGIN and no
        // HISTORY_RESET frame, an established route exists, and the route's
        // active TX belongs to another live session: register as parked at
        // next_rel + 1 -- the head's commit releases exactly that key
        // synchronously with its ++next_rel (release_parked_at), and every
        // abort/disconnect/HISTORY_RESET/eviction path bulk-releases
        // (release_all_parked) -- then wait, suspending on nothing but the
        // socket itself, until the in-flight TX resolves (released: fall
        // through with whatever is buffered), or this connection's first
        // post-HELLO frame completes (a TX_BEGIN ahead of the cursor is a
        // parked successor: re-key and keep waiting, bounded by that BEGIN's
        // own declared wire size; anything else -- BEGIN at or behind the
        // cursor, HISTORY_RESET -- falls through immediately with the frame
        // buffered and lets the unchanged dispatch below decide), or a
        // bounded grace expires (min(this run's own absolute operation
        // deadline, 2000 ms), implemented as a timer whose expiry cancels
        // the socket wait; falling through then reproduces exactly today's
        // behavior). Waking on the socket wait is the only suspension
        // point.
        //
        // K=1 visibility: this block only runs while ANOTHER session's TX is
        // in flight on the route (route->pending owned by a different
        // serial), which at depth 1 can only happen as a failure-recovery
        // reconnect racing the live head's tail. There F now waits up to the
        // grace bound for that TX to resolve (or for the new session's first
        // frame) instead of replacing the live head's pending TX at once.
        // No K=1 wire bytes change: a fall-through runs the identical
        // stage()/HISTORY_RESET/BEGIN dispatch below, reading the drained
        // buffer first through next_frame() exactly as the parked path does.
        {
            boost::system::error_code prepark_avail_error;
            const std::size_t prepark_available =
                socket.available(prepark_avail_error);
            bool prepark_saw_history_reset = false;
            if (!prepark_avail_error && prepark_available > 0) {
                std::vector<uint8_t> prepark_buffer(prepark_available);
                boost::system::error_code prepark_recv_error;
                const std::size_t prepark_peeked = socket.receive(
                    asio::buffer(prepark_buffer), tcp::socket::message_peek,
                    prepark_recv_error);
                if (!prepark_recv_error) {
                    FrameParser prepark_parser(
                        selection.limits.max_frame_payload);
                    for (const Frame& prepark_frame : prepark_parser.feed(
                             std::span<const uint8_t>(prepark_buffer.data(),
                                                      prepark_peeked))) {
                        // The first post-HELLO frame is the only one that
                        // matters: BEGIN-at-cursor / HISTORY_RESET /
                        // anything else all fall through unchanged, so just
                        // note which shape it is.
                        if (prepark_frame.type == MessageType::TX_BEGIN) {
                            break;
                        }
                        if (prepark_frame.type == MessageType::HISTORY_RESET) {
                            prepark_saw_history_reset = true;
                            break;
                        }
                    }
                }
            }
            if (!result.f_hello_time_peeked_begin_rel_seq &&
                !prepark_saw_history_reset) {
                const auto prepark_namespace =
                    impl_->namespaces.find(hello.c_store_guid);
                Impl::Route* prepark_route = nullptr;
                if (prepark_namespace != impl_->namespaces.end()) {
                    const ProfileId prepark_profile =
                        require_single_route_profile(
                            selection.negotiated_profiles);
                    prepark_route = Impl::find_route(prepark_namespace->second,
                                                    prepark_profile);
                }
                if (prepark_route != nullptr && prepark_route->pending &&
                    prepark_route->pending->owner_serial != session.serial) {
                    // Level-triggered key: one past the current cursor. The
                    // head's commit bumps next_rel and releases exactly this
                    // key; any other resolution bulk-releases. prepark_key
                    // always names the rel_seq the CURRENT waiter is
                    // registered under, so every forget_parked below (loop
                    // exits and error paths alike) stays symmetric with its
                    // park() call even after a re-key.
                    RelSeq prepark_key{prepark_route->next_rel.value + 1};
                    std::shared_ptr<Impl::ParkedWaiter> prepark_waiter =
                        Impl::park(*prepark_route, prepark_key, io);
                    FrameParser prepark_drain_parser(
                        selection.limits.max_frame_payload);
                    std::size_t prepark_drained_total = 0;
                    std::size_t prepark_ceiling = 1u << 20;
                    bool prepark_ceiling_exceeded = false;
                    bool prepark_first_frame_done = false;
                    // Bounded grace: cap at the earlier of this run's own
                    // absolute operation deadline (if any) and 2000 ms. The
                    // absolute deadline is sampled on this process's
                    // CLOCK_MONOTONIC -- the same clock asio::steady_timer
                    // arms from -- so the conversion is exact; an invalid or
                    // missing deadline keeps the 2000 ms bound.
                    auto prepark_grace =
                        std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(2000);
                    if (deadline && deadline->valid()) {
                        const auto absolute =
                            deadline->as_steady_time_point();
                        if (absolute < prepark_grace)
                            prepark_grace = absolute;
                    }
                    asio::steady_timer prepark_timer(
                        socket.get_executor(), prepark_grace);
                    prepark_timer.async_wait(
                        [io](const boost::system::error_code&) {
                            // Grace expiry cancels the socket wait; the
                            // park loop below observes the wait completing
                            // without a release and falls through.
                            boost::system::error_code ignored;
                            io->socket.cancel(ignored);
                        });
                    for (;;) {
                        if (prepark_waiter->released)
                            break;
                        boost::system::error_code prepark_wait_error;
                        co_await socket.async_wait(
                            tcp::socket::wait_read,
                            asio::redirect_error(asio::use_awaitable,
                                                 prepark_wait_error));
                        require_operation();
                        if (prepark_waiter->released)
                            break;
                        if (prepark_wait_error) {
                            Impl::forget_parked(*prepark_route, prepark_key,
                                                prepark_waiter);
                            cancel_quietly(prepark_timer);
                            throw boost::system::system_error(
                                prepark_wait_error);
                        }
                        boost::system::error_code prepark_eof_error;
                        const std::size_t prepark_available_now =
                            socket.available(prepark_eof_error);
                        if (prepark_eof_error) {
                            Impl::forget_parked(*prepark_route, prepark_key,
                                                prepark_waiter);
                            cancel_quietly(prepark_timer);
                            throw boost::system::system_error(
                                prepark_eof_error);
                        }
                        if (prepark_available_now == 0) {
                            // Readable-but-empty is a graceful close while
                            // pre-parked: exactly the parked loop's EOF.
                            Impl::forget_parked(*prepark_route, prepark_key,
                                                prepark_waiter);
                            cancel_quietly(prepark_timer);
                            throw boost::system::system_error(
                                asio::error::eof);
                        }
                        std::vector<uint8_t> prepark_drain_buffer(
                            prepark_available_now);
                        boost::system::error_code prepark_recv_error;
                        const std::size_t prepark_drained_now = socket.receive(
                            asio::buffer(prepark_drain_buffer), 0,
                            prepark_recv_error);
                        if (prepark_recv_error) {
                            Impl::forget_parked(*prepark_route, prepark_key,
                                                prepark_waiter);
                            cancel_quietly(prepark_timer);
                            throw boost::system::system_error(
                                prepark_recv_error);
                        }
                        prepark_drained_total += prepark_drained_now;
                        for (Frame& prepark_frame : prepark_drain_parser.feed(
                                 std::span<const uint8_t>(
                                     prepark_drain_buffer.data(),
                                     prepark_drained_now)))
                            buffered_frames.push_back(
                                std::move(prepark_frame));
                        if (prepark_drained_total > prepark_ceiling) {
                            prepark_ceiling_exceeded = true;
                            break;
                        }
                        if (!prepark_first_frame_done) {
                            // Decide from the FIRST fully drained post-HELLO
                            // frame only.
                            if (buffered_frames.empty())
                                continue;
                            const Frame& prepark_first =
                                buffered_frames.front();
                            prepark_first_frame_done = true;
                            if (prepark_first.type ==
                                    MessageType::TX_BEGIN &&
                                decode_as<TxBegin>(prepark_first).rel_seq
                                        .value >
                                    prepark_route->next_rel.value) {
                                // A successor BEGIN ahead of the cursor:
                                // re-key the park onto its own rel_seq (the
                                // head's commit releases exactly that) and
                                // keep waiting, with that BEGIN's declared
                                // wire size as the drain ceiling -- exactly
                                // the parked path's bound arithmetic.
                                const TxBegin prepark_begin =
                                    decode_as<TxBegin>(prepark_first);
                                prepark_ceiling =
                                    static_cast<std::size_t>(
                                        prepark_begin.raw_bytes +
                                        prepark_begin.body.encoded_bytes) *
                                        2 +
                                    (1u << 20);
                                if (prepark_begin.rel_seq != prepark_key) {
                                    Impl::forget_parked(
                                        *prepark_route, prepark_key,
                                        prepark_waiter);
                                    prepark_key = prepark_begin.rel_seq;
                                    prepark_waiter =
                                        Impl::park(*prepark_route, prepark_key,
                                                   io);
                                }
                                continue;
                            }
                            // BEGIN at or behind the cursor, HISTORY_RESET,
                            // anything else: fall through immediately with
                            // the frame buffered; the unchanged code below
                            // handles it.
                            break;
                        }
                    }
                    Impl::forget_parked(*prepark_route, prepark_key,
                                        prepark_waiter);
                    cancel_quietly(prepark_timer);
                    if (prepark_ceiling_exceeded)
                        throw std::length_error(
                            "F endpoint pre-parked connection exceeded its "
                            "declared TX size");
                }
            }
        }
        // step 5c: serve already-drained frames (buffered while parked)
        // before falling through to the live socket. Mechanically the same
        // async_read_frame() every other profile/path on this connection
        // already uses -- just fed from a different source first.
        const auto next_frame = [&]() -> asio::awaitable<Frame> {
            if (buffered_frame_index < buffered_frames.size())
                co_return std::move(buffered_frames[buffered_frame_index++]);
            co_return co_await async_read_frame(
                socket, selection.limits.max_frame_payload,
                impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions, verify);
        };
        SessionState state = impl_->stage(session, hello, selection);
        co_await async_write_message(socket, state, selection.limits.max_frame_payload,
                                     impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                     impl_->completions, control, verify);

        Frame next = co_await next_frame();
        if (next.type == MessageType::HISTORY_RESET) {
            const HistoryReset reset = decode_as<HistoryReset>(next);
            impl_->validate_history_reset_candidate(session, reset);
            impl_->activate(session);
            impl_->reset_history(session, reset);
            SessionState reset_ack = impl_->session_state(session, selection);
            co_await async_write_message(socket, reset_ack, selection.limits.max_frame_payload,
                                         impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                         impl_->completions, control, verify);
            next = co_await next_frame();
            // Diagnostic-only: sampled immediately after the BEGIN frame read
            // completes, before decode_as<TxBegin>, so this is F's receipt
            // time for the BEGIN bytes and excludes prepare/activate/install
            // work below. See ICECC_P50_F_RESULT_TRACE and the NTP-offset
            // derivation in tools/stagetrace/analyze.py.
            {
                struct timespec wall_ts{};
                if (::clock_gettime(CLOCK_REALTIME, &wall_ts) == 0)
                    result.f_begin_recv_wall_ns =
                        static_cast<uint64_t>(wall_ts.tv_sec) * 1000000000ULL +
                        static_cast<uint64_t>(wall_ts.tv_nsec);
            }
            const TxBegin begin = decode_as<TxBegin>(next);
            impl_->begin(session, begin, selection.negotiated_profiles);
        } else {
            // Same placement rationale as above: this `next` is the frame
            // already read before the HISTORY_RESET check, and is the BEGIN
            // frame on this branch.
            {
                struct timespec wall_ts{};
                if (::clock_gettime(CLOCK_REALTIME, &wall_ts) == 0)
                    result.f_begin_recv_wall_ns =
                        static_cast<uint64_t>(wall_ts.tv_sec) * 1000000000ULL +
                        static_cast<uint64_t>(wall_ts.tv_nsec);
            }
            const TxBegin begin = decode_as<TxBegin>(next);
            Impl::PreparedBegin prepared = impl_->prepare_begin_candidate(
                session, begin, selection.negotiated_profiles);
            impl_->activate(session);
            impl_->install_begin(session, std::move(prepared));
        }
        f_begin_done = std::chrono::steady_clock::now();
        if (f_hello_done)
            result.f_hello_to_begin_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    *f_begin_done - *f_hello_done)
                    .count());

        do {
            Frame component = co_await next_frame();
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
        f_body_complete_done = std::chrono::steady_clock::now();
        if (f_begin_done)
            result.f_begin_to_body_complete_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    *f_body_complete_done - *f_begin_done)
                    .count());

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
        result.f_rel_seq = committed_begin.rel_seq;
        result.f_raw_bytes = committed_begin.raw_bytes;
        const auto commit_start = std::chrono::steady_clock::now();
        const TxCommit commit = impl_->commit_materialized(
            session, std::move(materialized), job_state,
            result.candidate_input,
            result.completed_input, result.committed_input);
        const auto commit_done = std::chrono::steady_clock::now();
        result.f_commit_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                commit_done - commit_start)
                .count());
        impl_->live_sessions.at(session.serial).committed = true;
        impl_->live_sessions.at(session.serial).final_commit = commit;
        co_await async_write_message(
            socket, commit, selection.limits.max_frame_payload,
            impl_->stamp(session, AsyncOperationKind::WriteFragment, &committed_begin),
            impl_->completions, control, verify);
        const auto commit_write_done = std::chrono::steady_clock::now();
        result.f_commit_write_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                commit_write_done - commit_done)
                .count());
        if (f_begin_done)
            result.f_begin_to_commit_written_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    commit_write_done - *f_begin_done)
                    .count());
        {
            struct timespec wall_ts{};
            if (::clock_gettime(CLOCK_REALTIME, &wall_ts) == 0)
                result.f_wall_clock_ns =
                    static_cast<uint64_t>(wall_ts.tv_sec) * 1000000000ULL +
                    static_cast<uint64_t>(wall_ts.tv_nsec);
        }
        const auto wait_peer_close_start = std::chrono::steady_clock::now();
        co_await async_wait_peer_close(
            socket, impl_->stamp(session, AsyncOperationKind::WaitPeerClose, &committed_begin),
            impl_->completions, verify);
        result.f_wait_peer_close_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - wait_peer_close_start)
                .count());
        impl_->disconnect(session, false);
        result.status = ServerRunStatus::Completed;
        close_now(socket);
        result.f_run_total_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - f_run_start)
                .count());
        co_return result;
    } catch (const StaleCompletion&) {
        close_now(socket);
        impl_->disconnect(session, true);
        result.status = ServerRunStatus::Disconnected;
        result.f_run_total_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - f_run_start)
                .count());
        co_return result;
    } catch (const boost::system::system_error&) {
        close_now(socket);
        impl_->disconnect(session, true);
        result.status = io->expired || deadline_crossed()
                            ? ServerRunStatus::DeadlineExceeded
                            : ServerRunStatus::Disconnected;
        result.f_run_total_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - f_run_start)
                .count());
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
        result.f_run_total_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - f_run_start)
                .count());
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
    result.f_run_total_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - f_run_start)
            .count());
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
                                      .activated = true};
            impl_->record(ActionType::SESSION_DISCONNECTED, invalidated);
        }
    }
    while (impl_->global_resources->live_namespace_count() != 0)
        impl_->global_resources->evict_oldest();
    impl_->live_sessions.clear();
    impl_->namespaces.clear();
    impl_->revisions.clear();
    {
        // reset_store drops every retained input in one call -- structurally
        // the same bulk-free risk collect_input_garbage() has, just triggered
        // by an F-store incarnation replacement instead of routine GC. Route
        // it through the same reaper rather than freeing on this owner-thread
        // call.
        InputBackingBatch graveyard;
        impl_->input_records.clear(&graveyard);
        impl_->submit_backing_graveyard(std::move(graveyard));
    }
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
    const auto start = std::chrono::steady_clock::now();
    std::map<CStoreGuid, std::vector<InputRecordKey>> before;
    for (const auto& [c_guid, space] : impl_->namespaces) {
        (void)space;
        before.emplace(c_guid, impl_->input_records.namespace_keys(c_guid));
    }
    InputBackingBatch graveyard;
    impl_->input_records.collect_garbage(&graveyard);
    impl_->release_collected_global_inputs(before);
    // Measured before the submit()/fallback below so it reflects what was
    // released either way, regardless of whether the reaper or this thread
    // ends up actually freeing it.
    const uint64_t freed_bytes = input_backing_batch_bytes(graveyard);
    const bool deferred = impl_->input_backing_reaper.submit(std::move(graveyard));
    if (!deferred) {
        // Reaper queue full or stopped: submit() left `graveyard` untouched,
        // so destroying it now frees the backings synchronously, right here
        // -- same as collect_garbage() behaved before the reaper existed.
        // Correctness over speed. Still inside the timed region below, so a
        // fallback free is visible in elapsed_ms/wall_clock_ns.
        graveyard.clear();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    if (elapsed_ms > 50) {
        const auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        std::fprintf(stderr,
                     "cache sidecar collect_input_garbage took %lldms"
                     " (retained_input_records=%zu) wall_clock_ns=%lld"
                     " freed_bytes=%llu deferred=%d\n",
                     static_cast<long long>(elapsed_ms),
                     impl_->input_records.record_count(),
                     static_cast<long long>(elapsed_ns),
                     static_cast<unsigned long long>(freed_bytes),
                     deferred ? 1 : 0);
    }
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
