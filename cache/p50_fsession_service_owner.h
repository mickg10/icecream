/*
    FSessionServiceOwner — the sidecar F-role owner's composition layer between
    dedicated per-operation control connections and the operation reducers
    (S2 vertical).

    One authenticated AF_UNIX control connection is dedicated to one logical
    F-session operation (issue-16 5443811178 sec.3). This owner keeps a bounded
    table of {frame reader, SidecarFSessionOperation} slots, feeds received
    bytes through exactly ONE incremental parser per connection, dispatches
    complete frames to the operation reducer, and drains staged outbound
    frames under writability in bounded per-turn quanta (the transactional/
    quantum writer discipline of 5445285181 applied to the control stream).

    Single-writer: every method is called only from the sidecar owner
    executor. Transport is injected (callbacks), so the layer is fully
    testable without sockets and pluggable into the service's poll loop.
*/
#ifndef ICECC_CACHE_P50_FSESSION_SERVICE_OWNER_H
#define ICECC_CACHE_P50_FSESSION_SERVICE_OWNER_H

#include "p50_fsession_control.h"
#include "p50_fsession_sidecar_op.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace icecc::p50::fsession {

// Result of one bounded ingest turn on a connection.
enum class ServiceIngestStatus : uint8_t {
    Progress = 0,   // bytes consumed; zero or more frames dispatched
    ConnectionError, // framing/decode violation: close this connection
    UnknownConnection,
};

// write_fn(bytes) -> bytes written (>0), 0 = would-block, <0 = write error.
using ServiceWriteFn = std::function<long(std::span<const uint8_t>)>;

class FSessionServiceOwner {
public:
    explicit FSessionServiceOwner(size_t max_operations = 8) noexcept
        : max_operations_(max_operations == 0 ? 1 : max_operations) {}

    // Positive admission is CLOSED by default: identity-bearing payloads are
    // still placeholders, and no live production caller may admit an
    // operation until the exact payload codecs are installed (5448067827
    // sec.3). Tests and the eventual codec-complete wiring enable it
    // explicitly; while closed, connection_opened() refuses (no partial row).
    void set_admission_enabled(bool enabled) noexcept {
        admission_enabled_ = enabled;
    }
    [[nodiscard]] bool admission_enabled() const noexcept {
        return admission_enabled_;
    }

    // A new dedicated control connection was accepted. Returns its nonzero
    // connection id, or 0 when the bounded operation table is full (the
    // caller refuses the connection; no partial row exists).
    [[nodiscard]] uint64_t connection_opened();

    // Bytes arrived. Feeds the ONE parser for this connection and dispatches
    // every complete frame to the operation reducer. On ConnectionError the
    // caller closes the connection; the operation follows the control-loss
    // law (local facts preserved).
    [[nodiscard]] ServiceIngestStatus
    on_bytes(uint64_t connection_id, std::span<const uint8_t> bytes,
             int64_t now_ns);

    // Drain staged outbound frames through write_fn, at most max_bytes this
    // turn (bounded owner turn; remaining bytes wait for the next
    // writability event). Returns false on a write error (terminal for the
    // connection; caller closes it).
    [[nodiscard]] bool drain_outbound(uint64_t connection_id,
                                      const ServiceWriteFn& write_fn,
                                      size_t max_bytes);

    // Owner access to the operation for endpoint/route/permit driving.
    [[nodiscard]] SidecarFSessionOperation* operation(uint64_t connection_id);

    // True while staged outbound frames remain unflushed (Queued/Writing),
    // i.e. the connection wants a writability event.
    [[nodiscard]] bool has_pending_outbound(uint64_t connection_id);

    // Control EOF/reset or caller-initiated close: applies the control-loss
    // law to the operation and releases the connection slot. The operation
    // object is retained until reclaim() so its local facts survive for
    // reconciliation.
    void connection_closed(uint64_t connection_id);

    // Reclaim a retired/reconciled operation's slot storage.
    [[nodiscard]] bool reclaim(uint64_t connection_id);

    [[nodiscard]] size_t live_operations() const noexcept;

private:
    struct Slot {
        uint64_t connection_id = 0;
        bool connection_open = false;
        bool in_use = false;
        FSessionFrameReader reader;
        std::unique_ptr<SidecarFSessionOperation> op;
    };
    [[nodiscard]] Slot* find(uint64_t connection_id) noexcept;

    size_t max_operations_;
    bool admission_enabled_ = false;
    uint64_t next_connection_id_ = 1;
    std::vector<Slot> slots_;
};

} // namespace icecc::p50::fsession

#endif // ICECC_CACHE_P50_FSESSION_SERVICE_OWNER_H
