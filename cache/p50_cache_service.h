#pragma once

// Protocol-50 cache sidecar service (S2 bounded local-control bridge).
//
// The service intentionally owns only its private AF_UNIX control listener.
// iceccd remains the public TCP listener owner and will hand clean-boundary
// cache-session descriptors to this sidecar.  No public listener, daemon, or
// advertisement code belongs here; SidecarRuntime delegates to the shared
// endpoint/store reducer.

#include <cstdint>
#include "p50_incarnation_identity.h"
#include <chrono>
#include <atomic>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>

#include "p50_endpoint.h"
#include "p50_control_operation.h"
#include "p50_fsession_service_owner.h"
#include "p50_fd_handoff.h"
#include "p50_input_fd_attachment.h"
#include "p50_input_lifecycle.h"
#include "p50_local_transport.h"
#include "../client/p50_route_owner.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

namespace icecc::p50::service {

// Unset is the production default. Any configured value other than the one
// deliberately supported live-farm fault is malformed and must prevent READY.
[[nodiscard]] bool parse_p29_interner_fault_injection(
    const char* value, P29InternerFaultInjection& result) noexcept;

// Explicit state/configuration for the sidecar's one endpoint owner.  The
// service never creates a public listener: the daemon accepts the ordinary
// TCP link and passes that connected descriptor over the private control
// relationship.
struct RuntimeConfig {
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    // Allocator-issued F-store generation paired with f_store_guid.  Legacy
    // standalone fixtures may leave it zero; v4 retirement requests then
    // fail closed rather than inventing a default generation.
    uint64_t f_store_generation = 0;
    EndpointCaps endpoint_caps{};
    P50ServerEndpointConfig endpoint_config{};
    std::optional<SidecarLaunchIdentity> sidecar_launch;
    size_t max_live_handoffs = 1;
    size_t max_input_lifecycle_replays = 8192;
    size_t max_route_completed_requests = 65536;
    size_t max_route_relationships = 256;
    size_t max_route_endpoint_identities = 256;
    // Connecting, negotiating, arming, and crossing CACHE_SESSION happen
    // before the route gate but must not consume the full source operation
    // deadline. F acknowledges an arm before doing source work;
    // five seconds covers bounded connect retransmission and scheduling while
    // leaving the unchanged outer deadline for queued and CacheWire work.
    std::chrono::milliseconds source_open_arm_timeout{5000};
    P29InternerFaultInjection p29_interner_fault_injection =
        P29InternerFaultInjection::Disabled;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    // Injected at the real route prepare boundary after F has acknowledged
    // arm/CacheSession.  Test builds use this to prove owner poison reaches
    // SidecarRuntime's process-wide pre-open fence; production has no field.
    std::function<void()> before_route_prepare_for_test;
#endif
    std::chrono::milliseconds cancellation_grace{100};
    // Test/supervision seam: an injected owner failure is handled exactly like
    // an unexpected exception escaping the endpoint executor.  Production
    // callers leave this unset.
    std::function<void()> owner_failure;
    // Test-only seam: when set, the callback is posted after the endpoint has
    // published one live session and yielded into its adopted dialogue.  A
    // throwing callback escapes the owner executor (rather than the guarded
    // endpoint coroutine), proving the supervised fail-stop while a live
    // coroutine still owns endpoint state.  Production callers leave unset.
    std::function<void()> owner_failure_after_live;
    // Called on the control worker when the endpoint owner has not quiesced
    // by the original cumulative sidecar deadline.  Returning is not safe:
    // the owner coroutine may still reference this runtime, so run_one()
    // invokes the injectable policy and then unconditionally _Exit()s.
    std::function<void()> fail_stop;
};

enum class RuntimeStatus : uint8_t {
    Completed = 0,
    HandoffRejected,
    AdoptionFailed,
    Stopped,
    Busy,
    EndpointFailed,
    Cancelled,
};

enum class RuntimeCancellationReason : uint8_t {
    None = 0,
    Requested,
    ControlEof,
    Deadline,
    Stopped,
    Malformed,
};

struct RuntimeResult {
    RuntimeStatus status = RuntimeStatus::HandoffRejected;
    local::FdHandoffResult handoff{};
    std::optional<ServerRunResult> endpoint;
    RuntimeCancellationReason cancellation = RuntimeCancellationReason::None;
};

// One owner/reader for one authenticated control connection and one adopted
// endpoint session.  The endpoint object and its mutable store remain on the
// caller's io_context thread; this class adds no reducer or public listener.
class SidecarRuntime {
public:
    explicit SidecarRuntime(RuntimeConfig config);
    ~SidecarRuntime();
    SidecarRuntime(const SidecarRuntime&) = delete;
    SidecarRuntime& operator=(const SidecarRuntime&) = delete;

