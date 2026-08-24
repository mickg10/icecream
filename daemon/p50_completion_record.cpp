#include "p50_completion_record.h"

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <unistd.h>

namespace icecc::p50 {
namespace {

constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kStatsOffset = kHeaderSize;
constexpr std::size_t kStatusOffset = kStatsOffset + 8 * 4;
constexpr std::size_t kResultOffset = kStatusOffset + 4;
constexpr std::size_t kDispositionOffset = kResultOffset + 4;
constexpr std::size_t kJobIdOffset = kDispositionOffset + 4;
constexpr std::size_t kEpochOffset = kJobIdOffset + 4;
constexpr std::size_t kNonceOffset = kEpochOffset + 8;
constexpr std::size_t kProfileOffset = kNonceOffset + 8;
constexpr std::size_t kGuidOffset = kProfileOffset + 4;
constexpr std::size_t kTuSeqOffset = kGuidOffset + 16;
constexpr std::size_t kRawBytesOffset = kTuSeqOffset + 8;
constexpr std::size_t kRawDigestOffset = kRawBytesOffset + 8;
constexpr std::size_t kAttemptOffset = kRawDigestOffset + 16;
constexpr std::size_t kRequestOffset = kAttemptOffset + 8;
static_assert(kRequestOffset + 8 == kP50CompletionRecordWireSize);

bool nonzero(const std::array<uint8_t, 16>& value) {
    return std::any_of(value.begin(), value.end(), [](uint8_t byte) {
        return byte != 0;
    });
}

bool valid_result(P50CompletionResult result, int32_t status) {
    if (result == P50CompletionResult::Succeeded)
        return status == 0;
    if (result == P50CompletionResult::Failed)
        return status != 0;
    return false;
}

bool valid_record(const P50CompletionRecord& record) {
    return record.disposition == P50CompletionDisposition::Completed &&
           valid_result(record.result, record.result_status) &&
           record.job_id != 0 && record.assignment_epoch != 0 &&
           record.assignment_nonce != 0 &&
           record.input_profile == CompileInputIdentity::ZstdTuProfile &&
           nonzero(record.c_store_guid) && record.attempt_id != 0 &&
           record.request_id != 0;
}

void put_u16(std::array<uint8_t, kP50CompletionRecordWireSize>& bytes,
             std::size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<uint8_t>(value);
}

void put_u32(std::array<uint8_t, kP50CompletionRecordWireSize>& bytes,
             std::size_t offset, uint32_t value) {
    for (unsigned i = 0; i != 4; ++i)
        bytes[offset + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
}

void put_u64(std::array<uint8_t, kP50CompletionRecordWireSize>& bytes,
             std::size_t offset, uint64_t value) {
    for (unsigned i = 0; i != 8; ++i)
        bytes[offset + i] = static_cast<uint8_t>(value >> (56 - 8 * i));
}

uint16_t get_u16(std::span<const uint8_t> bytes, std::size_t offset) {
    return static_cast<uint16_t>((uint16_t(bytes[offset]) << 8) |
                                 uint16_t(bytes[offset + 1]));
}

uint32_t get_u32(std::span<const uint8_t> bytes, std::size_t offset) {
    uint32_t result = 0;
    for (unsigned i = 0; i != 4; ++i)
        result = (result << 8) | bytes[offset + i];
    return result;
}

uint64_t get_u64(std::span<const uint8_t> bytes, std::size_t offset) {
    uint64_t result = 0;
    for (unsigned i = 0; i != 8; ++i)
        result = (result << 8) | bytes[offset + i];
    return result;
}

void put_bytes(std::array<uint8_t, kP50CompletionRecordWireSize>& bytes,
               std::size_t offset, const std::array<uint8_t, 16>& value) {
    std::copy(value.begin(), value.end(), bytes.begin() + offset);
}

void get_bytes(std::span<const uint8_t> bytes, std::size_t offset,
               std::array<uint8_t, 16>* out) {
    std::copy_n(bytes.begin() + offset, out->size(), out->begin());
}

bool same_identity(const P50CompletionRecord& record, const CompileJob& job) {
    const CompileInputIdentity& input = job.compileInputIdentity();
    return record.job_id == job.jobID() &&
           record.assignment_epoch == job.assignmentEpoch() &&
           record.assignment_nonce == job.assignmentNonce() &&
           record.input_profile == input.profile &&
           record.c_store_guid == input.c_store_guid &&
           record.tu_seq == input.tu_seq && record.raw_bytes == input.raw_bytes &&
           record.raw_digest == input.raw_digest &&
           record.attempt_id == input.attempt_id &&
           record.request_id == input.request_id;
}

bool read_full(int fd, uint8_t* bytes, std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset != size) {
        const ssize_t count = ::read(fd, bytes + offset, size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool only_eof(int fd) noexcept {
    uint8_t trailing = 0;
    for (;;) {
        const ssize_t count = ::read(fd, &trailing, 1);
        if (count == 0)
            return true;
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
}

} // namespace

bool make_p50_completion_record(const CompileJob& job, const uint32_t stats[8],
                                int32_t result_status,
                                P50CompletionRecord* out) noexcept {
    if (out == nullptr || stats == nullptr || !job.compileInputIdentityValid() ||
        !job.usesP50Input() || job.jobID() == 0 || !job.hasAssignmentIdentity())
        return false;

    P50CompletionRecord result;
    std::copy_n(stats, 8, result.stats);
    result.result_status = result_status;
    result.result = result_status == 0 ? P50CompletionResult::Succeeded
                                       : P50CompletionResult::Failed;
    result.disposition = P50CompletionDisposition::Completed;
    result.job_id = job.jobID();
    result.assignment_epoch = job.assignmentEpoch();
    result.assignment_nonce = job.assignmentNonce();
    const CompileInputIdentity& input = job.compileInputIdentity();
    result.input_profile = input.profile;
    result.c_store_guid = input.c_store_guid;
    result.tu_seq = input.tu_seq;
    result.raw_bytes = input.raw_bytes;
    result.raw_digest = input.raw_digest;
    result.attempt_id = input.attempt_id;
    result.request_id = input.request_id;
    if (!valid_record(result))
        return false;
    *out = result;
    return true;
}

std::array<uint8_t, kP50CompletionRecordWireSize>
encode_p50_completion_record(const P50CompletionRecord& record) {
    if (!valid_record(record))
        throw std::invalid_argument("invalid P50 completion record");

    std::array<uint8_t, kP50CompletionRecordWireSize> bytes{};
    put_u32(bytes, 0, kP50CompletionMagic);
    put_u16(bytes, 4, kP50CompletionVersion);
    put_u16(bytes, 6, static_cast<uint16_t>(kP50CompletionRecordWireSize));
    put_u32(bytes, 8, 0);
    for (unsigned i = 0; i != 8; ++i)
        put_u32(bytes, kStatsOffset + 4 * i, record.stats[i]);
    put_u32(bytes, kStatusOffset, static_cast<uint32_t>(record.result_status));
    put_u32(bytes, kResultOffset, static_cast<uint32_t>(record.result));
    put_u32(bytes, kDispositionOffset, static_cast<uint32_t>(record.disposition));
    put_u32(bytes, kJobIdOffset, record.job_id);
    put_u64(bytes, kEpochOffset, record.assignment_epoch);
    put_u64(bytes, kNonceOffset, record.assignment_nonce);
    put_u32(bytes, kProfileOffset, record.input_profile);
    put_bytes(bytes, kGuidOffset, record.c_store_guid);
    put_u64(bytes, kTuSeqOffset, record.tu_seq);
    put_u64(bytes, kRawBytesOffset, record.raw_bytes);
    put_bytes(bytes, kRawDigestOffset, record.raw_digest);
    put_u64(bytes, kAttemptOffset, record.attempt_id);
    put_u64(bytes, kRequestOffset, record.request_id);
    return bytes;
}

bool decode_p50_completion_record(std::span<const uint8_t> bytes,
                                  P50CompletionRecord* out) noexcept {
    if (out == nullptr || bytes.size() != kP50CompletionRecordWireSize)
        return false;
    if (get_u32(bytes, 0) != kP50CompletionMagic ||
        get_u16(bytes, 4) != kP50CompletionVersion ||
        get_u16(bytes, 6) != kP50CompletionRecordWireSize ||
        get_u32(bytes, 8) != 0)
        return false;

    P50CompletionRecord result;
    for (unsigned i = 0; i != 8; ++i)
        result.stats[i] = get_u32(bytes, kStatsOffset + 4 * i);
    result.result_status = static_cast<int32_t>(get_u32(bytes, kStatusOffset));
    result.result = static_cast<P50CompletionResult>(get_u32(bytes, kResultOffset));
    result.disposition = static_cast<P50CompletionDisposition>(
        get_u32(bytes, kDispositionOffset));
    result.job_id = get_u32(bytes, kJobIdOffset);
    result.assignment_epoch = get_u64(bytes, kEpochOffset);
    result.assignment_nonce = get_u64(bytes, kNonceOffset);
    result.input_profile = get_u32(bytes, kProfileOffset);
    get_bytes(bytes, kGuidOffset, &result.c_store_guid);
    result.tu_seq = get_u64(bytes, kTuSeqOffset);
    result.raw_bytes = get_u64(bytes, kRawBytesOffset);
    get_bytes(bytes, kRawDigestOffset, &result.raw_digest);
    result.attempt_id = get_u64(bytes, kAttemptOffset);
    result.request_id = get_u64(bytes, kRequestOffset);
    if (!valid_record(result))
        return false;
    *out = result;
    return true;
}

bool write_p50_completion_record(int fd,
                                 const P50CompletionRecord& record) noexcept {
    try {
        const auto bytes = encode_p50_completion_record(record);
        std::size_t offset = 0;
        while (offset != bytes.size()) {
            const ssize_t count = ::write(fd, bytes.data() + offset,
                                          bytes.size() - offset);
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

P50CompletionObservation read_p50_completion_record(
    int fd, const CompileJob& expected_job) noexcept {
    P50CompletionObservation observation;
    std::array<uint8_t, kP50CompletionRecordWireSize> bytes{};
    if (!read_full(fd, bytes.data(), bytes.size()) || !only_eof(fd))
        return observation;
    P50CompletionRecord record;
    if (!decode_p50_completion_record(bytes, &record) ||
        !same_identity(record, expected_job))
        return observation;
    observation.disposition = P50CompletionDisposition::Completed;
    observation.record = record;
    return observation;
}

} // namespace icecc::p50
