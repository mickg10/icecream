#include "p50_completion_record.h"
#include "workit.h"

#include <comm.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
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

struct ChannelPair {
    MsgChannel *left = nullptr;
    MsgChannel *right = nullptr;
    ChannelPair() = default;
    ChannelPair(const ChannelPair&) = delete;
    ChannelPair& operator=(const ChannelPair&) = delete;
    ChannelPair(ChannelPair&& other) noexcept
        : left(other.left), right(other.right)
    {
        other.left = other.right = nullptr;
    }
    ~ChannelPair()
    {
        delete left;
        delete right;
    }
};

ChannelPair make_channel_pair()
{
    int fds[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
            "socketpair failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ChannelPair pair;
    std::thread left([&] {
        pair.left = Service::createChannel(
            fds[0], reinterpret_cast<sockaddr *>(&address), sizeof(address));
    });
    std::thread right([&] {
        pair.right = Service::createChannel(
            fds[1], reinterpret_cast<sockaddr *>(&address), sizeof(address));
    });
    left.join();
    right.join();
    require(pair.left != nullptr && pair.right != nullptr,
            "channel handshake failed");
    pair.left->protocol = pair.right->protocol = PROTOCOL_VERSION;
    return pair;
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
    job.setLanguage(CompileJob::Lang_C);
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
    input.attempt_id = job.assignmentNonce();
    input.request_id = job.assignmentNonce();
    job.setCompileInputIdentity(input);
    return job;
}

P50CompletionRecord complete_record(
    const CompileJob& job,
    P50CompletionDisposition disposition = P50CompletionDisposition::Accepted,
    int32_t status = 0) {
    const uint32_t stats[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t bound_stats[8];
    std::copy_n(stats, 8, bound_stats);
    bound_stats[JobStatistics::exit_code] = static_cast<uint32_t>(status);
    P50CompletionRecord record;
    require(make_p50_completion_record(
                job, bound_stats, status, disposition, &record),
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
            "valid split record was not accepted");
    require(observed.disposition == P50CompletionDisposition::Accepted &&
                observed.terminal_disposition() &&
                !observed.closes_logical_job(),
            "accepted record did not preserve its terminal disposition");
}

void test_all_dispositions_and_statuses() {
    const CompileJob job = complete_job();
    const P50CompletionObservation accepted = observe(
        encode_p50_completion_record(complete_record(
            job, P50CompletionDisposition::Accepted)), job);
    require(accepted.valid() && accepted.terminal_disposition() &&
                !accepted.closes_logical_job(),
            "accepted disposition was not terminal");

    const P50CompletionObservation cancelled = observe(
        encode_p50_completion_record(complete_record(
            job, P50CompletionDisposition::DefinitiveCancel, 73)), job);
    require(cancelled.valid() && cancelled.terminal_disposition() &&
                !cancelled.closes_logical_job() &&
                cancelled.record.result == P50CompletionResult::Failed,
            "definitive cancel did not preserve failed result status");

    const P50CompletionObservation attempt_only = observe(
        encode_p50_completion_record(complete_record(
            job, P50CompletionDisposition::AttemptCancelOnly)), job);
    require(attempt_only.valid() && !attempt_only.terminal_disposition() &&
                !attempt_only.closes_logical_job(),
            "attempt-only record became terminal");
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
    put_u32(disposition, 52, 99);
    require_attempt_only(observe(disposition, job),
                         "unknown disposition was accepted from a worker");
    auto result = canonical;
    put_u32(result, 48, 99);
    require_attempt_only(observe(result, job), "unknown result state was accepted");
    auto status_mismatch = canonical;
    put_u32(status_mismatch, 44, 17);
    require_attempt_only(observe(status_mismatch, job),
                         "result/status mismatch was accepted");
    auto stats_mismatch = canonical;
    put_u32(stats_mismatch, 12 + 4 * JobStatistics::exit_code, 17);
    require_attempt_only(observe(stats_mismatch, job),
                         "statistics/status mismatch was accepted");
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

    require(::pipe(fds) == 0, "pipe failed");
    require(write_p50_completion_record(fds[1], record),
            "open-writer fixture failed");
    P50CompletionRecordReader reader(fds[0], job);
    ::alarm(2);
    require(reader.pump() == P50CompletionPumpResult::Pending,
            "open writer was accepted before EOF");
    ::alarm(0);
    ::close(fds[1]);
    require(reader.pump() == P50CompletionPumpResult::Accepted,
            "closed exact writer was not accepted");
    const P50CompletionObservation open_writer = reader.observation();
    ::close(fds[0]);
    require(open_writer.valid(),
            "reader waited for EOF after one atomic record");

    require(::pipe(fds) == 0, "pipe failed");
    require(write_p50_completion_record(fds[1], record),
            "delayed-trailing fixture failed");
    P50CompletionRecordReader trailing_reader(fds[0], job);
    require(trailing_reader.pump() == P50CompletionPumpResult::Pending,
            "open exact writer did not wait for EOF");
    const uint8_t trailing_byte = 0;
    require(::write(fds[1], &trailing_byte, sizeof(trailing_byte)) == 1,
            "delayed trailing byte write failed");
    ::close(fds[1]);
    require(trailing_reader.pump() ==
                P50CompletionPumpResult::AttemptCancelOnly,
            "delayed trailing byte was ignored");
    ::close(fds[0]);

    const auto bytes = encode_p50_completion_record(record);
    require(::pipe(fds) == 0, "pipe failed");
    require(::write(fds[1], bytes.data(), bytes.size() - 1) ==
                static_cast<ssize_t>(bytes.size() - 1),
            "partial open-writer fixture failed");
    ::alarm(2);
    const P50CompletionObservation partial =
        read_p50_completion_record(fds[0], job);
    ::alarm(0);
    ::close(fds[0]);
    ::close(fds[1]);
    require_attempt_only(partial,
                         "partial open writer blocked or became terminal");
}

void test_legacy_job_is_not_a_p50_record() {
    CompileJob legacy;
    legacy.setJobID(7);
    const uint32_t stats[8] = {};
    P50CompletionRecord record;
    require(!make_p50_completion_record(
                legacy, stats, 0, P50CompletionDisposition::Accepted, &record),
            "legacy job entered P50 record path");

    CompileJob mismatched = complete_job();
    CompileInputIdentity input = mismatched.compileInputIdentity();
    ++input.request_id;
    mismatched.setCompileInputIdentity(input);
    require(!make_p50_completion_record(
                mismatched, stats, 0, P50CompletionDisposition::Accepted,
                &record),
            "assignment-nonce mismatch entered P50 record path");
}

void test_result_disposition_receiver()
{
    const CompileJob job = complete_job();
    {
        ChannelPair pair = make_channel_pair();
        require(pair.left->send_msg(ResultDispositionMsg(
                    job, ResultDispositionMsg::Accepted)),
                "accepted disposition send failed");
        require(pair.left->send_msg(ResultDispositionMsg(
                    job, ResultDispositionMsg::DefinitiveCancel)),
                "conflicting second disposition send failed");
        require(receive_p50_result_disposition(*pair.right, job, 2) ==
                    P50CompletionDisposition::Accepted,
                "first exact accepted disposition did not win");
    }
    {
        ChannelPair pair = make_channel_pair();
        require(pair.left->send_msg(ResultDispositionMsg(
                    job, ResultDispositionMsg::DefinitiveCancel)),
                "definitive-cancel disposition send failed");
        require(receive_p50_result_disposition(*pair.right, job, 2) ==
                    P50CompletionDisposition::DefinitiveCancel,
                "exact definitive cancellation was not forwarded");
    }
    {
        ChannelPair pair = make_channel_pair();
        CompileJob other = job;
        other.setJobID(job.jobID() + 1);
        require(pair.left->send_msg(ResultDispositionMsg(
                    other, ResultDispositionMsg::Accepted)),
                "mismatched disposition fixture send failed");
        require(receive_p50_result_disposition(*pair.right, job, 2) ==
                    P50CompletionDisposition::AttemptCancelOnly,
                "mismatched disposition became terminal");
    }
    {
        ChannelPair pair = make_channel_pair();
        require(pair.left->send_msg(EndMsg()),
                "unexpected-frame fixture send failed");
        require(receive_p50_result_disposition(*pair.right, job, 2) ==
                    P50CompletionDisposition::AttemptCancelOnly,
                "unexpected frame became terminal");
    }
    {
        ChannelPair pair = make_channel_pair();
        delete pair.left;
        pair.left = nullptr;
        require(receive_p50_result_disposition(*pair.right, job, 1) ==
                    P50CompletionDisposition::AttemptCancelOnly,
                "submitter disconnect became terminal");
    }
}

} // namespace

int main() {
    test_exact_canonical_record();
    test_all_dispositions_and_statuses();
    test_short_trailing_header_and_state_rejection();
    test_identity_mutations();
    test_missing_and_writer();
    test_legacy_job_is_not_a_p50_record();
    test_result_disposition_receiver();
    std::cout << "p50 completion record tests: PASS\n";
}
