/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* A P50-only worker-child to iceccd-parent completion witness. */
#ifndef ICECREAM_P50_COMPLETION_RECORD_H
#define ICECREAM_P50_COMPLETION_RECORD_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits.h>
#include <span>
#include <type_traits>

#include <job.h>

namespace icecc::p50 {

constexpr uint32_t kP50CompletionMagic = 0x50353043U; // "P50C"
constexpr uint16_t kP50CompletionVersion = 1;
/* Canonical bytes are big-endian; sizeof(P50CompletionRecord) is never sent. */
constexpr std::size_t kP50CompletionRecordWireSize = 144;
#ifdef PIPE_BUF
static_assert(kP50CompletionRecordWireSize <= PIPE_BUF,
              "P50 completion witness must be atomic on a pipe");
#endif

enum class P50CompletionResult : uint32_t { Succeeded = 1, Failed = 2 };

enum class P50CompletionDisposition : uint32_t {
    Completed = 1,
    /* Returned for EOF/malformed input; never emitted or used to close a job. */
    AttemptCancelOnly = 2,
};

/* These are exactly the identity dimensions currently available in
   CompileJob/CompileInputIdentity.  No scheduler/run/F-store identity is
   invented by this seam. */
struct P50CompletionRecord {
    uint32_t stats[8]{};
    int32_t result_status = 0;
    P50CompletionResult result = P50CompletionResult::Succeeded;
    P50CompletionDisposition disposition = P50CompletionDisposition::Completed;
    uint32_t job_id = 0;
    uint64_t assignment_epoch = 0;
    uint64_t assignment_nonce = 0;
    uint32_t input_profile = 0;
    std::array<uint8_t, 16> c_store_guid{};
    uint64_t tu_seq = 0;
    uint64_t raw_bytes = 0;
    std::array<uint8_t, 16> raw_digest{};
    uint64_t attempt_id = 0;
    uint64_t request_id = 0;
};

static_assert(std::is_trivially_copyable_v<P50CompletionRecord>,
              "record is a fixed POD value, not a wire image");
static_assert(std::is_standard_layout_v<P50CompletionRecord>,
              "record has one stable field layout for the in-process seam");

/* Missing/malformed is deliberately an attempt-only outcome.  This reusable
   seam has no operation that calls handle_end(), sends JobDone, or settles a
   logical lifecycle. */
struct P50CompletionObservation {
    P50CompletionDisposition disposition =
        P50CompletionDisposition::AttemptCancelOnly;
    P50CompletionRecord record{};
    bool valid() const { return disposition == P50CompletionDisposition::Completed; }
    bool closes_logical_job() const { return false; }
};

/* Copy only identity genuinely present on a complete P50 CompileJob. */
bool make_p50_completion_record(const CompileJob& job,
                                const uint32_t stats[8],
                                int32_t result_status,
                                P50CompletionRecord* out) noexcept;

/* Exact canonical encoding/decoding.  Decoding rejects short/trailing bytes,
   wrong version/size, reserved bits, invalid state, and incomplete identity. */
std::array<uint8_t, kP50CompletionRecordWireSize>
encode_p50_completion_record(const P50CompletionRecord& record);

bool decode_p50_completion_record(std::span<const uint8_t> bytes,
                                  P50CompletionRecord* out) noexcept;

/* Full EINTR-safe transfer helpers.  Read also requires EOF immediately after
   the fixed record, preventing a second/trailing record from being ignored. */
bool write_p50_completion_record(int fd,
                                 const P50CompletionRecord& record) noexcept;
P50CompletionObservation read_p50_completion_record(
    int fd, const CompileJob& expected_job) noexcept;

} // namespace icecc::p50

#endif
