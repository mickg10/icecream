#include "p50_actions.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace icecc::p50 {
namespace {

std::string bytes_hex(std::span<const uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (size_t i = 0; i != bytes.size(); ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return result;
}

struct TxIdentity {
    HistoryNonce nonce{};
    RelSeq rel{};
    TuSeq tu{};
    Digest128 transaction_digest{};
    Digest128 raw_digest{};
    auto operator<=>(const TxIdentity&) const = default;
};

// Route pipelining: the additive queue_depth trace field is validated
// against the same hard ceiling the runtime enforces
// (SidecarRuntime::RouteGate::kMaxPipelineDepth, cache/p50_cache_service.h)
// so a trace can never claim a deeper route than the admission gate can
// ever produce.
constexpr uint32_t kMaxTraceQueueDepth = 4;

TxIdentity tx_identity(const ActionRecord& record) {
    return {record.history_nonce, record.rel_seq, record.tu_seq,
            record.transaction_digest, record.raw_digest};
}

struct CCheckerState {
    bool cursor_known = false;
    HistoryNonce nonce{};
    RelSeq next_rel{};
    Digest128 state_digest{};
    // Route pipelining (P50_ROUTE_PIPELINING.md step 7, invariants 8/9):
    // the scalar "active" generalizes to C's bounded ACTIVE_TX FIFO
    // (active_queue_), front = head = oldest not-yet-retired TX.  Every
    // generalized check below is written so a one-member queue reproduces
    // the original scalar assertion exactly -- that is the K=1 identity
    // argument: a K=1 record carries no queue fields, so the queue holds at
    // most the head member and each check degenerates to today's.
    std::deque<TxIdentity> queue;
    // Identities removed by a *fielded* TX_ABORTED drain (universal
    // invalidation): a later HISTORY_RESET may legitimately discard their
    // still-unacknowledged durable commits.
    std::vector<TxIdentity> abandoned_pipelined;
    // Consecutive fielded TX_ABORTED records since the last queue
    // push/retire/reset: abandon_active drains front-to-back with
    // positions 0..n-1, so this validates a drain batch's order.
    size_t aborted_batch = 0;
};

struct FCheckerState {
    bool connected = false;
    uint64_t session_serial = 0;
    uint64_t last_session_serial = 0;
    bool reset_used_in_session = false;
    bool history_nonce_known = false;
    HistoryNonce last_history_nonce{};
    bool route = false;
    HistoryNonce nonce{};
    RelSeq next_rel{};
    Digest128 state_digest{};
    std::optional<TxIdentity> pending;
    bool body_complete = false;
    bool need_recorded = false;
    std::set<Key64> requested;
    std::set<Key64> remaining;
    bool materialized = false;
    std::optional<TxIdentity> last_commit;
    // Route pipelining: F may hold several durable commits C has not
    // accepted; they retire strictly front-first.  commit_digests carries
    // each commit's post digest in the same order.  At K=1 both hold at
    // most the one entry last_commit names, so "front of unacked" is
    // today's scalar.
    std::deque<TxIdentity> unacked;
    std::deque<Digest128> commit_digests;
    bool commit_unacknowledged = false;
    bool commit_disconnected = false;
    std::map<Key64, Digest128> installed;
};

struct RelationshipCheckerState {
    CCheckerState c;
    FCheckerState f;
};

void clear_f_pending(FCheckerState& state) {
    state.pending.reset();
    state.body_complete = false;
    state.need_recorded = false;
    state.requested.clear();
    state.remaining.clear();
    state.materialized = false;
}

uint64_t action_hold_timeout_ms() {
    const char* text = std::getenv("ICECC_P50_TEST_ACTION_HOLD_TIMEOUT_MS");
    if (text == nullptr || *text == '\0')
        return 30000;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > 300000)
        throw std::runtime_error("invalid Protocol-50 action hold timeout");
    return static_cast<uint64_t>(value);
}

void write_all(int fd, std::string_view bytes) {
    size_t offset = 0;
    while (offset != bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + offset,
                                        bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        throw std::runtime_error("cannot write Protocol-50 action hold marker");
    }
}

void sync_file(int fd, std::string_view detail) {
    while (::fdatasync(fd) != 0) {
        if (errno == EINTR)
            continue;
        throw std::runtime_error(std::string(detail));
    }
}

void close_checked(int& fd, std::string_view detail) {
    const int owned = fd;
    fd = -1;
    if (::close(owned) != 0)
        throw std::runtime_error(std::string(detail));
}

bool selected_hold_action(const ActionRecord& record) {
    const char* target = std::getenv("ICECC_P50_TEST_ACTION_HOLD");
    if (target == nullptr || *target == '\0')
        return false;
    const std::string expected = std::string(actor_name(record.actor)) + ':' +
                                 std::string(action_name(record.action));
    return expected == target;
}

bool completed_hold_marker(std::string_view path, const ActionRecord& record) {
    struct stat pathname {};
    const std::string owned_path(path);
    if (::lstat(owned_path.c_str(), &pathname) != 0 ||
        !S_ISREG(pathname.st_mode) || (pathname.st_mode & 07777) != 0600 ||
        pathname.st_nlink != 1 || pathname.st_uid != ::geteuid() ||
        pathname.st_size <= 0 || pathname.st_size > 64 * 1024)
        return false;

    int fd = ::open(owned_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat descriptor {};
    std::string bytes;
    bool valid = ::fstat(fd, &descriptor) == 0 &&
                 descriptor.st_dev == pathname.st_dev &&
                 descriptor.st_ino == pathname.st_ino &&
                 descriptor.st_size == pathname.st_size;
    if (valid) {
        bytes.resize(static_cast<size_t>(pathname.st_size));
        size_t offset = 0;
        while (offset != bytes.size()) {
            const ssize_t read_count =
                ::read(fd, bytes.data() + offset, bytes.size() - offset);
            if (read_count > 0) {
                offset += static_cast<size_t>(read_count);
                continue;
            }
            if (read_count < 0 && errno == EINTR)
                continue;
            valid = false;
            break;
        }
    }
    const int close_result = ::close(fd);
    if (close_result != 0)
        valid = false;
    if (!valid)
        return false;

    const std::string actor(actor_name(record.actor));
    const std::string action(action_name(record.action));
    const std::string header_prefix = "P50_ACTION_HOLD pid=";
    const std::string header_suffix =
        " actor=" + actor + " action=" + action + '\n';
    const size_t first_newline = bytes.find('\n');
    if (!bytes.starts_with(header_prefix) || first_newline == std::string::npos ||
        bytes.substr(0, first_newline + 1).find(header_suffix) ==
            std::string::npos ||
        bytes.empty() || bytes.back() != '\n' ||
        bytes.find('\n', first_newline + 1) != bytes.size() - 1)
        return false;
    const size_t pid_begin = header_prefix.size();
    const size_t pid_end = bytes.find(' ', pid_begin);
    if (pid_end == std::string::npos || pid_end == pid_begin)
        return false;
    for (size_t index = pid_begin; index != pid_end; ++index) {
        if (bytes[index] < '0' || bytes[index] > '9')
            return false;
    }
    if (bytes.substr(pid_begin, pid_end - pid_begin) == "0" ||
        bytes.substr(pid_end, first_newline + 1 - pid_end) != header_suffix)
        return false;
    const std::string json_prefix =
        "{\"action\":\"" + action + "\",\"actor\":\"" + actor + "\",";
    return bytes.substr(first_newline + 1).starts_with(json_prefix);
}

void hold_after_test_action(const ActionRecord& record) {
    if (!selected_hold_action(record))
        return;

    const char* marker = std::getenv("ICECC_P50_TEST_ACTION_HOLD_MARKER");
    const char* release = std::getenv("ICECC_P50_TEST_ACTION_HOLD_RELEASE");
    if (marker == nullptr || *marker == '\0' || release == nullptr ||
        *release == '\0' || std::string_view(marker) == release)
        throw std::runtime_error("Protocol-50 action hold paths are incomplete");

    struct stat existing {};
    if (::lstat(marker, &existing) == 0) {
        if (completed_hold_marker(marker, record))
            return;
        throw std::runtime_error("Protocol-50 action hold marker is incomplete");
    }
    if (errno != ENOENT)
        throw std::runtime_error("cannot inspect Protocol-50 action hold marker");

    const std::string temporary = std::string(marker) + ".tmp." +
                                  std::to_string(::getpid());
    int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                           O_NOFOLLOW,
                    0600);
    if (fd < 0) {
        throw std::runtime_error("cannot create Protocol-50 action hold marker");
    }
    try {
        const std::string header =
            "P50_ACTION_HOLD pid=" + std::to_string(::getpid()) +
            " actor=" + std::string(actor_name(record.actor)) +
            " action=" + std::string(action_name(record.action)) + '\n';
        write_all(fd, header);
        write_all(fd, action_jsonl(record) + '\n');
        sync_file(fd, "cannot sync Protocol-50 action hold marker");
        close_checked(fd, "cannot close Protocol-50 action hold marker");
        // ActionTrace is a single writer.  Publishing by rename means a
        // watcher can never observe a partially written marker; the private
        // per-run path is required to be absent before the daemon starts.
        if (::rename(temporary.c_str(), marker) != 0)
            throw std::runtime_error("cannot publish Protocol-50 action hold marker");
    } catch (...) {
        if (fd >= 0)
            (void)::close(fd);
        (void)::unlink(temporary.c_str());
        throw;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(action_hold_timeout_ms());
    for (;;) {
        struct stat status {};
        if (::lstat(release, &status) == 0) {
            if (!S_ISREG(status.st_mode) || status.st_nlink != 1)
                throw std::runtime_error(
                    "Protocol-50 action hold release is not a regular file");
            return;
        }
        if (errno != ENOENT)
            throw std::runtime_error("cannot inspect Protocol-50 action hold release");
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Protocol-50 action hold timed out");
        struct timespec delay {0, 10 * 1000 * 1000};
        while (::nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
    }
}

}  // namespace

void ActionTrace::record(ActionRecord record) noexcept {
    if (!valid_)
        return;
    if (records_.size() >= max_records_) {
        valid_ = false;
        return;
    }
    try {
        records_.push_back(std::move(record));
        const ActionRecord& emitted = records_.back();
        const char* variable = emitted.actor == ActorSide::C
                                   ? "ICECC_P50_C_ACTION_TRACE"
                                   : "ICECC_P50_F_ACTION_TRACE";
        const char* path = std::getenv(variable);
        if (path == nullptr || *path == '\0')
            path = std::getenv("ICECC_P50_ACTION_TRACE");
        if (path != nullptr && *path != '\0') {
            const std::string line = action_jsonl(emitted) + '\n';
            int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC |
                                      O_NOFOLLOW,
                            0600);
            if (fd < 0)
                throw std::runtime_error("cannot open Protocol-50 action trace sink");
            try {
                write_all(fd, line);
                if (selected_hold_action(emitted))
                    sync_file(fd, "cannot sync Protocol-50 action trace sink");
                close_checked(fd, "cannot close Protocol-50 action trace sink");
            } catch (...) {
                if (fd >= 0)
                    (void)::close(fd);
                throw;
            }
        }
        hold_after_test_action(emitted);
    } catch (...) {
        valid_ = false;
    }
}

bool action_trace_sink_enabled() noexcept {
    const char* c_path = std::getenv("ICECC_P50_C_ACTION_TRACE");
    const char* f_path = std::getenv("ICECC_P50_F_ACTION_TRACE");
    const char* shared_path = std::getenv("ICECC_P50_ACTION_TRACE");
    return (c_path != nullptr && *c_path != '\0') ||
           (f_path != nullptr && *f_path != '\0') ||
           (shared_path != nullptr && *shared_path != '\0');
}

std::string_view action_name(ActionType action) {
    switch (action) {
    case ActionType::SESSION_OPENED: return "SESSION_OPENED";
    case ActionType::SESSION_REPLACED: return "SESSION_REPLACED";
    case ActionType::SESSION_DISCONNECTED: return "SESSION_DISCONNECTED";
    case ActionType::HISTORY_RESET: return "HISTORY_RESET";
    case ActionType::TX_BEGIN: return "TX_BEGIN";
    case ActionType::TX_ABORTED: return "TX_ABORTED";
    case ActionType::BODY_COMPLETE: return "BODY_COMPLETE";
    case ActionType::NEED_RECORDED: return "NEED_RECORDED";
    case ActionType::OBJECT_APPLIED: return "OBJECT_APPLIED";
    case ActionType::INPUT_MATERIALIZED: return "INPUT_MATERIALIZED";
    case ActionType::INPUT_COMMITTED: return "INPUT_COMMITTED";
    case ActionType::COMMIT_ACCEPTED: return "COMMIT_ACCEPTED";
    case ActionType::ACTIVE_REPLAYED: return "ACTIVE_REPLAYED";
    case ActionType::LOST_COMMIT_ACCEPTED: return "LOST_COMMIT_ACCEPTED";
    case ActionType::F_STORE_INCAR_REPLACED: return "F_STORE_INCAR_REPLACED";
    }
    throw std::logic_error("unknown Protocol-50 action");
}

std::string_view actor_name(ActorSide actor) {
    switch (actor) {
    case ActorSide::C: return "C";
    case ActorSide::F: return "F";
    }
    throw std::logic_error("unknown Protocol-50 actor");
}

std::string action_jsonl(const ActionRecord& record) {
    std::ostringstream out;
    out << "{\"action\":\"" << action_name(record.action)
        << "\",\"actor\":\"" << actor_name(record.actor)
        << "\",\"profile\":\"" << profile_name(record.profile)
        << "\",\"c_store_guid\":\"" << bytes_hex(record.c_store_guid.bytes)
        << "\",\"f_store_guid\":\"" << bytes_hex(record.f_store_guid.bytes)
        << "\",\"previous_f_store_guid\":\""
        << bytes_hex(record.previous_f_store_guid.bytes)
        << "\",\"session_serial\":" << record.session_serial
        << ",\"history_nonce\":" << record.history_nonce.value
        << ",\"rel_seq\":" << record.rel_seq.value
        << ",\"tu_seq\":" << record.tu_seq.value
        << ",\"transaction_digest\":\""
        << digest128_hex(record.transaction_digest)
        << "\",\"raw_digest\":\"" << digest128_hex(record.raw_digest)
        << "\",\"state_digest\":\"" << digest128_hex(record.state_digest)
        << "\",\"content_digest\":\"" << digest128_hex(record.content_digest)
        << "\",\"key64\":";
    if (record.key) out << record.key->wire_value();
    else out << "null";
    out << ",\"need_keys\":[";
    for (size_t index = 0; index != record.need_keys.size(); ++index) {
        if (index) out << ',';
        out << record.need_keys[index].wire_value();
    }
    out << "],\"remaining_need\":" << record.remaining_need
        << ",\"duplicate\":" << (record.duplicate ? "true" : "false")
        << ",\"stage_bytes\":" << record.stage_bytes;
    // Route-pipelining additive fields: emitted only when set (i.e. only on
    // C-side records of a K>1 route), so a K=1 record's bytes -- including
    // this object's terminating '}' -- are identical to a pre-pipelining
    // build's output.
    if (record.queue_position)
        out << ",\"queue_position\":" << *record.queue_position;
    if (record.queue_depth)
        out << ",\"queue_depth\":" << *record.queue_depth;
    out << '}';
    return out.str();
}

void write_action_trace(const ActionTrace& trace, const std::string& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open action trace output");
    for (const ActionRecord& record : trace.records())
        output << action_jsonl(record) << '\n';
    if (!output) throw std::runtime_error("cannot write action trace output");
}

std::optional<std::string> check_action_trace(std::span<const ActionRecord> records) {
    using RelationshipKey = std::tuple<CStoreGuid, FStoreGuid, ProfileId>;
    std::map<RelationshipKey, RelationshipCheckerState> relationships;
    for (size_t index = 0; index != records.size(); ++index) {
        const ActionRecord& record = records[index];
        if (profile_bit(record.profile) == 0 ||
            (profile_bit(record.profile) & kOperationalProfileMask) == 0)
            return "action " + std::to_string(index) +
                   ": profile is not operational";
        RelationshipCheckerState& relationship =
            relationships[{record.c_store_guid, record.f_store_guid,
                           record.profile}];
        CCheckerState& c = relationship.c;
        FCheckerState& f = relationship.f;
        const auto error = [&](std::string_view detail) -> std::optional<std::string> {
            return "action " + std::to_string(index) + " (" +
                   std::string(actor_name(record.actor)) + "/" +
                   std::string(action_name(record.action)) + "/" +
                   std::string(profile_name(record.profile)) + "): " +
                   std::string(detail);
        };
        const auto current_f_session = [&] {
            return record.actor == ActorSide::F && f.connected &&
                   record.session_serial == f.session_serial;
        };
        switch (record.action) {
        case ActionType::SESSION_OPENED:
        case ActionType::SESSION_REPLACED: {
            if (record.actor != ActorSide::F)
                return error("session transition was not emitted by F");
            if (record.session_serial == 0) return error("session serial is zero");

            FCheckerState* connected = nullptr;
            uint64_t last_session_serial = 0;
            for (auto& [key, candidate] : relationships) {
                if (std::get<0>(key) != record.c_store_guid ||
                    std::get<1>(key) != record.f_store_guid)
                    continue;
                last_session_serial =
                    std::max(last_session_serial,
                             candidate.f.last_session_serial);
                if (candidate.f.connected) {
                    if (connected != nullptr)
                        return error("multiple profile sessions were live");
                    connected = &candidate.f;
                }
            }
            if ((connected != nullptr) !=
                (record.action == ActionType::SESSION_REPLACED))
                return error("open/replace does not match the current F session");
            if (record.session_serial <= last_session_serial)
                return error("session serial was reused or did not increase");
            if (connected != nullptr) {
                if (connected->commit_unacknowledged)
                    connected->commit_disconnected = true;
                connected->connected = false;
                clear_f_pending(*connected);
            }
            f.connected = true;
            f.session_serial = record.session_serial;
            f.last_session_serial = record.session_serial;
            f.reset_used_in_session = false;
            clear_f_pending(f);
            break;
        }
        case ActionType::SESSION_DISCONNECTED:
            if (!current_f_session())
                return error("disconnect does not name the current session");
            f.connected = false;
            if (f.commit_unacknowledged) f.commit_disconnected = true;
            clear_f_pending(f);
            break;
        case ActionType::F_STORE_INCAR_REPLACED: {
            if (record.actor != ActorSide::C ||
                record.previous_f_store_guid == FStoreGuid{} ||
                record.previous_f_store_guid == record.f_store_guid)
                return error("incarnation replacement lacks distinct old/new F identities");
            const auto old_position = relationships.find(
                {record.c_store_guid, record.previous_f_store_guid,
                 record.profile});
            // Generalized from the scalar c.active: the retained C
            // transaction is the head of the old relationship's ACTIVE_TX
            // queue (at K=1 the queue holds at most that one member).
            if (old_position == relationships.end() ||
                old_position->second.c.queue.empty() ||
                old_position->second.c.queue.front() != tx_identity(record))
                return error("incarnation replacement lost its retained C transaction");
            if (old_position->second.f.connected)
                return error("incarnation replacement occurred while the old F session was live");
            if (!f.connected)
                return error("incarnation replacement lacks the observed new F session");
            // Universal invalidation: the whole old queue goes, never a
            // partial unwind (design invariant 6).
            old_position->second.c.queue.clear();
            old_position->second.c.abandoned_pipelined.clear();
            old_position->second.c.aborted_batch = 0;
            clear_f_pending(old_position->second.f);
            old_position->second.f.unacked.clear();
            old_position->second.f.commit_digests.clear();
            old_position->second.f.commit_unacknowledged = false;
            old_position->second.f.commit_disconnected = false;
            break;
        }
        case ActionType::HISTORY_RESET:
            if (!current_f_session())
                return error("history reset without a current F session");
            if (f.pending) return error("history reset while F has a pending transaction");
            // Generalized from the scalar c.active: a pipelined route may
            // hold up to K outstanding TXs; a reset is legal only once every
            // one has been resolved (accepted commit or a TX_ABORTED drain).
            if (!c.queue.empty())
                return error("history reset while C has an active transaction");
            // A pipelined route may reset while F still holds durable
            // commits C has deliberately invalidated (poison -> cold reset,
            // design section 3 P3/P5) -- but only if every unacknowledged
            // commit names an abandoned queue member.  At K=1 nothing is
            // ever in abandoned_pipelined, so this stays exactly today's
            // blanket rejection.
            if (f.commit_unacknowledged &&
                std::any_of(f.unacked.begin(), f.unacked.end(),
                            [&](const TxIdentity& entry) {
                                return std::find(c.abandoned_pipelined.begin(),
                                                 c.abandoned_pipelined.end(),
                                                 entry) ==
                                       c.abandoned_pipelined.end();
                            }))
                return error("history reset skipped an unacknowledged durable commit");
            if (f.reset_used_in_session)
                return error("second history reset in one F session");
            if (f.history_nonce_known &&
                record.history_nonce.value <= f.last_history_nonce.value)
                return error("history nonce was reused or did not increase");
            f.reset_used_in_session = true;
            f.history_nonce_known = true;
            f.last_history_nonce = record.history_nonce;
            f.route = true;
            f.nonce = record.history_nonce;
            f.next_rel = RelSeq{0};
            f.state_digest = record.state_digest;
            f.last_commit.reset();
            f.unacked.clear();
            f.commit_digests.clear();
            f.commit_unacknowledged = false;
            f.commit_disconnected = false;
            c.queue.clear();
            c.abandoned_pipelined.clear();
            c.aborted_batch = 0;
            break;
        case ActionType::TX_BEGIN: {
            const TxIdentity identity = tx_identity(record);
            if (record.actor == ActorSide::C) {
                // Additive route-pipelining fields: both present (K>1
                // admission) or both absent (K=1 / pre-pipelining, today's
                // scalar path verbatim).  A record carrying exactly one is
                // a generation error.
                const bool has_position = record.queue_position.has_value();
                const bool has_depth = record.queue_depth.has_value();
                if (has_position != has_depth)
                    return error("C TX_BEGIN carries only one queue field");
                if (has_depth &&
                    (*record.queue_depth == 0 ||
                     *record.queue_depth > kMaxTraceQueueDepth))
                    return error("C TX_BEGIN has an invalid queue depth");
                if (has_position &&
                    *record.queue_position >= *record.queue_depth)
                    return error("C TX_BEGIN has an invalid queue position");
                if (has_position) {
                    // Pipelined admission: the position is the 0-based slot
                    // this TX takes in C's ACTIVE_TX queue, so it must equal
                    // the current length and stay below the depth K.
                    if (*record.queue_position != c.queue.size())
                        return error("C TX_BEGIN missed its queue slot");
                    // A successor begin is legal while F still requires
                    // reconciliation of a predecessor (that is the point of
                    // pipelining), but the reconciliation identity must be
                    // a live queue member.
                    if (f.pending &&
                        std::find(c.queue.begin(), c.queue.end(), *f.pending) ==
                            c.queue.end())
                        return error("C began while F owns a foreign pending transaction");
                } else if (!c.queue.empty()) {
                    return error("C opened a second active transaction");
                }
                if (!has_position && (f.pending || f.commit_unacknowledged))
                    return error("C began while F state still required reconciliation");
                if (record.rel_seq.value == std::numeric_limits<uint64_t>::max())
                    return error("C REL_SEQ exhausted before TX_BEGIN");
                if (!c.cursor_known || record.history_nonce != c.nonce) {
                    if (record.rel_seq.value != 0)
                        return error("new C history did not start at REL_SEQ zero");
                    c.cursor_known = true;
                    c.nonce = record.history_nonce;
                    c.next_rel = record.rel_seq;
                    c.state_digest = record.state_digest;
                } else if (has_position) {
                    // Pipelined successor: rel_seq advances one per queue
                    // slot from the confirmed cursor.  At position 0 the
                    // pre-state digest is still the confirmed cursor digest
                    // (exactly today's check); at position > 0 it is the
                    // predecessor's predicted post-digest, which the trace
                    // does not carry, so only the rel_seq chain is
                    // checkable here -- F's own TX_BEGIN cursor check below
                    // verifies the digest against F's route, unchanged.
                    if (record.rel_seq.value !=
                            c.next_rel.value + *record.queue_position ||
                        (*record.queue_position == 0 &&
                         record.state_digest != c.state_digest))
                        return error("C transaction does not match its route cursor");
                } else if (record.rel_seq != c.next_rel ||
                           record.state_digest != c.state_digest) {
                    return error("C transaction does not match its route cursor");
                }
                c.queue.push_back(identity);
                c.aborted_batch = 0;
                break;
            }
            if (!current_f_session() || !f.route)
                return error("F accepted TX_BEGIN without a current route");
            if (f.pending) return error("F accepted a second pending transaction");
            // Generalized from the scalar c.active: F begins one TX at a
            // time, strictly in rel_seq order, but under pipelining the TX
            // it begins may be any member of C's outstanding queue, not
            // just the head.  At K=1 the queue holds at most the head
            // member, so this is today's scalar assertion.
            if (std::find(c.queue.begin(), c.queue.end(), identity) ==
                c.queue.end())
                return error("F TX_BEGIN does not name C's active transaction");
            // Route pipelining: a successor connection is pre-admitted by C
            // (its TX is already a queue member before F releases it), so a
            // session replacement that opened this session was ordinary
            // pipelining, not a lost acknowledgement -- the predecessor's
            // COMMIT is still in flight on its own connection.  Guarded on
            // the *pipelined* shape (a queue holding a successor beyond the
            // accepted-not-yet head) so a K=1 lost-ack marker can never be
            // erased by a replayed begin: at K=1 the queue never holds more
            // than the one unacknowledged member itself.
            if (c.queue.size() > 1)
                f.commit_disconnected = false;
            if (record.history_nonce != f.nonce || record.rel_seq != f.next_rel ||
                record.state_digest != f.state_digest)
                return error("F TX_BEGIN does not match its route cursor");
            clear_f_pending(f);
            f.pending = identity;
            break;
        }
        case ActionType::TX_ABORTED: {
            const TxIdentity identity = tx_identity(record);
            // Generalized from the scalar c.active: a pipelined route aborts
            // exactly the TXs it still holds outstanding, one record per
            // queue member, front-to-back (CRoute::abandon_active).  At K=1
            // the queue holds at most the head member.
            const auto member = std::find(c.queue.begin(), c.queue.end(),
                                         identity);
            if (record.actor != ActorSide::C || member == c.queue.end())
                return error("abort does not name C's active transaction");
            const bool has_position = record.queue_position.has_value();
            const bool has_depth = record.queue_depth.has_value();
            if (has_position != has_depth)
                return error("C TX_ABORTED carries only one queue field");
            if (has_position) {
                // Pipelined abort drain: the record's position names the
                // member (emitted front-to-back with positions 0..n-1) and
                // must not exceed the route's depth.  A batch that ends
                // early leaves a survivor, which the reconciliation witness
                // / HISTORY_RESET boundary checks reject.
                if (*record.queue_position >= *record.queue_depth ||
                    *record.queue_position >= kMaxTraceQueueDepth ||
                    *record.queue_position != c.aborted_batch ||
                    member != c.queue.begin())
                    return error("pipelined abort does not match its queue slot");
                // A successor abort is legal while F still holds the
                // predecessor's durable-but-unaccepted commit (universal
                // invalidation) or is mid-TX on the predecessor (parked
                // successors emit no F records); F's *own* pending
                // transaction is the predecessor's, so only the aborted
                // member itself must not be F-pending.
                if (f.pending && *f.pending == identity)
                    return error("C aborted F's pending transaction");
                c.queue.pop_front();
                c.abandoned_pipelined.push_back(identity);
                ++c.aborted_batch;
            } else {
                if (f.pending)
                    return error("C aborted while F had a pending transaction");
                if (f.commit_unacknowledged)
                    return error("C aborted before accepting F's durable commit");
                c.queue.erase(member);
                c.aborted_batch = 0;
            }
            break;
        }
        case ActionType::BODY_COMPLETE:
            if (!current_f_session() || !f.pending ||
                *f.pending != tx_identity(record) || f.body_complete)
                return error("BODY completion does not name F's pending transaction");
            f.body_complete = true;
            break;
        case ActionType::NEED_RECORDED:
            if (!current_f_session() || !f.pending ||
                *f.pending != tx_identity(record) || !f.body_complete ||
                f.need_recorded)
                return error("NEED was recorded before the exact BODY");
            if (!std::is_sorted(record.need_keys.begin(), record.need_keys.end()) ||
                std::adjacent_find(record.need_keys.begin(), record.need_keys.end()) !=
                    record.need_keys.end() ||
                record.remaining_need != record.need_keys.size())
                return error("recorded Need is not one exact sorted set");
            f.need_recorded = true;
            f.requested = {record.need_keys.begin(), record.need_keys.end()};
            f.remaining = f.requested;
            break;
        case ActionType::OBJECT_APPLIED:
            if (!current_f_session() || !f.pending ||
                *f.pending != tx_identity(record) || !f.need_recorded || !record.key ||
                !f.requested.contains(*record.key))
                return error("object application is outside the active Need");
            if (const auto installed = f.installed.find(*record.key);
                installed != f.installed.end()) {
                if (installed->second != record.content_digest)
                    return error("Key64 was applied with different immutable content");
            } else {
                f.installed.emplace(*record.key, record.content_digest);
            }
            if (f.remaining.erase(*record.key) != 0) {
                if (record.duplicate)
                    return error("first application of a missing object was marked duplicate");
            } else if (!record.duplicate) {
                return error("repeat object application was not marked duplicate");
            }
            if (record.remaining_need != f.remaining.size())
                return error("object application changed the exact Need incorrectly");
            break;
        case ActionType::INPUT_MATERIALIZED:
            if (!current_f_session() || !f.pending ||
                *f.pending != tx_identity(record) || !f.body_complete ||
                (f.need_recorded && !f.remaining.empty()) ||
                f.materialized)
                return error("input materialized before all prerequisites completed");
            f.materialized = true;
            break;
        case ActionType::INPUT_COMMITTED:
            if (!current_f_session() || !f.pending ||
                *f.pending != tx_identity(record) || !f.materialized)
                return error("F commit does not close its exactly materialized transaction");
            if (f.next_rel.value == std::numeric_limits<uint64_t>::max())
                return error("F REL_SEQ exhausted");
            f.last_commit = *f.pending;
            // Route pipelining: F may hold several durable commits C has not
            // accepted; they retire strictly front-first.  At K=1 the FIFO
            // holds at most the entry last_commit names.
            f.unacked.push_back(*f.pending);
            f.commit_digests.push_back(record.state_digest);
            f.commit_unacknowledged = true;
            f.commit_disconnected = false;
            clear_f_pending(f);
            ++f.next_rel.value;
            f.state_digest = record.state_digest;
            break;
        case ActionType::COMMIT_ACCEPTED:
        case ActionType::LOST_COMMIT_ACCEPTED: {
            const TxIdentity identity = tx_identity(record);
            const bool has_position = record.queue_position.has_value();
            const bool has_depth = record.queue_depth.has_value();
            if (has_position != has_depth)
                return error("C acceptance carries only one queue field");
            if (has_position &&
                (*record.queue_position >= *record.queue_depth ||
                 *record.queue_position >= kMaxTraceQueueDepth ||
                 // retire_locked_commits only ever emits the retiring head,
                 // so a fielded acceptance must name slot 0.
                 *record.queue_position != 0))
                return error("C acceptance does not name the retiring queue head");
            if (record.actor != ActorSide::C || c.queue.empty() ||
                // Generalized from the scalar c.active: C accepts strictly
                // front-first (retire_locked_commits), so the accepted TX
                // must be the head of C's outstanding queue.
                c.queue.front() != identity)
                return error("C acceptance does not name its active transaction");
            // ... and the head of F's unacknowledged commits (the oldest
            // durable commit C has not accepted yet), replacing today's
            // scalar last_commit comparison.  The accepted digest is
            // compared against this TX's *own* commit digest, not F's
            // newest route digest -- at K=1 (one outstanding commit) these
            // are the same value, so today's check is preserved exactly.  A
            // successor retiring before its head is the exact bug the
            // design's touch-up (b) forbids.
            if (f.unacked.empty() || f.unacked.front() != identity ||
                record.state_digest != f.commit_digests.front())
                return error("C acceptance lacks the matching durable F commit");
            if (!f.commit_unacknowledged ||
                (record.action == ActionType::COMMIT_ACCEPTED &&
                 f.commit_disconnected) ||
                (record.action == ActionType::LOST_COMMIT_ACCEPTED &&
                 !f.commit_disconnected))
                return error("C acceptance used the wrong normal/lost path");
            if (c.next_rel.value == std::numeric_limits<uint64_t>::max())
                return error("C REL_SEQ exhausted");
            c.queue.pop_front();
            c.aborted_batch = 0;
            c.abandoned_pipelined.erase(
                std::remove(c.abandoned_pipelined.begin(),
                            c.abandoned_pipelined.end(), identity),
                c.abandoned_pipelined.end());
            ++c.next_rel.value;
            c.state_digest = record.state_digest;
            f.unacked.pop_front();
            f.commit_digests.pop_front();
            f.commit_unacknowledged = !f.unacked.empty();
            f.commit_disconnected = false;
            break;
        }
        case ActionType::ACTIVE_REPLAYED: {
            const TxIdentity identity = tx_identity(record);
            if (!current_f_session() || !f.route || f.pending ||
                // Same queue-membership generalization as F TX_BEGIN: the
                // replayed TX is one of C's outstanding queue members.  At
                // K=1 the queue holds at most the head member.
                std::find(c.queue.begin(), c.queue.end(), identity) ==
                    c.queue.end())
                return error("replay does not name C's retained active transaction");
            if (record.history_nonce != f.nonce || record.rel_seq != f.next_rel ||
                record.state_digest != f.state_digest)
                return error("replay does not match F's route cursor");
            clear_f_pending(f);
            f.pending = tx_identity(record);
            break;
        }

        // Reconciliation witness, generalized from "F is exactly one
        // REL_SEQ ahead of C" to "ahead by at most K, never behind"
        // (P50_ROUTE_PIPELINING.md invariant 9).  live_unacked excludes
        // commits whose C transaction was already abandoned (universal
        // invalidation): those are being discarded wholesale and a
        // HISTORY_RESET at a fresh nonce retires them above.  At K=1
        // nothing is ever abandoned-pipelined, so every check below sees
        // exactly today's scalar shape.
        if (f.commit_unacknowledged) {
            std::vector<TxIdentity> live_unacked;
            for (const TxIdentity& entry : f.unacked) {
                if (std::find(c.abandoned_pipelined.begin(),
                              c.abandoned_pipelined.end(),
                              entry) == c.abandoned_pipelined.end())
                    live_unacked.push_back(entry);
            }
            // Every *live* durable commit must still name a C queue member
            // (C has not accepted it and has not invalidated it).  F's
            // pending transaction, if any, is likewise a queue member.
            if ((!live_unacked.empty() && c.queue.empty()) ||
                (f.pending &&
                 std::find(c.queue.begin(), c.queue.end(), *f.pending) ==
                     c.queue.end()))
                return error("durable F commit lost its C reconciliation identity");
            // F commits strictly in rel_seq order and C retires strictly
            // front-first, so F's *live* unacknowledged commits are exactly
            // the oldest queue members, in order.
            if (live_unacked.size() > c.queue.size() ||
                !std::equal(live_unacked.begin(), live_unacked.end(),
                            c.queue.begin()))
                return error("durable F commit is not C's queue head");
            if (!f.last_commit || !f.unacked.empty() &&
                    *f.last_commit != f.unacked.back())
                return error("durable F commit does not match C active");
            if (!c.cursor_known || !f.route)
                return error("durable F commit lacks route cursors");
            // C's queue head sits exactly at C's confirmed cursor, so F's
            // cursor (one past its newest durable commit) is exactly
            // unacked.size() ahead -- the abandoned entries counted too,
            // since F's cursor really did advance past the commits C is
            // discarding.  At K=1 (one member, one unacked entry) this is
            // exactly the original "one REL_SEQ ahead".
            if (c.next_rel.value == std::numeric_limits<uint64_t>::max() ||
                f.nonce != c.nonce ||
                f.next_rel.value != c.next_rel.value + f.unacked.size())
                return error("durable F commit is not at most K REL_SEQ ahead");
            if (!live_unacked.empty() &&
                (live_unacked.front().nonce != c.nonce ||
                 live_unacked.front().rel != c.next_rel))
                return error("durable F commit active identity missed C cursor");
        }
    }
    return std::nullopt;
}

}  // namespace icecc::p50
