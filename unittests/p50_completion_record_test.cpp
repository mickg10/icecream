#include "p50_completion_record.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace icecc::p50;

/* The focused standalone build does not need host-platform discovery. */
std::string determine_platform() { return "test-platform"; }

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "p50_completion_record_test: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition)
        fail(message);
}

void put_u32(std::array<uint8_t, kP50CompletionRecordWireSize>& bytes,
             std::size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<uint8_t>(value);
}

CompileJob complete_job() {
    CompileJob job;
    job.setJobID(0x10203040);
    job.setAssignmentIdentity(0x0102030405060708ULL,
                              0x1112131415161718ULL);
    CompileInputIdentity input;
    input.profile = CompileInputIdentity::ZstdTuProfile;
    for (unsigned i = 0; i != input.c_store_guid.size(); ++i) {
        input.c_store_guid[i] = static_cast<uint8_t>(0x20 + i);
        input.raw_digest[i] = static_cast<uint8_t>(0x40 + 2 * i);
    }
    input.tu_seq = 0x2122232425262728ULL;
    input.raw_bytes = 0x3132333435363738ULL;
    input.attempt_id = 0x4142434445464748ULL;
    input.request_id = 0x5152535455565758ULL;
    job.setCompileInputIdentity(input);
    return job;
}

P50CompletionRecord complete_record(const CompileJob& job) {
    const uint32_t stats[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    P50CompletionRecord record;
    require(make_p50_completion_record(job, stats, 0, &record),
            "complete P50 job did not produce a record");
    return record;
}

P50CompletionObservation observe(std::span<const uint8_t> bytes,
                                 const CompileJob& job) {
    int fds[2];
    require(::pipe(fds) == 0, "pipe failed");
    std::size_t offset = 0;
    while (offset != bytes.size()) {
        const std::size_t step = std::min<std::size_t>(7, bytes.size() - offset);
        require(::write(fds[1], bytes.data() + offset, step) ==
                    static_cast<ssize_t>(step),
                "split fixture write failed");
        offset += step;
    }
    ::close(fds[1]);
    P50CompletionObservation result = read_p50_completion_record(fds[0], job);
    ::close(fds[0]);
    return result;
}

void require_attempt_only(const P50CompletionObservation& observation,
                          std::string_view message) {
    require(observation.disposition == P50CompletionDisposition::AttemptCancelOnly &&
                !observation.closes_logical_job(),
            message);
}

void test_exact_canonical_record() {
    const CompileJob job = complete_job();
    const P50CompletionRecord expected = complete_record(job);
    const auto bytes = encode_p50_completion_record(expected);
    require(bytes.size() == kP50CompletionRecordWireSize,
            "wire record size changed");
    require(bytes[0] == 'P' && bytes[1] == '5' && bytes[2] == '0' &&
                bytes[3] == 'C',
            "canonical magic changed");
    require(bytes[4] == 0 && bytes[5] == kP50CompletionVersion,
            "canonical version changed");
    require(bytes[6] == 0 && bytes[7] == kP50CompletionRecordWireSize,
            "canonical size changed");
    P50CompletionRecord decoded;
    require(decode_p50_completion_record(bytes, &decoded),
            "canonical record did not decode");
    require(encode_p50_completion_record(decoded) == bytes,
            "canonical record did not round-trip");
    const P50CompletionObservation observed = observe(bytes, job);
    require(observed.valid() && observed.record.result_status == 0,
            "valid split record was not completed");
    require(!observed.closes_logical_job(),
            "reusable observation unexpectedly owns lifecycle closure");
}

void test_short_trailing_header_and_state_rejection() {
    const CompileJob job = complete_job();
    const auto canonical = encode_p50_completion_record(complete_record(job));

    require_attempt_only(observe(std::span(canonical).first(canonical.size() - 1), job),
                         "short record was accepted");
    auto trailing = std::vector<uint8_t>(canonical.begin(), canonical.end());
    trailing.push_back(0);
    require_attempt_only(observe(trailing, job), "trailing record byte was accepted");

    auto version = canonical;
    version[5]++;
    require_attempt_only(observe(version, job), "wrong version was accepted");
    auto size = canonical;
    size[7]--;
    require_attempt_only(observe(size, job), "wrong size was accepted");
    auto reserved = canonical;
    reserved[11] = 1;
    require_attempt_only(observe(reserved, job), "reserved bits were accepted");
    auto disposition = canonical;
    put_u32(disposition, 52, static_cast<uint32_t>(
                                  P50CompletionDisposition::AttemptCancelOnly));
    require_attempt_only(observe(disposition, job),
                         "attempt-only disposition was accepted from a worker");
    auto result = canonical;
    put_u32(result, 48, 99);
    require_attempt_only(observe(result, job), "unknown result state was accepted");
}

void test_identity_mutations() {
    const CompileJob job = complete_job();
    const auto canonical = encode_p50_completion_record(complete_record(job));
    const std::array<std::size_t, 11> offsets = {
        56, 60, 68, 76, 80, 96, 104, 112, 128, 136,  // all wire identities
        12,                                            // stats are not identity
    };
    for (const std::size_t offset : offsets) {
        auto mutated = canonical;
        mutated[offset] ^= 1;
        const P50CompletionObservation observation = observe(mutated, job);
        if (offset == 12)
            require(observation.valid(), "stats mutation rejected as identity");
        else
            require_attempt_only(observation, "identity mutation crossed parent seam");
    }
}

void test_missing_and_writer() {
    const CompileJob job = complete_job();
    const P50CompletionRecord record = complete_record(job);
    int fds[2];
    require(::pipe(fds) == 0, "pipe failed");
    ::close(fds[1]);
    const P50CompletionObservation missing = read_p50_completion_record(fds[0], job);
    ::close(fds[0]);
    require_attempt_only(missing, "missing record was not attempt-only");

    require(::pipe(fds) == 0, "pipe failed");
    require(write_p50_completion_record(fds[1], record),
            "full writer failed");
    ::close(fds[1]);
    const P50CompletionObservation written = read_p50_completion_record(fds[0], job);
    ::close(fds[0]);
    require(written.valid(), "writer output was not read as complete");
}

void test_legacy_job_is_not_a_p50_record() {
    CompileJob legacy;
    legacy.setJobID(7);
    const uint32_t stats[8] = {};
    P50CompletionRecord record;
    require(!make_p50_completion_record(legacy, stats, 0, &record),
            "legacy job entered P50 record path");
}

} // namespace

int main() {
    test_exact_canonical_record();
    test_short_trailing_header_and_state_rejection();
    test_identity_mutations();
    test_missing_and_writer();
    test_legacy_job_is_not_a_p50_record();
    std::cout << "p50 completion record tests: PASS\n";
}
