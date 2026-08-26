#pragma once

// Bounded Protocol-50 phase-open precursor.  This file deliberately stops at
// the accepted source-arrival metadata boundary: CompileFile wiring and the
// reverse sealed-FD transfer remain a later HOLD slice.

#include "p50_source_identity.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace icecc::p50 {

// A service-global offer is the authority for one on-demand handoff.  The
// request id is also the source-arm request id; keeping one value avoids a
// second correlation namespace that could be accidentally joined later.
struct HandoffOffer {
    uint64_t request_id = 0;
    uint32_t cache_profile = 0;
    P50SourceArm source_arm{};

    [[nodiscard]] bool valid() const noexcept;
    auto operator<=>(const HandoffOffer&) const = default;
};

// The only phase-open payload admitted by this precursor.  It must echo the
// exact offered request and complete source arm; no profile/path-only shortcut
// is accepted.
struct AttachmentPhaseOpen {
    uint64_t request_id = 0;
    P50SourceArm source_arm{};

    [[nodiscard]] bool valid() const noexcept;
    auto operator<=>(const AttachmentPhaseOpen&) const = default;
};

enum class PhaseOpenWirePhase : uint16_t {
    HandoffOffer = 1,
    AttachmentPhaseOpen = 2,
};

std::vector<uint8_t> encode_handoff_offer(const HandoffOffer& offer);
std::vector<uint8_t> encode_attachment_phase_open(const AttachmentPhaseOpen& open);
std::optional<HandoffOffer> decode_handoff_offer(std::span<const uint8_t> wire);
std::optional<AttachmentPhaseOpen> decode_attachment_phase_open(
    std::span<const uint8_t> wire);

// This object is created once per service incarnation and passed by reference
// to every receiver connection.  It is intentionally not connection-local:
// reconnects cannot reset request-id replay or high-water state.
enum class OfferDecision : uint8_t {
    Invalid = 0,
    Accepted,
    ExactReplay,
    StaleRequest,
    Conflict,
    NoOffer,
};

class P50HandoffAuthority {
public:
    P50HandoffAuthority() = default;
    P50HandoffAuthority(const P50HandoffAuthority&) = delete;
    P50HandoffAuthority& operator=(const P50HandoffAuthority&) = delete;

    // Accept one new strictly higher request, or replay the exact current
    // offer.  A lower/unknown request is stale; reusing the current request
    // for different bytes is a conflict.  The retained current offer is a
    // bounded replay record, while high_water_ permanently rejects old ids.
    OfferDecision offer(const HandoffOffer& value) noexcept;

    // Phase-open is admitted only after an exact offer.  Exact repeats across
    // a replacement receiver are idempotent and return ExactReplay.
    OfferDecision phase_open(const AttachmentPhaseOpen& value) noexcept;

    [[nodiscard]] uint64_t request_high_water() const noexcept { return high_water_; }
    [[nodiscard]] const std::optional<HandoffOffer>& current_offer() const noexcept {
        return current_offer_;
    }
    [[nodiscard]] bool phase_opened() const noexcept { return phase_opened_; }

private:
    uint64_t high_water_ = 0;
    std::optional<HandoffOffer> current_offer_;
    std::optional<AttachmentPhaseOpen> current_phase_open_;
    bool phase_opened_ = false;
};

enum class PhaseOpenState : uint8_t {
    Idle = 0,
    Offered,
    Established,
    SourceArrived,
    Expired,
    Closed,
};

enum class PhaseOpenResult : uint8_t {
    Invalid = 0,
    Accepted,
    ExactReplay,
    WrongRequest,
    WrongArm,
    PhaseViolation,
    DeadlineExpired,
};

// Local receiver state binds both deadline phases to absolute monotonic
// points.  Callers pass `now` explicitly so tests cannot accidentally restart
// a relative budget during a reconnect or ACK wait.
class P50PhaseOpenState {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    [[nodiscard]] PhaseOpenState state() const noexcept { return state_; }
    [[nodiscard]] const HandoffOffer& offer() const noexcept { return offer_; }
    [[nodiscard]] TimePoint phase_open_deadline() const noexcept {
        return phase_open_deadline_;
    }
    [[nodiscard]] TimePoint source_arrival_deadline() const noexcept {
        return source_arrival_deadline_;
    }

    // Installs an offer only if both supplied deadlines are absolute,
    // monotonic, and ordered.  This does not open the phase by itself.
    PhaseOpenResult install_offer(const HandoffOffer& offer,
                                  TimePoint now,
                                  TimePoint phase_open_deadline,
                                  TimePoint source_arrival_deadline) noexcept;

    // Establishment requires the exact request/arm after the offer and before
    // its dedicated absolute phase-open deadline.
    PhaseOpenResult accept_phase_open(const AttachmentPhaseOpen& open,
                                      TimePoint now) noexcept;

    // Source arrival is accepted only from Established and before the second
    // absolute deadline.  It records no FD; reverse sealed-FD delivery remains
    // intentionally outside this precursor.
    PhaseOpenResult accept_source_arrival(const P50InputReady& ready,
                                          TimePoint now) noexcept;

    void close() noexcept;

private:
    HandoffOffer offer_{};
    std::optional<P50InputReady> ready_;
    TimePoint phase_open_deadline_{};
    TimePoint source_arrival_deadline_{};
    PhaseOpenState state_ = PhaseOpenState::Idle;
};

}  // namespace icecc::p50
