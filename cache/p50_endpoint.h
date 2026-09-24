#pragma once

#include "p50_actions.h"
#include "p50_input_record.h"
#include "p50_profile.h"
#include "p50_sidecar_identity.h"
#include "p50_zstd.h"
#include "p50_slice0.h"
#include "p50_endpoint_run_cancel.h"
#include "services/p50_cache_session_wire.h"

#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace icecc::p50 {

class GlobalResourceTrace;

/* A C-side P29V1 authority failed while reserving/initializing its permanent
   interner.  This exception is intentionally narrower than length_error:
   callers may disable only P29V1 for the current supervised READY lease, while
   ordinary per-TU size errors and transport failures remain retryable. */
class P29V1CapabilityUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace sidecar {
class P5coEndpointHandoff;
}

struct EndpointCaps {
    SessionLimits wire{};
    ZstdTuLimits zstd{uint64_t{64} << 20, uint64_t{2} << 30};
    ProfileId profile = ProfileId::ZSTD_TU;
    // C selects one profile for its preparation authority; F advertises the
    // runnable source profiles it can negotiate on each session.  Keeping
    // these distinct permits the READY capability mask to remain truthful
    // while each route still has one concrete profile.
    uint32_t supported_profiles = kOperationalProfileMask;
    auto operator<=>(const EndpointCaps&) const = default;
};

/* One immutable prepared source. Revision 1 has exactly one BODY component;
   interactive P29V1 NEED/FILL state remains owned by its route codec. */
struct PreparedInputEnvelope {
    TxBegin begin;
    std::vector<uint8_t> body;
};
using PreparedInputPtr = std::shared_ptr<const PreparedInputEnvelope>;

enum class AsyncOperationKind : uint8_t {
    Accept,
    Connect,
    ReadHeader,
    ReadPayload,
    WriteFragment,
    WaitPeerClose,
};

std::string_view async_operation_name(AsyncOperationKind operation);

struct CompletionStamp {
    ActorSide actor = ActorSide::C;
    AsyncOperationKind operation = AsyncOperationKind::Connect;
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    std::optional<daemon::P50FSessionOperationId> cache_session_operation;
    std::optional<sidecar::AbsoluteMonotonicDeadline> absolute_deadline;
    uint64_t session_serial = 0;
    HistoryNonce history_nonce{};
    RelSeq rel_seq{};
    TuSeq tu_seq{};
    Digest128 transaction_digest{};
    Digest128 raw_digest{};
    bool transaction_bound = false;
    auto operator<=>(const CompletionStamp&) const = default;
};

// The endpoint identity derived from current product state after an asynchronous
// operation completes.  It intentionally excludes the operation kind: that is
// frozen in CompletionStamp and checked on the observed-completion path.
struct CompletionLiveIdentity {
    ActorSide actor = ActorSide::C;
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    std::optional<daemon::P50FSessionOperationId> cache_session_operation;
    std::optional<sidecar::AbsoluteMonotonicDeadline> absolute_deadline;
    uint64_t session_serial = 0;
    HistoryNonce history_nonce{};
    RelSeq rel_seq{};
    TuSeq tu_seq{};
    Digest128 transaction_digest{};
    Digest128 raw_digest{};
    bool transaction_bound = false;
    auto operator<=>(const CompletionLiveIdentity&) const = default;
};

struct AsyncCompletion {
    CompletionStamp stamp{};
    uint64_t transferred_bytes = 0;
    int error_value = 0;
    auto operator<=>(const AsyncCompletion&) const = default;
};

class CompletionLog {
public:
    explicit CompletionLog(size_t max_records = std::numeric_limits<size_t>::max())
        : max_records_(max_records) {}

    void record(AsyncCompletion completion) noexcept {
        if (!valid_)
            return;
        if (completions_.size() >= max_records_) {
            valid_ = false;
            return;
        }
        try {
            completions_.push_back(std::move(completion));
        } catch (...) {
            valid_ = false;
        }
    }
    [[nodiscard]] const std::vector<AsyncCompletion>& completions() const { return completions_; }
    [[nodiscard]] bool valid() const { return valid_; }
    void clear() {
        completions_.clear();
        valid_ = true;
    }

private:
    size_t max_records_ = std::numeric_limits<size_t>::max();
    bool valid_ = true;
    std::vector<AsyncCompletion> completions_;
};

