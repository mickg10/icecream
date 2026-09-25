#pragma once

// Protocol-50 cache sidecar service (S2 bounded local-control bridge).
//
// The service intentionally owns only its private AF_UNIX control listener.
// iceccd remains the public TCP listener owner and will hand clean-boundary
// cache-session descriptors to this sidecar.  No public listener, daemon, or
// advertisement code belongs here; SidecarRuntime delegates to the shared
// endpoint/store reducer.

#include <cstdint>
#include <array>
#include "p50_incarnation_identity.h"
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

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
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>

namespace icecc::p50::service {

struct PendingP51Transfer;
struct PendingP51Admission;

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
    size_t max_route_completed_requests = 4096;
    size_t max_route_relationships = 256;
    size_t max_route_endpoint_identities = 256;
    size_t max_pending_p51_source_reservations = 120;
    // Source work is admitted independently across C/F relationships.  Raw
    // bytes are reserved from the regular-file length before vector allocation.
    size_t max_active_source_transfers = 4;
    // P51 jobs retain raw/window credit independently of R1's opener count.
    // Control operations remain globally bounded by kMaxControlWorkers.  This
    // independently bounded queue includes accepted source jobs waiting for
    // preparation and reply settlement.
    size_t max_active_p51_source_transfers = 120;
    size_t max_pending_p51_source_operations = 120;
    uint64_t max_aggregate_source_raw_bytes = uint64_t{2} * 1024 * 1024 * 1024;
    // Connecting, negotiating, arming, and crossing CACHE_SESSION must never
    // monopolize the process-wide route-owner gate for the full source
    // operation deadline. F acknowledges an arm before doing source work;
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
    // Owner-thread witness for deadline/cancel retirement tests. Called only
    // after an exact reservation row and any matching endpoint tombstone have
    // been retired; production builds have no callback field.
    std::function<void(Id128, bool)> p51_reservation_retired_for_test;
    // Called on the owner executor when a P51 request is queued but cannot
    // currently acquire aggregate byte/count credit. Test-only admission
    // tests use this to synchronize a bounded fairness witness.
    std::function<void(uint64_t)> p51_source_credit_waiting_for_test;
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
    // Takes ownership of one authenticated kind-8 connection and source FD,
    // then returns immediately. File preparation runs on a bounded pool;
    // route work and the local reply/Goodbye exchange stay asynchronous on
    // the owner executor. The operation's original absolute deadline covers
    // queueing and every later phase.
    [[nodiscard]] bool enqueue_p51_source_transfer(
        local::Connection&& connection, local::Identity identity,
        local::ControlOperation operation,
        local::HandoffFd source) noexcept;

    // Reserve one P51 source job on the endpoint owner before the daemon
    // publishes ARMED. The reservation is bounded and idempotent for the
    // exact source request. It does not itself open a data link or claim a
    // CacheWire session.
    [[nodiscard]] local::P51SourceReservationResult reserve_p51_source_on_owner(
        local::P51SourceReservationRequest request) noexcept;
    [[nodiscard]] bool cancel_p51_source_on_owner(
        const P51SourceArmFields& arm,
        const std::array<uint8_t, 16>& reservation_id,
        std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] P51SourceLinkLookupResult
    lookup_p51_link_reservation_on_owner(const LinkHello& hello) noexcept;
    [[nodiscard]] std::optional<P51SourceJobLease>
    consume_p51_job_reservation_on_owner(const LinkHello& link,
                                         const JobBind& binding) noexcept;
    [[nodiscard]] bool authorize_p51_job_publication_on_owner(
        const LinkHello& link, const JobBind& binding) noexcept;
    void settle_p51_cancelled_job_on_owner(
        const LinkHello& link, const JobBind& binding) noexcept;
    [[nodiscard]] bool record_p51_job_commit_on_owner(
        const LinkHello& link, const JobBind& binding,
        const R2TxCommit& commit) noexcept;
    [[nodiscard]] bool acknowledge_p51_receipt_on_owner(
        const LinkHello& link, const CommitAck& ack) noexcept;
    [[nodiscard]] std::optional<P51RecoveryReceiptInterval>
    recover_p51_receipts_on_owner(
        const LinkHello& link, const RecoverBegin& begin,
        std::span<const RecoverWitness> witnesses,
        const RecoverEnd& end) noexcept;
    [[nodiscard]] bool settle_p51_interrupted_job_on_owner(
        const LinkHello& link) noexcept;
    [[nodiscard]] std::optional<ResetAck> validate_p51_reset_on_owner(
        const LinkHello& link, const ResetRequest& request) noexcept;
    [[nodiscard]] bool commit_p51_reset_on_owner(
        const LinkHello& link, const ResetRequest& request,
        const ResetAck& ack) noexcept;
    [[nodiscard]] bool confirm_p51_reset_on_owner(
        const LinkHello& link, const ResetConfirm& confirm) noexcept;
    [[nodiscard]] bool p51_source_reservation_terminal_on_owner(
        const JobBind& binding) noexcept;
    void schedule_p51_reservation_sweep_on_owner() noexcept;
    void sweep_p51_reservations_on_owner() noexcept;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    // Runs a short assertion/setup callback on the mutable endpoint owner.
    // Test code must not call *_on_owner methods from its own thread while
    // reservation expiry timers are active.
    void run_owner_callback_for_test(std::function<void()> callback);
    void notify_p51_reservation_retired_for_test(
        const Id128& id, bool endpoint_marker_retired) noexcept;
#endif
    void release_p51_link_on_owner(const LinkHello& hello) noexcept;

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
    void start_adopted_r2_endpoint(int adopted_fd,
                                   EndpointIoControl endpoint_control = {}) noexcept;

