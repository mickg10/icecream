#pragma once

// The source arm is the small, explicit bridge between the ordinary
// CompileFile assignment and the later CacheWire input.  It intentionally
// keeps compiler-attempt authority separate from the immutable input key:
// retries may replace the attempt while consuming the same C_STORE_GUID/TU_SEQ.

#include "protocol50.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace icecc::p50 {

struct P50SourceArm {
    uint32_t wire_job_id = 0;
    uint64_t assignment_epoch = 0;
    uint64_t assignment_nonce = 0;

    // The selected F is one ordinary/cache endpoint authority.  The daemon
    // must not join these fields from separate UseCS frames.
    std::string selected_f_host;
    uint32_t selected_f_ordinary_port = 0;
    uint32_t selected_f_cache_port = 0;
    uint32_t cache_protocol = 0;
    uint32_t cache_profile = 0;

    uint64_t logical_job = 0;
    uint64_t attempt_id = 0;
    uint64_t c_store_generation = 0;
    CStoreGuid c_store_guid{};
    uint64_t source_request_id = 0;
    uint32_t source_mode = 0;

    auto operator<=>(const P50SourceArm&) const = default;
    [[nodiscard]] bool valid() const noexcept;
};

struct P50InputReady {
    P50SourceArm arm{};
    TuSeq tu_seq{};
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};
    FStoreGuid f_store_guid{};
    uint64_t attachment_store_generation = 0;
    uint64_t attachment_request_id = 0;
    uint64_t ready_event_id = 0;

    auto operator<=>(const P50InputReady&) const = default;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool matches_arm(const P50SourceArm& expected) const noexcept;
};

// Stable, length-delimited Protocol-50 fixtures.  This is a fixture codec for
// the two-phase seam, not a new negotiated protocol.  Version 1 is deliberately
// the existing Protocol-50 number (50); legacy peers never enter this codec.
enum class P50SourceWirePhase : uint16_t {
    Arm = 1,
    InputReady = 2,
};

std::vector<uint8_t> encode_source_arm(const P50SourceArm& arm);
std::vector<uint8_t> encode_input_ready(const P50InputReady& ready);
std::optional<P50SourceArm> decode_source_arm(std::span<const uint8_t> wire);
std::optional<P50InputReady> decode_input_ready(std::span<const uint8_t> wire);

}  // namespace icecc::p50