// This control is a deterministic test seam around message boundaries and write
// fragmentation. Normal product callers use the default value.
struct EndpointIoControl {
    size_t max_write_fragment = std::numeric_limits<size_t>::max();
    std::optional<MessageType> close_before_write;
    std::optional<MessageType> close_after_write;
    std::optional<AsyncOperationKind> wrong_digest_completion;
    std::optional<AsyncOperationKind> wrong_raw_digest_completion;
    // Receives only the observed test copy; the endpoint retains and checks
    // its independently frozen pre-await completion stamp.
    std::function<void(CompletionStamp&)> before_completion_check;
    // Receives only a derived live-identity copy. Product endpoint state stays
    // private, and both endpoints use the same field-by-field validator.
    std::function<void(const CompletionStamp&, CompletionLiveIdentity&)>
        before_live_identity_check;
    // Test-only copied-begin transform: applied to a COPY of the outbound
    // TX_BEGIN after the client coroutine constructs it and before the
    // outbound-admission validation/send, so a test can drive the production
    // validator AND its production callsite with a wire-valid but
    // unnegotiated begin. The retained prepared record is never mutated.
    // Product callers leave it unset.
    std::function<TxBegin(const TxBegin&, std::span<const uint8_t>)>
        outbound_begin_transform;
    // Test-only cancellation linearization hook immediately before the first
    // CacheWire write becomes possible.  It receives no endpoint state;
    // product callers leave it unset.  This lets the deletion gate prove the
    // narrow no-remote-transmission cancellation outcome deterministically.
    std::function<void()> before_first_remote_write;
    // Test-only worker hook.  It executes after the immutable complete-BODY
    // materialization job leaves the endpoint owner and before codec work.
    // Product callers leave it unset; it proves the owner timer cannot be
    // starved by a blocked codec worker.
    std::function<void()> before_materialize_on_worker;
    // Simulator/test-only observation after one complete message has been
    // written successfully. It receives an immutable message copy and cannot
    // change product framing or endpoint state.
    std::function<void(ActorSide, const Message&)> outbound_message_observer;
};

struct PrepareRequestKey {
    uint64_t producer_session = 0;
    uint64_t request_token = 0;
    auto operator<=>(const PrepareRequestKey&) const = default;
};

// C-wide preparation is shared, but route-profile planning is relationship
// scoped.  The F generation fences a replacement route from its predecessor.
struct PreparationRouteKey {
    FStoreGuid f_store_guid{};
    uint64_t f_store_generation = 0;
    ProfileId profile = ProfileId::ZSTD_TU;
    auto operator<=>(const PreparationRouteKey&) const = default;
};

struct PreparationAuthorityLimits {
    size_t max_live_entries = 4096;
    uint64_t max_retained_encoded_bytes = uint64_t{512} << 20;
    // Only a gate: the interner allocates firefox() (2.32 GiB) if it fits
    // under this, else probe().  It never grows into the remainder.
    uint64_t max_interner_reserved_bytes = uint64_t{24} << 30;
    uint64_t max_route_state_bytes = uint64_t{1} << 30;
    auto operator<=>(const PreparationAuthorityLimits&) const = default;
};

class PreparedTuHandle {
public:
    PreparedTuHandle() = default;

    [[nodiscard]] explicit operator bool() const {
        return entry_id_ != 0 && !authority_.expired();
    }

    friend bool operator==(const PreparedTuHandle& left, const PreparedTuHandle& right) {
        const bool same_authority = !left.authority_.owner_before(right.authority_) &&
                                    !right.authority_.owner_before(left.authority_);
        return left.entry_id_ == right.entry_id_ && same_authority;
    }

private:
    PreparedTuHandle(std::weak_ptr<const void> authority, uint64_t entry_id)
        : authority_(std::move(authority)), entry_id_(entry_id) {}

    std::weak_ptr<const void> authority_;
    uint64_t entry_id_ = 0;

    friend class P50PreparationAuthority;
};