    RuntimeResult run_one(local::Connection& control, const local::HandoffRequest& expected,
                          std::chrono::steady_clock::time_point deadline,
                          EndpointIoControl endpoint_control = {},
                          // The all-zero placeholder is intentionally not an
                          // S2 claim: exact nonzero session binding and
                          // daemon OP_CANCEL/C_SOURCE emission remain HOLD.
                          local::ControlBindingPlaceholder binding_placeholder = {});

    // Queue the mutable endpoint lookup on the endpoint owner's io_context.
    // The returned cursor owns its immutable backing and can be materialized
    // by the bounded control worker after this call returns.
    std::optional<InputCursor> attach_input_on_owner(
        InputFdRequest request,
        std::chrono::steady_clock::time_point deadline) noexcept;
    void finish_input_attachment_on_owner(
        InputFdRequest request, bool authorized,
        std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] std::optional<InputLifecycleApplyStatus>
    apply_input_lifecycle_on_owner(
        InputLifecycleRequest request,
        std::chrono::steady_clock::time_point deadline) noexcept;

    // Consume one admitted source descriptor on the bounded control worker;
    // the persistent route owner and mutable sender state stay on context_.
    [[nodiscard]] local::P50SourceTransferResult transfer_source_on_owner(
        local::P50SourceTransferRequest request,
        sidecar::AbsoluteMonotonicDeadline deadline,
        local::HandoffFd source) noexcept;

    // Route an authenticated dedicated F-session control connection (first
    // post-handshake bytes carry the P5FS envelope magic) onto the endpoint
    // owner executor. Takes ownership of connection_fd. No per-connection
    // thread: the owner-affine FSessionServiceOwner drives it incrementally.
    void route_fsession_connection(int connection_fd) noexcept;
    [[nodiscard]] size_t live_fsession_operations() const noexcept;

    // Start one already-authenticated public CacheWire socket on the endpoint
    // owner.  The control worker transfers ownership here after the
    // CacheSession handoff is ACKed; the historical run_one() worker remains
    // out of the production path.  Each start consumes one session
    // reservation, released when the endpoint's run ends: the control worker
    // reserves before READY and answers BUSY when none is left, so F never
    // refuses a session after READY.
    [[nodiscard]] bool try_reserve_session() noexcept;
    void release_session_reservation() noexcept;
    void start_adopted_endpoint(int adopted_fd,
                                EndpointIoControl endpoint_control = {}) noexcept;

    void stop() noexcept;
    [[nodiscard]] bool stopped() const noexcept { return stop_requested_.load(); }
    [[nodiscard]] size_t live_session_count() const;
    [[nodiscard]] size_t live_handoff_count() const noexcept {
        return busy_.test(std::memory_order_relaxed) ? 1u : 0u;
    }
    [[nodiscard]] FStoreGuid f_store_guid() const noexcept { return config_.f_store_guid; }
    [[nodiscard]] uint64_t max_raw_bytes() const noexcept {
        return config_.endpoint_caps.zstd.max_raw_bytes;
    }
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    // Seeds the bounded endpoint map without opening an F connection.  Test
    // builds use this only while no source transfer is active.
    [[nodiscard]] bool seed_route_endpoint_identity_for_test(
        std::string host, uint32_t cache_port, FStoreGuid guid,
        uint64_t generation) noexcept;
    // Seeds both the authenticated endpoint binding and one retained profile
    // relationship without opening F.  Tests use this to prove that a known
    // endpoint/new-profile capacity refusal occurs before any network work.
    [[nodiscard]] bool seed_route_relationship_for_test(
        std::string host, uint32_t cache_port, FStoreGuid guid,
        uint64_t generation, ProfileId profile) noexcept;
#endif

private:
    struct RouteEndpointKey {
        std::string host;
        uint32_t cache_port = 0;
        auto operator<=>(const RouteEndpointKey&) const = default;
    };

    struct RouteStoreIdentity {
        FStoreGuid guid{};
        uint64_t generation = 0;
        auto operator<=>(const RouteStoreIdentity&) const = default;
    };

    struct EndpointOwnerResult {
        RuntimeStatus status = RuntimeStatus::EndpointFailed;
        std::optional<ServerRunResult> endpoint;
    };

    using EndpointWorkGuard =
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

    boost::asio::awaitable<void> run_endpoint_on_owner(
        int adopted_fd, EndpointIoControl endpoint_control,
        std::promise<EndpointOwnerResult> completion, int completion_wake_fd);
    void endpoint_owner_loop() noexcept;

