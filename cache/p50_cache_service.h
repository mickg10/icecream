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
#include <memory>
#include <mutex>
#include <optional>
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

namespace icecc::p50::service {

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
    // out of the production path.
    void start_adopted_endpoint(int adopted_fd,
                                EndpointIoControl endpoint_control = {}) noexcept;

    void stop() noexcept;
    [[nodiscard]] bool stopped() const noexcept { return stop_requested_.load(); }
    [[nodiscard]] size_t live_session_count() const;
    [[nodiscard]] size_t live_handoff_count() const noexcept {
        return busy_.test(std::memory_order_relaxed) ? 1u : 0u;
    }
    [[nodiscard]] FStoreGuid f_store_guid() const noexcept { return config_.f_store_guid; }

private:
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

    RuntimeConfig config_;
    InputLifecycleRegistry input_lifecycle_;
    boost::asio::io_context context_;
    std::unique_ptr<P50ServerEndpoint> endpoint_;
    std::unique_ptr<P50CRouteOwner> route_owner_;
    // P50CRouteOwner retains one sender per C/F/profile relationship and its
    // preparation authority permits only one uncommitted successor.  Source
    // requests arrive on independent bounded control workers, so serialize
    // them here under their unchanged absolute deadline before touching that
    // single-owner state.  This also bounds buffered source memory to one TU.
    std::timed_mutex source_transfer_mutex_;
    EndpointWorkGuard endpoint_work_guard_;
    std::thread endpoint_owner_thread_;
    std::atomic<bool> endpoint_owner_failed_{false};
    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> stop_requested_{false};
    std::atomic<size_t> live_sessions_{0};
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