class P50PreparationAuthority {
public:
    explicit P50PreparationAuthority(
        CStoreGuid c_store_guid,
        ZstdTuLimits zstd_limits = {uint64_t{64} << 20, uint64_t{2} << 30},
                                     PreparationAuthorityLimits authority_limits = {},
                                     int compression_level = 1,
                                     ProfileId profile = ProfileId::ZSTD_TU,
                                     TuSeq first_tu_seq = {});
    P50PreparationAuthority(
        CStoreGuid c_store_guid, ZstdTuLimits zstd_limits,
        PreparationAuthorityLimits authority_limits, int compression_level,
        ProfileId profile, TuSeq first_tu_seq,
        P29InternerFaultInjection fault_injection);
    ~P50PreparationAuthority();
    P50PreparationAuthority(const P50PreparationAuthority&) = delete;
    P50PreparationAuthority& operator=(const P50PreparationAuthority&) = delete;

    PreparedTuHandle prepare(PrepareRequestKey request,
                             std::span<const uint8_t> exact_input);
    PreparedTuHandle prepare_for_route(PreparationRouteKey route,
                                       PrepareRequestKey request,
                                       std::span<const uint8_t> exact_input);
    // The route-independent half of a P29V1 prepare_for_route (TU_SEQ and
    // interning).  Unlike every other member it may run on any thread, so a
    // route's serialized dialogue on the owner need not wait for it.  The
    // owner installs the result; prepare_for_route then reuses it for the
    // same request, and release_source drops it if no route admitted it.
    class PreparedSource;
    std::shared_ptr<const PreparedSource> prepare_source(
        PrepareRequestKey request,
        std::shared_ptr<const std::vector<uint8_t>> exact_input,
        Digest128 raw_digest);
    void install_source(PrepareRequestKey request,
                        std::shared_ptr<const PreparedSource> prepared);
    void release_source(PrepareRequestKey request) noexcept;
    std::span<const uint8_t> answer_p29v1_need(
        PreparedTuHandle handle, std::span<const uint8_t> inner_need);
    [[nodiscard]] Digest128 p29v1_system_source_fingerprint(
        PreparedTuHandle handle) const;
    [[nodiscard]] std::optional<bool> p29v1_system_source_reuse(
        PreparedTuHandle handle) const;
    void pin_p29v1_system_source_reuse(PreparedTuHandle handle,
                                       Digest128 f_fingerprint);
    void restart_p29v1_transport_retry(PreparedTuHandle handle);
    PreparedInputPtr reset_p29v1_route(PreparedTuHandle handle,
                                       FStoreGuid f_store_guid,
                                       HistoryNonce history_nonce);
    uint64_t retain(PreparedTuHandle handle);
    uint64_t release(PreparedTuHandle handle);
    void commit(PreparedTuHandle handle);
    [[nodiscard]] CStoreGuid c_store_guid() const;
    [[nodiscard]] ZstdTuLimits zstd_limits() const;
    [[nodiscard]] bool contains(PreparedTuHandle handle) const;
    // Exposes the authenticated C-wide identity carried by a prepared route
    // view; all forks of one request must report the same value.
    [[nodiscard]] TuSeq prepared_tu_seq(PreparedTuHandle handle) const;
    [[nodiscard]] ProfileId prepared_profile(PreparedTuHandle handle) const;
    [[nodiscard]] size_t live_entry_count() const;
    [[nodiscard]] uint64_t retained_encoded_bytes() const;
    [[nodiscard]] uint64_t p29v1_interner_reserved_bytes() const;
    [[nodiscard]] uint64_t p29v1_interner_committed_bytes() const;
    [[nodiscard]] uint64_t p29v1_route_state_bytes(
        PreparationRouteKey route) const;
    [[nodiscard]] size_t route_history_bytes() const;
    [[nodiscard]] size_t route_history_bytes(PreparationRouteKey route) const;
    [[nodiscard]] Digest128 route_history_digest() const;
    [[nodiscard]] Digest128 route_history_digest(PreparationRouteKey route) const;
    [[nodiscard]] size_t route_history_entries() const;
    [[nodiscard]] size_t route_history_entries(PreparationRouteKey route) const;
    [[nodiscard]] ProfileId profile() const;