    void stop() noexcept;
    [[nodiscard]] bool stopped() const noexcept { return stop_requested_.load(); }
    [[nodiscard]] size_t live_session_count() const;
    [[nodiscard]] size_t live_handoff_count() const noexcept {
        return busy_.test(std::memory_order_relaxed) ? 1u : 0u;
    }
    [[nodiscard]] FStoreGuid f_store_guid() const noexcept { return config_.f_store_guid; }
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
    [[nodiscard]] size_t pending_p51_source_operations_for_test() const noexcept {
        return p51_source_operation_count_.load(std::memory_order_acquire);
    }
    [[nodiscard]] uint64_t active_source_raw_bytes_for_test() noexcept;
#endif

private:
    struct P51RawCredit;
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

    struct SourceIncarnationKey {
        FStoreGuid guid{};
        uint64_t generation = 0;
        auto operator<=>(const SourceIncarnationKey&) const = default;
    };

    struct EndpointOwnerResult {
        RuntimeStatus status = RuntimeStatus::EndpointFailed;
        std::optional<ServerRunResult> endpoint;
    };

    struct P51SourceRelationship {
        ProfileId profile = ProfileId::P29V1;
        std::array<uint8_t, 16> logical_id{};
        uint64_t c_store_generation = 0;
        uint64_t c_control_generation = 0;
        uint64_t c_control_attempt = 0;
        uint64_t epoch = 0;
        uint32_t selected_window = 0;
        uint64_t next_relationship_ordinal = 1;
        uint64_t pending_ordinal = 0;
        uint64_t pending_physical_link_generation = 0;
        Digest128 pending_binding_digest{};
        uint64_t committed_prefix_k = 0;
        uint64_t acknowledged_prefix_q = 0;
        size_t outstanding = 0;
        bool link_active = false;
        uint64_t physical_link_generation = 0;
        uint64_t highest_physical_link_generation = 0;
        bool has_receipts = false;
        P51SourceArmedFields anchor_armed{};
        Id128 anchor_reservation_id{};
        HistoryNonce history_nonce{};
        struct RecoveryContext {
            Id128 operation_id{};
            uint64_t physical_link_generation = 0;
            uint64_t relationship_epoch = 0;
            uint64_t verified_floor_a = 0;
            uint64_t prepared_prefix_p = 0;
            uint64_t committed_prefix_k = 0;
            uint64_t acknowledged_prefix_q = 0;
            Digest128 transcript_digest{};
        };
        std::optional<RecoveryContext> recovery_context;
        std::optional<ResetAck> last_reset_ack;
        bool last_reset_confirmed = false;
        std::array<std::optional<R2TxCommit>, 30> receipt_rows{};
    };

    struct P51SourceReservationRow {
        P51SourceArmedFields armed{};
        sidecar::AbsoluteMonotonicDeadline absolute_deadline{};
        bool consumed = false;
        uint64_t consumed_ordinal = 0;
        uint64_t consumed_physical_link_generation = 0;
        bool cancel_requested = false;
        bool publishing = false;
        std::optional<JobBind> consumed_binding;
    };

