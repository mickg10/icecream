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

class MsgChannel;

namespace icecc::p50 {

constexpr uint32_t kP50CompletionMagic = 0x50353043U; // "P50C"
constexpr uint16_t kP50CompletionVersion = 1;
/* Canonical bytes are big-endian; sizeof(P50CompletionRecord) is never sent. */
constexpr std::size_t kP50CompletionRecordWireSize = 144;
/* The deployed pre-P50 child status pipe is exactly eight native uint32_t
   words.  This constant is an assertion boundary only; legacy bytes are not
   encoded or decoded by the canonical P50 record. */
constexpr std::size_t kLegacyCompletionStatsWireSize = 8 * sizeof(uint32_t);
static_assert(kLegacyCompletionStatsWireSize == 32,
              "legacy child completion path must remain exactly 32 bytes");
#ifdef PIPE_BUF
static_assert(kP50CompletionRecordWireSize <= PIPE_BUF,
              "P50 completion witness must be atomic on a pipe");
#endif

enum class P50CompletionResult : uint32_t { Succeeded = 1, Failed = 2 };

enum class P50CompletionDisposition : uint32_t {
    Accepted = 1,
    DefinitiveCancel = 2,
    /* Missing/malformed/disconnected submitter disposition.  This ends only
       the worker attempt and is never a logical-job terminal observation. */
    AttemptCancelOnly = 3,
};

/* These are exactly the identity dimensions currently available in
   CompileJob/CompileInputIdentity.  No scheduler/run/F-store identity is
   invented by this seam. */
struct P50CompletionRecord {
    uint32_t stats[8]{};
    int32_t result_status = 0;
    P50CompletionResult result = P50CompletionResult::Succeeded;
    P50CompletionDisposition disposition =
        P50CompletionDisposition::AttemptCancelOnly;
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
    bool record_valid = false;

    bool valid() const { return record_valid; }
    bool terminal_disposition() const
    {
        return record_valid &&
            (disposition == P50CompletionDisposition::Accepted ||
             disposition == P50CompletionDisposition::DefinitiveCancel);
    }
    /* The observation alone never owns lifecycle mutation.  The daemon must
       additionally match its retained sidecar lease before settling. */
    bool closes_logical_job() const { return false; }
};

enum class P50CompletionPumpResult : uint8_t {
    Pending = 0,
    Accepted,
    DefinitiveCancel,
    AttemptCancelOnly,
};

/* A daemon-event-loop reader.  Each pump is nonblocking and bounded.  A full
   record remains Pending until EOF proves that the child wrote no trailing or
   conflicting witness; callers retain the reader and readiness-drive it. */
class P50CompletionRecordReader {
public:
    P50CompletionRecordReader(int fd, const CompileJob& expected_job) noexcept;

    P50CompletionPumpResult pump(
        std::size_t max_bytes = kP50CompletionRecordWireSize + 1) noexcept;
    bool done() const noexcept { return done_; }
    P50CompletionPumpResult result() const noexcept { return result_; }
    const P50CompletionObservation& observation() const noexcept
    {
        return observation_;
    }

private:
    int fd_ = -1;
    const CompileJob* expected_job_ = nullptr;
    std::array<uint8_t, kP50CompletionRecordWireSize> bytes_{};
    std::size_t offset_ = 0;
    bool checking_eof_ = false;
    bool done_ = false;
    P50CompletionPumpResult result_ = P50CompletionPumpResult::Pending;
    P50CompletionObservation observation_{};
};

/* Copy only identity genuinely present on a complete P50 CompileJob. */
bool make_p50_completion_record(const CompileJob& job,
                                const uint32_t stats[8],
                                int32_t result_status,
                                P50CompletionDisposition disposition,
                                P50CompletionRecord* out) noexcept;

/* Exact canonical encoding/decoding.  Decoding rejects short/trailing bytes,
   wrong version/size, reserved bits, invalid state, and incomplete identity. */
std::array<uint8_t, kP50CompletionRecordWireSize>
encode_p50_completion_record(const P50CompletionRecord& record);

bool decode_p50_completion_record(std::span<const uint8_t> bytes,
                                  P50CompletionRecord* out) noexcept;

/* Worker-side ordinary-link bridge.  One exact first frame wins; timeout,
   disconnect, another message type, or any identity mismatch is attempt-only.
   MsgChannel performs the fixed-shape/protocol/payload validation first. */
P50CompletionDisposition receive_p50_result_disposition(
    MsgChannel& channel, const CompileJob& expected_job,
    int timeout_seconds) noexcept;

/* The writer emits one atomic <= PIPE_BUF record.  The convenience reader
   performs one bounded pump; event-loop integration should retain the reader
   above until it reaches a non-Pending state. */
bool write_p50_completion_record(int fd,
                                 const P50CompletionRecord& record) noexcept;
P50CompletionObservation read_p50_completion_record(
    int fd, const CompileJob& expected_job) noexcept;

} // namespace icecc::p50

#endif