    // Route owners call this only after all handles for the relationship have
    // been released.  It drops matcher/history state without touching the
    // C-wide immutable catalogue or TU allocator.
    [[nodiscard]] bool reset_route(PreparationRouteKey route) noexcept;

private:
    PreparedInputPtr resolve(PreparedTuHandle handle) const;
    void validate_begin(const TxBegin& begin) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class P50ClientEndpoint;
};

enum class EndpointReconnectOutcome : uint8_t {
    ExactMatch,
    LostFinalAcknowledgement,
    ColdFStore,
    RouteHistoryReset,
    WrongAdoptedPeer,
};

enum class ClientRunStatus : uint8_t {
    Committed,
    Disconnected,
    DeadlineExceeded,
    TerminalError,
};

// Operation-scoped cancellation is intentionally distinct from an ordinary
// transport disconnect.  A caller may authorize a clean pre-durable abort
// only while the endpoint can prove that no CacheWire byte may have reached
// the peer and no previously active transaction is being reconciled.
enum class ClientCancellationDisposition : uint8_t {
    None,
    ReconcileRequired,
};

// A local endpoint observation never proves durability.  Only the owning
// FSession operation may later settle this observation as committed or
// pre-durable-aborted.
enum class ClientRunObservation : uint8_t {
    Unresolved,
    ExactCommitObserved,
    PeerTerminalFrame,
    ProtocolViolation,
    Disconnected,
    DeadlineExpired,
    Cancelled,
    WrongAdoptedPeer,
    ReconcileRequired,
    LocalFailure,
};

struct ClientRunResult {
    ClientRunStatus status = ClientRunStatus::Disconnected;
    EndpointReconnectOutcome reconnect = EndpointReconnectOutcome::ExactMatch;
    ClientRunObservation observation = ClientRunObservation::Unresolved;
    ClientCancellationDisposition cancellation =
        ClientCancellationDisposition::None;
    // These witnesses are populated only after the endpoint has accepted the
    // exact, fully validated TX_COMMIT.  They are deliberately independent of
    // ActionTrace (which is diagnostic, not an authority).
    std::optional<TxCommit> committed_commit;
    std::optional<InputRecordKey> committed_input;
    std::optional<ErrorMessage> terminal_error;
};

enum class ServerRunStatus : uint8_t {
    Completed,
    Disconnected,
    TerminalError,
    DeadlineExceeded,
};

struct ServerRunResult {
    ServerRunStatus status = ServerRunStatus::Disconnected;
    uint64_t session_serial = 0;
    // Codec work performed on F after receiving profile data: applying FILL
    // fragments plus exact materialization/verification. Queueing, network
    // waits, selector callbacks, and owner-side publication are excluded.
    uint64_t f_apply_materialize_ns = 0;
    std::optional<CStoreGuid> c_store_guid;
    // candidate_input is set before the job-state selector. completed_input is
    // set only after exact publication/closed-commit validation succeeds.
    // committed_input remains the narrower compiler-attachable witness.
    std::optional<InputRecordKey> candidate_input;
    std::optional<InputRecordKey> completed_input;
    std::optional<InputRecordKey> committed_input;
    std::optional<ErrorMessage> terminal_error;
};

enum class InputJobState : uint8_t {
    Open,
    Closed,
};

// Runs on the endpoint state owner after exact materialization and before
// InputRecord publication or route visibility. Throwing leaves the exact
// transaction identity available for replay.
using InputJobStateSelector = std::function<InputJobState(
    CStoreGuid, const TxBegin&, const TxCommit&, std::span<const uint8_t>)>;

struct P50ServerOwnerLimits {
    size_t max_live_sessions = 64;
    size_t max_namespaces = 4096;
    uint64_t max_pending_encoded_bytes = uint64_t{512} << 20;
    uint64_t max_pending_raw_bytes = uint64_t{8} << 30;
    uint64_t max_decoder_window_bytes = uint64_t{8} << 30;
    size_t max_retained_input_records = 4096;
    uint64_t max_retained_input_bytes = uint64_t{8} << 30;
    auto operator<=>(const P50ServerOwnerLimits&) const = default;
};