    using EndpointWorkGuard =
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

    boost::asio::awaitable<void> run_endpoint_on_owner(
        int adopted_fd, EndpointIoControl endpoint_control,
        std::promise<EndpointOwnerResult> completion, int completion_wake_fd,
        bool r2_link = false);
    void endpoint_owner_loop() noexcept;

    void cancel_endpoint_run() noexcept;
    void cancel_endpoint_incarnation() noexcept;
    void release_endpoint_run() noexcept;
    void cancel_active_control() noexcept;
    void close_active_control() noexcept;
    [[nodiscard]] bool bind_route_endpoint_identity(
        const RouteEndpointKey& endpoint,
        RouteStoreIdentity observed, bool p51 = false) noexcept;
    [[nodiscard]] bool acquire_source_address(
        const RouteEndpointKey& endpoint,
        std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] bool acquire_source_incarnation(
        const SourceIncarnationKey& incarnation,
        std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] bool acquire_source_incarnations(
        std::vector<SourceIncarnationKey> incarnations,
        std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] bool transition_source_incarnation(
        const SourceIncarnationKey& predecessor,
        const SourceIncarnationKey& successor,
        std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] bool acquire_source_credit(
        uint64_t raw_bytes,
        std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] bool try_acquire_p51_source_credit(
        uint64_t raw_bytes) noexcept;
    void release_p51_source_credit(uint64_t raw_bytes) noexcept;
    void queue_p51_source_admission(
        std::shared_ptr<PendingP51Transfer> pending,
        ProfileId profile, uint64_t raw_bytes) noexcept;
    void drain_p51_source_admissions() noexcept;
    void schedule_p51_source_admission_timer() noexcept;
    void prepare_p51_source_read(
        std::shared_ptr<PendingP51Transfer> pending, ProfileId profile,
        uint64_t raw_bytes,
        std::shared_ptr<P51RawCredit> raw_credit) noexcept;
    void start_p51_source_transfer_after_read(
        std::shared_ptr<PendingP51Transfer> pending, ProfileId profile,
        std::shared_ptr<const std::vector<uint8_t>> raw,
        std::shared_ptr<P51RawCredit> raw_credit) noexcept;
    void post_p51_source_transfer_reply(
        std::shared_ptr<PendingP51Transfer> pending,
        local::P50SourceTransferResult result) noexcept;
    [[nodiscard]] bool p51_source_peer_closed(
        const PendingP51Transfer& pending) const noexcept;
    void release_source_admission(
        const RouteEndpointKey& endpoint,
        const std::optional<SourceIncarnationKey>& incarnation,
        const std::optional<SourceIncarnationKey>& predecessor,
        const std::optional<P50RouteRelationship>& relationship,
        bool address_admitted, bool relationship_reserved,
        uint64_t raw_bytes, bool has_credit) noexcept;
    void release_source_credit(uint64_t raw_bytes) noexcept;
    void latch_route_replacement() noexcept;
    [[nodiscard]] bool owner_preflight_source_endpoint(
        const RouteEndpointKey& endpoint,
        std::optional<RouteStoreIdentity>& known_identity,
        bool& route_fatal,
        std::chrono::steady_clock::time_point deadline) noexcept;
    [[nodiscard]] bool owner_round_trip(
        std::function<void()> operation,
        std::chrono::steady_clock::time_point deadline,
        bool allow_during_stop = false) noexcept;