    void cancel_endpoint_run() noexcept;
    void cancel_endpoint_incarnation() noexcept;
    void release_endpoint_run() noexcept;
    void cancel_active_control() noexcept;
    void close_active_control() noexcept;
    [[nodiscard]] bool bind_route_endpoint_identity(
        const RouteEndpointKey& endpoint,
        RouteStoreIdentity observed) noexcept;

    RuntimeConfig config_;
    InputLifecycleRegistry input_lifecycle_;
    boost::asio::io_context context_;
    std::unique_ptr<P50ServerEndpoint> endpoint_;
    std::unique_ptr<P50CRouteOwner> route_owner_;
    // route_owner_'s authority, fixed before the owner thread starts so
    // control workers can prepare sources on their own threads.
    std::shared_ptr<P50PreparationAuthority> source_authority_;
    // Endpoint address is the stable scheduler-facing relationship key.  Its
    // exact authenticated F incarnation is owner-affine and bounded; a change
    // retires every old-profile route before the successor is admitted.
    // Guarded by source_route_mutex_.
    std::map<RouteEndpointKey, RouteStoreIdentity> route_endpoint_identities_;
    // P50CRouteOwner retains one sender per C/F/profile relationship and each
    // route permits only one uncommitted successor, so uploads to one F
    // endpoint are serialized by its transfer gate under their unchanged
    // absolute deadline.  A few more may connect and arm F ahead of the gate;
    // the bound keeps idle armed sessions off F's shared session table.
    // Uploads to different F overlap on the owner executor.  Buffered source
    // memory is at most kArmedSessions TUs per F endpoint.  Arming costs four
    // round trips before the gate, so a far F (~18 ms) needs a window of about
    // eight to keep its gate busy; three capped it near 18 inputs/s.
    struct RouteGate {
        static constexpr std::ptrdiff_t kArmedSessions = 8;
        std::counting_semaphore<kArmedSessions> armed{kArmedSessions};
        std::timed_mutex transfer;
    };
    std::mutex source_route_mutex_;
    std::map<RouteEndpointKey, std::shared_ptr<RouteGate>> source_route_gates_;
    std::atomic<size_t> active_source_transfers_{0};
    // This is the outermost opener fence.  P50CRouteOwner also retains its
    // own latch, but transfer_source_on_owner must refuse before it connects
    // to or arms any F after whole-sidecar replacement becomes necessary.
    std::atomic<bool> route_replacement_required_{false};
    EndpointWorkGuard endpoint_work_guard_;
    std::thread endpoint_owner_thread_;
    std::atomic<bool> endpoint_owner_failed_{false};
    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> stop_requested_{false};
    std::atomic<size_t> live_sessions_{0};
    // Adopted endpoints started or about to be (READY sent); bounded by the
    // endpoint's max_live_sessions.
    std::atomic<size_t> session_reservations_{0};
    std::atomic<int> active_control_cancel_fd_{-1};
    mutable std::mutex endpoint_cancel_mutex_;
    std::optional<EndpointCancelPermit> endpoint_cancel_permit_;

    // --- dedicated F-session control connections (owner-affine) -----------
    struct FSessionPump;
    void fsession_arm_read(std::shared_ptr<FSessionPump> pump) noexcept;
    void fsession_drain(std::shared_ptr<FSessionPump> pump) noexcept;
    void fsession_close(std::shared_ptr<FSessionPump> pump) noexcept;
    // Accessed only on the endpoint owner executor.
    fsession::FSessionServiceOwner fsession_owner_{4};
    std::vector<std::shared_ptr<FSessionPump>> fsession_pumps_;
    std::atomic<size_t> fsession_live_{0};
    // Blocking reopens (a transfer's second open, and reopens after BUSY) run
    // here, not on the owner: two at a time, with at most kReopenLimit queued
    // or running; one more fails at once.  Declared last so it joins first on
    // teardown.
    static constexpr size_t kReopenLimit = 8;
    std::shared_ptr<std::atomic<size_t>> reopen_outstanding_ =
        std::make_shared<std::atomic<size_t>>(0);
    boost::asio::thread_pool reopen_pool_{2};
};

struct Options {
    std::string socket_path;
    local::Identity identity{};
    uint64_t f_store_generation = 0;
    uint64_t store_derivation_version = 0;
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    local::CredentialExpectation expected_peer;
    std::optional<uint64_t> drop_uid;
    std::optional<uint64_t> drop_gid;
    int backlog = 1;
};

// Parses only the service's explicit options.  It never consults PATH,
// invokes a shell, or accepts an implicit/default socket or identity.
bool parse_options(int argc, char* const argv[], Options& options,
                   bool& show_help) noexcept;

// Runs the bounded service loop.  A zero return means clean stop; nonzero is
// a fail-closed startup or control error.  The function installs no process
// supervisor and does not alter daemon advertisement state.
int run(const Options& options) noexcept;

} // namespace icecc::p50::service