struct P50ServerOwnerUsage {
    size_t live_sessions = 0;
    size_t namespaces = 0;
    size_t revisions = 0;
    uint64_t pending_encoded_bytes = 0;
    uint64_t pending_raw_bytes = 0;
    uint64_t decoder_window_bytes = 0;
    size_t retained_input_records = 0;
    uint64_t retained_input_bytes = 0;
    auto operator<=>(const P50ServerOwnerUsage&) const = default;
};

struct P50ServerEndpointConfig {
    uint16_t protocol_error_code = 1;
    P50ServerOwnerLimits owner_limits{};
    InputJobStateSelector input_job_state;
    std::optional<SidecarLaunchIdentity> sidecar_launch;
    uint64_t endpoint_generation = 1;
    // Optional owner-visible trace for the global resource binding. The
    // endpoint never owns this sink; callers keep it alive for the endpoint.
    GlobalResourceTrace* global_resource_trace = nullptr;
    // Called on the endpoint owner immediately after an input record is
    // published and before TX_COMMIT is exposed to the peer.  Sidecar
    // lifecycle ownership must observe this edge before a daemon can issue
    // the corresponding attachment.
    std::function<void(InputRecordKey, bool)> on_input_committed;
    std::function<void(EndpointCancelPermit)> on_run_admitted;
    std::function<void(EndpointCancelPermit, EndpointTerminalResult)> on_run_terminal;
};

// Outbound-admission law shared by the client's production send path and its
// tests: a C endpoint must refuse to place a TX_BEGIN whose profile lies
// outside the session's negotiated mask on the wire. Pure over its inputs --
// callers pass a COPY of the outbound begin and the session's negotiated
// mask, so exercising the law mutates no retained endpoint state.
void require_outbound_profile_negotiated(uint32_t negotiated_profiles,
                                         const TxBegin& begin);

class P50ClientEndpoint {
public:
    explicit P50ClientEndpoint(std::shared_ptr<P50PreparationAuthority> preparation,
                               EndpointCaps caps = {},
                               HistoryNonce first_history_nonce = HistoryNonce{1},
                               CompletionLog* completions = nullptr,
                               ActionTrace* actions = nullptr,
                               std::optional<EndpointRunIdentity> run_identity_seed =
                                   std::nullopt,
                               std::function<void(EndpointCancelPermit)>
                                   on_run_admitted = {},
                               std::function<void(EndpointCancelPermit,
                                                  EndpointTerminalResult)>
                                   on_run_terminal = {},
                               std::optional<PreparationRouteKey> route =
                                   std::nullopt);
    ~P50ClientEndpoint();
    P50ClientEndpoint(const P50ClientEndpoint&) = delete;
    P50ClientEndpoint& operator=(const P50ClientEndpoint&) = delete;

    // The deadline is an absolute steady-clock time.  Omitting it preserves
    // the historical unbounded behavior for callers that do not opt in.
    boost::asio::awaitable<ClientRunResult> run(boost::asio::ip::tcp::endpoint remote,
                                                PreparedTuHandle prepared = {},
                                                EndpointIoControl control = {},
                                                std::optional<std::chrono::steady_clock::time_point>
                                                    deadline = std::nullopt);

