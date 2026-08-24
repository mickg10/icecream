#include "client/p50_compile_binding.h"

#include <stdexcept>

using namespace icecc::p50;

namespace {

void check(bool value, const char* expression) {
    if (!value)
        throw std::runtime_error(expression);
}

#define CHECK(expression) check((expression), #expression)

CompileJob assigned_job(uint64_t epoch = 101, uint64_t nonce = 202) {
    CompileJob job;
    job.setJobID(33);
    job.setAssignmentIdentity(epoch, nonce);
    return job;
}

UseCSMsg admissible_assignment() {
    return UseCSMsg{"x86_64", "127.0.0.1", 10245, 33, true, 9, 0,
                    101, 202, 10246, CACHE_WIRE_PROTOCOL_V1,
                    CACHE_PROFILE_ZSTD_TU};
}

void test_exact_mode_admission() {
    UseCSMsg assignment = admissible_assignment();
    CHECK(p50_zstd_compile_admissible(assignment,
                                      PROTOCOL_VERSION_CACHE_ADVERTISEMENT));

    UseCSMsg mutant = assignment;
    mutant.assignment_nonce_hi = mutant.assignment_nonce_lo = 0;
    CHECK(!p50_zstd_compile_admissible(mutant,
                                       PROTOCOL_VERSION_CACHE_ADVERTISEMENT));
    mutant = assignment;
    mutant.cache_endpoint_port = 0;
    CHECK(!p50_zstd_compile_admissible(mutant,
                                       PROTOCOL_VERSION_CACHE_ADVERTISEMENT));
    mutant = assignment;
    mutant.cache_profile_mask = CACHE_PROFILE_P29;
    CHECK(!p50_zstd_compile_admissible(mutant,
                                       PROTOCOL_VERSION_CACHE_ADVERTISEMENT));
    mutant = assignment;
    mutant.hostname.clear();
    CHECK(!p50_zstd_compile_admissible(mutant,
                                       PROTOCOL_VERSION_CACHE_ADVERTISEMENT));
    CHECK(!p50_zstd_compile_admissible(assignment,
                                       PROTOCOL_VERSION_ASSIGNMENT_FENCE));
}

void test_namespace_and_request_are_assignment_bound() {
    const CompileJob first = assigned_job();
    const CompileJob second = assigned_job(101, 203);
    const CStoreGuid first_guid = derive_compile_c_store_guid(first, 7, 8);
    CHECK(first_guid != CStoreGuid{});
    CHECK(first_guid == derive_compile_c_store_guid(first, 7, 8));
    CHECK(first_guid != derive_compile_c_store_guid(second, 7, 8));
    CHECK(first_guid != derive_compile_c_store_guid(first, 7, 9));
    CHECK(compile_prepare_request(first) == (PrepareRequestKey{101, 202}));
}

void test_only_exact_commit_binds_compile_selector() {
    const CompileJob job = assigned_job();
    const CStoreGuid guid = derive_compile_c_store_guid(job, 7, 8);
    ZstdSourceTransferResult transfer;
    transfer.status = ZstdSourceTransferStatus::Committed;
    transfer.committed_input = InputRecordKey{guid, TuSeq{44}};
    transfer.raw_bytes = 1234;
    transfer.raw_digest = icecc::digest128("exact source");
    transfer.attempts = 2;

    const auto identity = bind_compile_input(job, guid, transfer);
    CHECK(identity.has_value());
    CHECK(identity->profile == CompileInputIdentity::ZstdTuProfile);
    CHECK(identity->c_store_guid == guid.bytes);
    CHECK(identity->tu_seq == 44);
    CHECK(identity->raw_bytes == 1234);
    CHECK(identity->raw_digest == transfer.raw_digest.bytes);
    CHECK(identity->attempt_id == job.assignmentNonce());
    CHECK(identity->request_id == job.assignmentNonce());
    CHECK(identity->validPresent());

    ZstdSourceTransferResult mutant = transfer;
    mutant.status = ZstdSourceTransferStatus::DeadlineExceeded;
    CHECK(!bind_compile_input(job, guid, mutant));
    mutant = transfer;
    mutant.committed_input.reset();
    CHECK(!bind_compile_input(job, guid, mutant));
    mutant = transfer;
    mutant.committed_input->c_store_guid = Id128::from_u64(999);
    CHECK(!bind_compile_input(job, guid, mutant));
    mutant = transfer;
    mutant.attempts = 3;
    CHECK(!bind_compile_input(job, guid, mutant));
    CHECK(!bind_compile_input(CompileJob{}, guid, transfer));
}

}  // namespace

int main() {
    test_exact_mode_admission();
    test_namespace_and_request_are_assignment_bound();
    test_only_exact_commit_binds_compile_selector();
}
