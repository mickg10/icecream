#pragma once

// Protocol-50 cache sidecar service (S2 bounded local-control bridge).
//
// The service intentionally owns only its private AF_UNIX control listener.
// iceccd remains the public TCP listener owner and will hand clean-boundary
// cache-session descriptors to this sidecar.  No public listener, daemon, or
// advertisement code belongs here; SidecarRuntime delegates to the shared
// endpoint/store reducer.

#include <cstdint>
#include <chrono>
#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "p50_endpoint.h"
#include "p50_fd_handoff.h"
#include "p50_local_transport.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

namespace icecc::p50::service {

// Explicit state/configuration for the sidecar's one endpoint owner.  The
// service never creates a public listener: the daemon accepts the ordinary
// TCP link and passes that connected descriptor over the private control
// relationship.
struct RuntimeConfig {
    FStoreGuid f_store_guid{};
    EndpointCaps endpoint_caps{};
    P50ServerEndpointConfig endpoint_config{};
    size_t max_live_handoffs = 1;
};

enum class RuntimeStatus : uint8_t {
    Completed = 0,
    HandoffRejected,
    AdoptionFailed,
    Stopped,
    Busy,
    EndpointFailed,
};

struct RuntimeResult {
    RuntimeStatus status = RuntimeStatus::HandoffRejected;
    local::FdHandoffResult handoff{};
    std::optional<ServerRunResult> endpoint;
};

// Installed sidecars derive their empty-store identity from the complete
// authenticated incarnation.  Both fields are encoded into the 128-bit GUID,
// so a restart with the same generation but a new attempt cannot reopen the
// previous empty store by accident.
FStoreGuid f_store_guid_for_identity(local::Identity identity) noexcept;

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
                          EndpointIoControl endpoint_control = {});

    // Queue the mutable endpoint lookup on the endpoint owner's io_context.
    // The returned cursor owns its immutable backing and can be materialized
    // by the bounded control worker after this call returns.
    std::optional<InputCursor> attach_input_on_owner(
        InputRecordKey key, std::chrono::steady_clock::time_point deadline) noexcept;

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
        std::promise<EndpointOwnerResult> completion);
    void endpoint_owner_loop() noexcept;

    void cancel_active_socket() noexcept;
    void release_active_socket() noexcept;
    void cancel_active_control() noexcept;
    void release_active_control() noexcept;

    RuntimeConfig config_;
    boost::asio::io_context context_;
    std::unique_ptr<P50ServerEndpoint> endpoint_;
    EndpointWorkGuard endpoint_work_guard_;
    std::thread endpoint_owner_thread_;
    std::atomic<bool> endpoint_owner_failed_{false};
    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> stop_requested_{false};
    std::atomic<size_t> live_sessions_{0};
    std::atomic<int> active_cancel_fd_{-1};
    std::atomic<int> active_control_cancel_fd_{-1};
};

struct Options {
    std::string socket_path;
    local::Identity identity{};
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