    // Run over a socket that is already connected by the ordinary listener
    // negotiation path.  Ownership is consumed on every success and failure
    // path; the endpoint validates that it is a connected IPv4/IPv6 TCP socket.
    boost::asio::awaitable<ClientRunResult> run(
        boost::asio::ip::tcp::socket socket, PreparedTuHandle prepared = {},
        EndpointIoControl control = {},
        std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

    boost::asio::awaitable<ClientRunResult> run_adopted_fd(
        int fd, PreparedTuHandle prepared = {}, EndpointIoControl control = {},
        std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

    static std::optional<boost::asio::ip::tcp::socket> adopt_connected_fd(
        boost::asio::any_io_executor executor, int fd,
        boost::system::error_code& error);

    // Cancel only the currently active C-role dialogue.  The caller must post
    // this method onto the endpoint's owner executor.  It never resets route
    // state or authorizes fallback: run() reports an observation and the
    // owning FSession operation performs any distributed settlement.
    EndpointCancelResult request_cancel(const EndpointCancelPermit& permit) noexcept;
    size_t cancel_all_for_incarnation(const SidecarLaunchIdentity& incarnation) noexcept;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    void request_cancel_for_test() noexcept;
#endif

    [[nodiscard]] CStoreGuid c_store_guid() const;
    [[nodiscard]] std::optional<FStoreGuid> f_store_guid() const;
    [[nodiscard]] bool has_active_transaction() const;
    [[nodiscard]] bool has_reconciliation_work() const;
    [[nodiscard]] RelSeq next_rel_seq() const;
    [[nodiscard]] Digest128 state_digest() const;

private:
    boost::asio::awaitable<ClientRunResult> run_connected(
        std::optional<boost::asio::ip::tcp::endpoint> remote,
        std::optional<boost::asio::ip::tcp::socket> socket,
        PreparedTuHandle prepared, EndpointIoControl control,
        std::optional<std::chrono::steady_clock::time_point> deadline);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class P50ServerEndpoint {
public:
    // Takes ownership of fd on every path.  Success returns one move-only,
    // CLOEXEC, connected TCP socket; failure closes fd and returns nullopt.
    static std::optional<boost::asio::ip::tcp::socket> adopt_connected_fd(
        boost::asio::any_io_executor executor, int fd,
        boost::system::error_code& error);

    explicit P50ServerEndpoint(FStoreGuid f_store_guid, EndpointCaps caps = {},
                               CompletionLog* completions = nullptr,
                               ActionTrace* actions = nullptr,
                               P50ServerEndpointConfig config = {});
    ~P50ServerEndpoint();
    P50ServerEndpoint(const P50ServerEndpoint&) = delete;
    P50ServerEndpoint& operator=(const P50ServerEndpoint&) = delete;

    boost::asio::awaitable<ServerRunResult> accept_one(boost::asio::ip::tcp::acceptor& acceptor,
                                                       EndpointIoControl control = {});

    // Consumes exactly one already-connected socket.  No accept/listen or read
    // occurs before ownership reaches the shared server transaction reducer.
    boost::asio::awaitable<ServerRunResult> run_adopted(
        boost::asio::ip::tcp::socket socket, EndpointIoControl control = {});

    // Consumes the complete post-P5CO authority bundle.  The retained socket
    // descriptor is endpoint-private and cannot be detached independently of
    // the exact adopted outcome and original absolute deadline.
    boost::asio::awaitable<ServerRunResult> run_adopted(
        sidecar::P5coEndpointHandoff handoff,
        EndpointIoControl control = {});

    // Cancels the active socket on the endpoint's owner executor. The caller
    // must arrange that affinity (SidecarRuntime posts this method); it never
    // changes listener or store ownership and is a no-op between dialogues.
    EndpointCancelResult request_cancel(const EndpointCancelPermit& permit) noexcept;
    size_t cancel_all_for_incarnation(const SidecarLaunchIdentity& incarnation) noexcept;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    void request_cancel_for_test() noexcept;
    [[nodiscard]] std::optional<std::string>
    global_resource_invariant_for_test() const;
#endif

    void reset_store(FStoreGuid new_guid);
    [[nodiscard]] InputCursor attach_input(InputRecordKey key) const;
    void close_input_job(InputRecordKey key);
    void collect_input_garbage();
    [[nodiscard]] FStoreGuid f_store_guid() const;
    [[nodiscard]] size_t namespace_count() const;
    [[nodiscard]] size_t revision_count() const;
    [[nodiscard]] size_t live_session_count() const;
    [[nodiscard]] P50ServerOwnerUsage owner_usage() const;
    [[nodiscard]] std::optional<InputRecordKey>
    last_committed_input(CStoreGuid c_store_guid) const;

private:
    class SessionRegistration;

    boost::asio::awaitable<ServerRunResult> run_connected(
        boost::asio::ip::tcp::socket socket, SessionRegistration registration,
        EndpointIoControl control, boost::asio::ip::tcp::acceptor* acceptor,
        std::optional<sidecar::AbsoluteMonotonicDeadline> deadline,
        std::optional<CStoreGuid> expected_c_store_guid);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace icecc::p50
