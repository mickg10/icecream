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
#include <algorithm>
#include <chrono>
#include <atomic>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "p50_endpoint.h"
#include "p50_armed_admission.h"
#include "p50_control_operation.h"
#include "p50_fsession_service_owner.h"
#include "p50_fd_handoff.h"
#include "p50_input_fd_attachment.h"
#include "p50_input_lifecycle.h"
#include "p50_local_transport.h"
#include "p50_owner_watchdog.h"
#include "../client/p50_route_owner.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>

namespace icecc::p50::service {

namespace diag = ::icecc::p50::diag;

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
    // Fired, in program order, immediately before the F-independent source
    // read (read_source_fd) runs and immediately after armed-slot admission
    // is granted -- regardless of whether ICECC_P50_PREP_AHEAD moved the read
    // ahead of admission. Test builds use these two seams to prove the
    // PREP_AHEAD reordering by recording call order; production has no field.
    std::function<void()> before_source_read_for_test;
    std::function<void()> after_admission_for_test;
    // Test-only seam: when nonzero, apply_input_lifecycle_on_owner() sleeps
    // for this long on the owner executor before applying the lifecycle
    // action, so tests can exercise the sidecar-local
    // sidecar_lifecycle_apply_budget deadline (see Options below) against a
    // deliberately slow owner without needing a real contended F. Production
    // callers leave this at its default (zero: no stall).
    std::chrono::milliseconds input_lifecycle_owner_apply_stall_for_test{0};
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
    // Exposes RouteGate::route_pipeline_depth for direct unit testing
    // without a full transfer_source_on_owner() call: it is a pure function
    // of environment variables and the raw cache_profile bitmask, reading
    // and mutating no runtime state.
    [[nodiscard]] static uint32_t route_pipeline_depth_for_test(
        uint32_t cache_profile) noexcept {
        return RouteGate::route_pipeline_depth(cache_profile);
    }
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
    // Process-lifetime K (ICECC_P50_ROUTE_PIPELINE_DEPTH, clamped), read once
    // at construction. The per-route transfer lock is released early at
    // write-complete only when this is above 1.
    uint32_t route_pipeline_depth_ = 1;
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
    // eight to keep its gate busy; three capped it near 18 inputs/s.  The
    // window is taken in ArmedAdmission's order, largest sources first within
    // a bounded head start.
    struct RouteGate {
        static constexpr std::ptrdiff_t kArmedSessions = 8;
        // ICECC_P50_ARMED_WINDOW overrides the window size below.  Unset,
        // empty, non-numeric, or non-positive keeps kArmedSessions.  F's
        // live-session table (P50ServerOwnerLimits::max_live_sessions,
        // cache/p50_endpoint.h) is shared by every route an F endpoint
        // serves, so a window set much larger than that table's capacity
        // divided by the number of routes concurrently armed against the
        // same F buys nothing but BUSY (kFSessionBusy) elsewhere; this does
        // not change F's behaviour.
        static std::ptrdiff_t armed_window_slots() noexcept;
        // ICECC_P50_ROUTE_LANES overrides the lane count below.  Unset,
        // empty, non-numeric, or non-positive keeps 1 (byte-identical to no
        // lanes at all).  A request above what max_live_sessions can hold
        // alongside the window is clamped down to the largest value that
        // fits lanes * armed_window_slots() within max_live_sessions, with a
        // P50_ROUTE_LANES_CLAMPED warning on stderr.
        static uint32_t route_lane_count(size_t max_live_sessions) noexcept;

