#include "client/p50_compile_binding.h"

#include <cstdlib>
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

    CHECK(p50_zstd_selected_profile(assignment,
                                    PROTOCOL_VERSION_CACHE_ADVERTISEMENT) ==
          std::optional<ProfileId>{ProfileId::ZSTD_TU});
    assignment.cache_profile_mask = CACHE_PROFILE_ZSTD_ROUTE;
    CHECK(p50_zstd_compile_admissible(assignment,
                                      PROTOCOL_VERSION_CACHE_ADVERTISEMENT));
    CHECK(p50_zstd_selected_profile(assignment,
                                    PROTOCOL_VERSION_CACHE_ADVERTISEMENT) ==
          std::optional<ProfileId>{ProfileId::Z3_LONG});

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
    CHECK(p50_zstd_compile_admissible(mutant,
                                      PROTOCOL_VERSION_CACHE_ADVERTISEMENT));
    CHECK(p50_zstd_selected_profile(mutant,
                                    PROTOCOL_VERSION_CACHE_ADVERTISEMENT) ==
          std::optional<ProfileId>{ProfileId::P29});
    mutant.cache_profile_mask = CACHE_PROFILE_ZSTD_TU | CACHE_PROFILE_ZSTD_ROUTE;
    CHECK(!p50_zstd_compile_admissible(mutant,
                                       PROTOCOL_VERSION_CACHE_ADVERTISEMENT));
    mutant.cache_profile_mask = CACHE_PROFILE_ZSTD_ROUTE | UINT32_C(0x80000000);
    CHECK(!p50_zstd_compile_admissible(mutant,
                                       PROTOCOL_VERSION_CACHE_ADVERTISEMENT));
    mutant = assignment;
    mutant.hostname.clear();
    CHECK(!p50_zstd_compile_admissible(mutant,
                                       PROTOCOL_VERSION_CACHE_ADVERTISEMENT));
    CHECK(!p50_zstd_compile_admissible(assignment,
                                       PROTOCOL_VERSION_ASSIGNMENT_FENCE));
}

void test_explicit_profile_selection() {
    CHECK(::unsetenv("ICECC_P50_PROFILE") == 0);
    CHECK(p50_cache_profile_request_from_env() ==
          P50CacheProfileRequest::Default);
    const uint32_t advertised =
        CACHE_PROFILE_ZSTD_TU | CACHE_PROFILE_ZSTD_ROUTE;
    CHECK(p50_select_cache_profile(advertised,
                                   P50CacheProfileRequest::Default) ==
          CACHE_PROFILE_ZSTD_ROUTE);

    CHECK(::setenv("ICECC_P50_PROFILE", "ZSTD_TU", 1) == 0);
    CHECK(p50_cache_profile_request_from_env() ==
          P50CacheProfileRequest::ZSTD_TU);
    CHECK(p50_select_cache_profile(advertised,
                                   P50CacheProfileRequest::ZSTD_TU) ==
          CACHE_PROFILE_ZSTD_TU);

    CHECK(::setenv("ICECC_P50_PROFILE", "ZSTD_ROUTE", 1) == 0);
    CHECK(p50_cache_profile_request_from_env() ==
          P50CacheProfileRequest::ZSTD_ROUTE);
    CHECK(p50_select_cache_profile(advertised,
                                   P50CacheProfileRequest::ZSTD_ROUTE) ==
          CACHE_PROFILE_ZSTD_ROUTE);
    CHECK(p50_select_cache_profile(CACHE_PROFILE_ZSTD_TU,
                                   P50CacheProfileRequest::ZSTD_ROUTE) == 0);
    CHECK(p50_cache_profile_request_from_env() != P50CacheProfileRequest::P29);
    CHECK(::setenv("ICECC_P50_PROFILE", "P29", 1) == 0);
    CHECK(p50_cache_profile_request_from_env() == P50CacheProfileRequest::P29);
    CHECK(p50_select_cache_profile(CACHE_PROFILE_P29,
                                   P50CacheProfileRequest::P29) == CACHE_PROFILE_P29);
    CHECK(p50_select_cache_profile(CACHE_PROFILE_ZSTD_TU,
                                   P50CacheProfileRequest::P29) == 0);

    CHECK(::setenv("ICECC_P50_PROFILE", "UNSUPPORTED_PROFILE", 1) == 0);
    CHECK(p50_cache_profile_request_from_env() ==
          P50CacheProfileRequest::Unsupported);
    CHECK(p50_select_cache_profile(advertised,
                                   P50CacheProfileRequest::Unsupported) == 0);
    CHECK(p50_select_cache_profile(advertised | CACHE_PROFILE_Z3_SHARED_LONG,
                                   P50CacheProfileRequest::ZSTD_ROUTE) == 0);
    CHECK(::unsetenv("ICECC_P50_PROFILE") == 0);
}

void test_namespace_and_request_are_assignment_bound() {
    const CompileJob first = assigned_job();
    const CompileJob second = assigned_job(101, 203);
    const CStoreGuid first_guid = derive_compile_c_store_guid(first, 7, 8);
    CHECK(first_guid != CStoreGuid{});
    CHECK(store_identity_guid_valid_for_role(
        first_guid.bytes, kStoreIdentityClientRole));
    CHECK(!store_identity_guid_valid_for_role(
        first_guid.bytes, kStoreIdentityFileRole));
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
    mutant = transfer;
    mutant.profile = ProfileId::GRZ;
    CHECK(!bind_compile_input(job, guid, mutant));
    CHECK(!bind_compile_input(CompileJob{}, guid, transfer));
}

}  // namespace

int main() {
    test_exact_mode_admission();
    test_explicit_profile_selection();
    test_namespace_and_request_are_assignment_bound();
    test_only_exact_commit_binds_compile_selector();
}