    RuntimeConfig config_;
    InputLifecycleRegistry input_lifecycle_;
    boost::asio::io_context context_;
    boost::asio::steady_timer p51_reservation_sweep_timer_{context_};
    boost::asio::steady_timer p51_source_admission_timer_{context_};
    boost::asio::thread_pool source_setup_pool_;
    boost::asio::thread_pool p51_source_prepare_pool_;
    std::atomic<size_t> p51_source_operation_count_{0};
    std::atomic<bool> p51_source_admission_wakeup_posted_{false};
    // Outstanding (queued + running) blocking retry setup tasks. Shared with
    // each task so a resolver that outlives its source deadline still holds
    // one bounded slot until its completion path actually returns.
    std::shared_ptr<std::atomic<size_t>> source_setup_inflight_;
    std::unique_ptr<P50ServerEndpoint> endpoint_;
    std::unique_ptr<P50CRouteOwner> route_owner_;
    // Detached R2 pumps may finish after their caller and request an
    // owner-affine retired-route reap. The weak lifetime token makes that
    // posted notification inert once SidecarRuntime begins destruction.
    std::shared_ptr<std::atomic<bool>> route_owner_callback_alive_ =
        std::make_shared<std::atomic<bool>>(true);
    // Endpoint address is the stable scheduler-facing relationship key.  Its
    // exact authenticated F incarnation is owner-affine and bounded; a change
    // retires every old-profile route before the successor is admitted.
    std::map<RouteEndpointKey, RouteStoreIdentity> route_endpoint_identities_;
    // Owner-affine reservations keep parallel novel addresses from all
    // passing endpoint-table preflight against the same final free slot.
    std::set<RouteEndpointKey> pending_route_endpoint_identities_;
    std::set<P50RouteRelationship> pending_route_relationships_;
    // Owner-affine tombstones prevent a stale endpoint alias from recreating
    // route history after its incarnation has been retired at another alias.
    std::set<SourceIncarnationKey> retired_source_incarnations_;
    // The bookkeeping mutex is short-held and never spans file I/O, setup,
    // CacheWire, or commit waiting. Admission leases themselves intentionally
    // remain held for the admitted operation.
    std::mutex source_admission_mutex_;
    std::condition_variable source_admission_changed_;
    std::set<RouteEndpointKey> active_source_addresses_;
    std::set<SourceIncarnationKey> active_source_incarnations_;
    std::set<SourceIncarnationKey> retiring_source_incarnations_;
    size_t active_source_count_ = 0;
    size_t active_p51_source_count_ = 0;
    uint64_t active_source_raw_bytes_ = 0;
    // This is the outermost opener fence.  P50CRouteOwner also retains its
    // own latch, but transfer_source_on_owner must refuse before it connects
    // to or arms any F after whole-sidecar replacement becomes necessary.
    std::atomic<bool> route_replacement_required_{false};
    // P51 reservation identity belongs to the same single endpoint owner as
    // namespaces/routes. The relationship survives codec-history resets;
    // only per-job reservation rows are retired.
    std::map<CStoreGuid, P51SourceRelationship> p51_source_relationships_;
    std::map<std::array<uint8_t, 16>, P51SourceReservationRow>
        p51_source_reservations_;
    uint64_t next_p51_relationship_epoch_ = 1;
    uint64_t next_p51_arm_observation_ = 1;
    std::shared_ptr<std::atomic<bool>> source_setup_cancelled_ =
        std::make_shared<std::atomic<bool>>(false);
    EndpointWorkGuard endpoint_work_guard_;
    std::thread endpoint_owner_thread_;
    std::atomic<bool> endpoint_owner_failed_{false};
    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> stop_requested_{false};
    std::atomic<int64_t> stop_requested_at_ns_{0};
    std::atomic<size_t> live_sessions_{0};
    std::atomic<int> active_control_cancel_fd_{-1};
    mutable std::mutex endpoint_cancel_mutex_;
    std::optional<EndpointCancelPermit> endpoint_cancel_permit_;

    // --- dedicated F-session control connections (owner-affine) -----------
    struct FSessionPump;
    struct P51TransferReplyPump;
    std::deque<std::shared_ptr<PendingP51Admission>>
        pending_p51_source_admissions_;
    void fsession_arm_read(std::shared_ptr<FSessionPump> pump) noexcept;
    void fsession_drain(std::shared_ptr<FSessionPump> pump) noexcept;
    void fsession_close(std::shared_ptr<FSessionPump> pump) noexcept;
    void start_p51_transfer_reply(
        local::Connection connection, local::Identity identity,
        local::ControlOperation operation,
        local::P50SourceTransferResult result,
        std::function<void()> settled) noexcept;
    void advance_p51_transfer_reply(
        std::shared_ptr<P51TransferReplyPump> pump) noexcept;
    void close_p51_transfer_reply(
        std::shared_ptr<P51TransferReplyPump> pump) noexcept;
    // Accessed only on the endpoint owner executor.
    fsession::FSessionServiceOwner fsession_owner_{4};
    std::vector<std::shared_ptr<FSessionPump>> fsession_pumps_;
    std::vector<std::shared_ptr<P51TransferReplyPump>> p51_transfer_reply_pumps_;
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