        // ICECC_P50_ROUTE_PIPELINE_DEPTH overrides the per-route outstanding
        // TX depth (K) below. Unset, empty, non-numeric, or non-positive
        // keeps 1 -- today's single-successor behaviour, byte-identical. A
        // request above 1 is further bounded by: (a) armed_window_slots(),
        // since every outstanding TX still needs its own live, armed F
        // session drawn from the same lane window; (b) kMaxPipelineDepth, a
        // hard ceiling regardless of (a); (c) the route's raw cache_profile
        // bitmask -- only CACHE_PROFILE_P29V1 is in scope for pipelining
        // (see research/designs/P50_ROUTE_PIPELINING.md); every other
        // profile is unconditionally forced back to 1. Each reduction is
        // reported once on stderr as P50_ROUTE_PIPELINE_DEPTH_CLAMPED,
        // mirroring route_lane_count's P50_ROUTE_LANES_CLAMPED above. A
        // depth above 1 also forces this route's lane count to 1 at the
        // transfer_source_on_owner call site: lanes and pipelining would
        // otherwise both multiply F's armed-session budget for the same
        // route, and combining them is out of scope (see the design doc's
        // settled decisions).
        //
        // SAFETY GATE (temporary, pre-F-sequencer): the F-side per-route
        // sequencer that lets F actually admit a parked successor (design
        // doc, step 5) does not exist yet. Until it lands, a request above
        // 1 unconditionally collapses back to 1 unless
        // ICECC_P50_ROUTE_PIPELINE_DEPTH_TEST_UNSAFE is *also* set to a
        // nonempty value other than "0" -- that escape is for this step's
        // own tests, to exercise the C-side plumbing ahead of F's half
        // landing, and must never be set outside a test process.
        static constexpr uint32_t kMaxPipelineDepth = 4;
        static uint32_t route_pipeline_depth(uint32_t cache_profile) noexcept;

        // One lane's armed-window admission and transfer serialization. Each
        // lane presents its own derived C-store GUID to F (see
        // derive_lane_c_store_guid in p50_cache_service.cpp), so F treats
        // every lane as an independent session/namespace with its own "one
        // uncommitted successor" slot -- the mechanism that lets N lanes'
        // CacheWire exchanges to the same F endpoint overlap.
        struct Lane {
            ArmedAdmission armed{armed_window_slots()};
            std::timed_mutex transfer;
        };

        explicit RouteGate(uint32_t lane_count = 1)
        {
            const uint32_t count = std::max<uint32_t>(1, lane_count);
            lanes.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
                lanes.push_back(std::make_unique<Lane>());
        }

        // Picks the least-loaded lane (most free armed-window slots); ties
        // and an all-full window fall back to round robin. With one lane
        // (the default), always and only selects lane 0.
        [[nodiscard]] size_t select_lane() noexcept
        {
            if (lanes.size() == 1)
                return 0;
            size_t best = 0;
            std::ptrdiff_t best_free = lanes[0]->armed.free_slots();
            for (size_t i = 1; i < lanes.size(); ++i) {
                const std::ptrdiff_t free = lanes[i]->armed.free_slots();
                if (free > best_free) {
                    best_free = free;
                    best = i;
                }
            }
            if (best_free > 0)
                return best;
            return next_lane.fetch_add(1, std::memory_order_relaxed) % lanes.size();
        }

        std::vector<std::unique_ptr<Lane>> lanes;
        std::atomic<size_t> next_lane{0};
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

    // --- owner-lag diagnostic probe (ICECC_P50_OWNER_LAG_TRACE) -----------
    // Entirely diagnostic and shared by both the C and F sidecar roles,
    // since both are the same SidecarRuntime. Disabled unless the env var
    // names a file; when disabled, arm_owner_lag_timer() is never posted (no
    // timer ever runs) and the per-op bookkeeping in attach_input_on_owner /
    // finish_input_attachment_on_owner / apply_input_lifecycle_on_owner is
    // skipped, so there is no behavior, timing, or memory-growth change on
    // the production path. See append_owner_lag_second_trace and
    // append_owner_lag_op_trace in p50_cache_service.cpp for the two emitted
    // JSONL schemas: a once-per-second lag/CPU aggregate from the timer, and
    // one record per lifecycle op from the three functions above.
    const bool owner_lag_trace_enabled_;
    boost::asio::steady_timer owner_lag_timer_{context_};
    std::chrono::steady_clock::time_point owner_lag_timer_scheduled_{};
    std::chrono::steady_clock::time_point owner_lag_second_started_{};
    uint64_t owner_lag_second_n_ = 0;
    int64_t owner_lag_second_max_ns_ = 0;
    std::vector<int64_t> owner_lag_second_samples_;
    int64_t owner_lag_prev_cpu_ns_ = 0;
    // Self-validation: the probe must not become the stall it measures.
    // Every append_owner_lag_op_trace / append_owner_lag_second_trace call
    // on the owner thread times its own open+write+close and folds the
    // duration in here; the per-second flush reports (and resets) these two
    // accumulators, so a slow trace write shows up in trace_write_max_ns /
    // trace_write_total_ns on the *next* line (the in-flight flush's own
    // write duration is folded into the following second's window rather
    // than its own, to avoid timing a write before it has happened).
    int64_t owner_lag_trace_write_max_ns_ = 0;
    int64_t owner_lag_trace_write_total_ns_ = 0;
    void arm_owner_lag_timer() noexcept;
    // Local proxy for "this retained input has an in-flight attachment right
    // now": present between a successful attach_input_on_owner and its
    // matching finish_input_attachment_on_owner call for the same key.
    // Diagnostic only -- mutated and read exclusively inside context_-posted
    // lambdas, all of which run on endpoint_owner_thread_, so no additional
    // lock is needed. Only ever populated when owner_lag_trace_enabled_.
    std::unordered_map<InputRecordKey, std::chrono::steady_clock::time_point,
                        InputRecordKeyHash>
        owner_lag_tx_inflight_;

    // --- owner-stall watchdog (ICECC_P50_OWNER_WATCHDOG_MS) ----------------
    // Entirely diagnostic (see p50_owner_watchdog.h for the full design and
    // motivation); disabled unless the env var names a threshold in
    // [100, 10000] ms. When disabled, owner_watchdog_ stays null,
    // arm_owner_heartbeat_timer() is never posted, and endpoint_owner_loop()
    // skips the heartbeat bump/tid publish/sigaltstack install below -- no
    // behavior, timing, or memory-growth change on the production path.
    const diag::OwnerWatchdogThresholdResult owner_watchdog_threshold_;
    std::atomic<int64_t> owner_heartbeat_ns_{0};
    std::atomic<int> owner_tid_{0};
    boost::asio::steady_timer owner_heartbeat_timer_{context_};
    void arm_owner_heartbeat_timer() noexcept;
    // Constructed only when owner_watchdog_threshold_.enabled; destroyed
    // first in ~SidecarRuntime (before stop()/context_ teardown) since it
    // reads owner_heartbeat_ns_/owner_tid_, which stop being updated once
    // the owner thread exits.
    std::unique_ptr<diag::OwnerWatchdog> owner_watchdog_;

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
    // Which backend this sidecar's InputFdAttachmentService should use to
    // materialize compiler input snapshots. Defaults to Native, so a launch
    // that omits --input-backend keeps today's exact sealed-memfd behavior.
    // The daemon is the only decision-maker: it resolves its platform-compat
    // module's input mitigation exactly once at its own startup and passes
    // the result down via --input-backend; this sidecar never reads
    // ICECC_P50_PLATFORM* or re-probes anything itself. See
    // doc/p50-platform-compat.md, "M2 input artifact".
    InputMaterializationBackend input_backend = InputMaterializationBackend::Native;
    // Sidecar-local budget for the owner-apply + reply + ack sequence of
    // non-retirement InputLifecycle actions (e.g. CloseAcceptedJob), measured
    // from the moment handle_connection decodes the request -- NOT the
    // pre-decode kHandshakeMilliseconds=500ms handshake deadline, which is
    // too tight once decode has already consumed part of it and which has no
    // relation to the daemon's own budget for these ops
    // (config_.input_lifecycle_timeout, default 5000ms,
    // daemon/p50_lifecycle_timeout_override.h). Retirement actions
    // (PrepareAttemptRetirement/CommitAttemptReplacement/
    // CloseLogicalInputLease) are unaffected: they keep using the daemon's
    // own operation.absolute_deadline, which is already sent down for them.
    // Resolved exactly once at process startup in run() from
    // ICECC_P50_SIDECAR_LIFECYCLE_APPLY_MS (default 5000ms, matching the
    // daemon's own default); see p50_sidecar_lifecycle_apply_override.h.
    std::chrono::milliseconds sidecar_lifecycle_apply_budget{5000};
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
