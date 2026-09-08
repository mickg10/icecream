#include "client/p50_compile_binding.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace icecc::p50;

namespace {

void check(bool value, const char* expression) {
    if (!value)
        throw std::runtime_error(expression);
}

#define CHECK(expression) check((expression), #expression)

struct ChannelPair {
    MsgChannel* client = nullptr;
    MsgChannel* worker = nullptr;
};

ChannelPair make_channel_pair() {
    int fds[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ChannelPair pair;
    std::thread client([&] {
        pair.client = Service::createChannel(
            fds[0], reinterpret_cast<sockaddr*>(&address), sizeof(address));
    });
    std::thread worker([&] {
        pair.worker = Service::createChannel(
            fds[1], reinterpret_cast<sockaddr*>(&address), sizeof(address));
    });
    client.join();
    worker.join();
    CHECK(pair.client != nullptr && pair.worker != nullptr);
    pair.client->protocol = PROTOCOL_VERSION_CACHE_ADVERTISEMENT;
    pair.worker->protocol = PROTOCOL_VERSION_CACHE_ADVERTISEMENT;
    return pair;
}

enum class LegacyEntropyMode { Distinct, Interrupted, Zero, Short, Error };

LegacyEntropyMode legacy_entropy_mode = LegacyEntropyMode::Distinct;
unsigned legacy_entropy_calls = 0;

ssize_t legacy_entropy(void* buffer, size_t size, unsigned) noexcept {
    ++legacy_entropy_calls;
    if (legacy_entropy_mode == LegacyEntropyMode::Interrupted &&
        legacy_entropy_calls == 1) {
        errno = EINTR;
        return -1;
    }
    if (legacy_entropy_mode == LegacyEntropyMode::Short)
        return size == 0 ? 0 : static_cast<ssize_t>(size - 1);
    if (legacy_entropy_mode == LegacyEntropyMode::Error) {
        errno = EIO;
        return -1;
    }
    const unsigned char fill = legacy_entropy_mode == LegacyEntropyMode::Zero
        ? 0
        : static_cast<unsigned char>(legacy_entropy_calls);
    std::memset(buffer, fill, size);
    return static_cast<ssize_t>(size);
}

CompileJob assigned_job(uint64_t epoch = 101, uint64_t nonce = 202) {
    CompileJob job;
    job.setJobID(33);
    job.setAssignmentIdentity(epoch, nonce);
    return job;
}

UseCSMsg admissible_assignment() {
    return UseCSMsg{"x86_64", "127.0.0.1", 10245, 33, true, 9, 0,
                    101, 202, 10246, CACHE_WIRE_REVISION,
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
          std::optional<ProfileId>{ProfileId::ZSTD_ROUTE});

    UseCSMsg mutant = assignment;
    mutant.assignment_nonce_hi = mutant.assignment_nonce_lo = 0;
    CHECK(!p50_zstd_compile_admissible(mutant,
                                       PROTOCOL_VERSION_CACHE_ADVERTISEMENT));
    mutant = assignment;
    mutant.cache_endpoint_port = 0;
    CHECK(!p50_zstd_compile_admissible(mutant,
                                       PROTOCOL_VERSION_CACHE_ADVERTISEMENT));
    mutant = assignment;
    mutant.cache_profile_mask = CACHE_PROFILE_P29V1;
    CHECK(p50_zstd_compile_admissible(mutant,
                                      PROTOCOL_VERSION_CACHE_ADVERTISEMENT));
    CHECK(p50_zstd_selected_profile(mutant,
                                    PROTOCOL_VERSION_CACHE_ADVERTISEMENT) ==
          std::optional<ProfileId>{ProfileId::P29V1});
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
    const uint32_t advertised = CACHE_PROFILE_P29V1 |
                                CACHE_PROFILE_ZSTD_TU |
                                CACHE_PROFILE_ZSTD_ROUTE;
    CHECK(p50_select_cache_profile(advertised,
                                   P50CacheProfileRequest::Default) ==
          CACHE_PROFILE_P29V1);

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
    CHECK(::setenv("ICECC_P50_PROFILE", "P29", 1) == 0);
    CHECK(p50_cache_profile_request_from_env() ==
          P50CacheProfileRequest::Unsupported);

    CHECK(::setenv("ICECC_P50_PROFILE", "P29V1", 1) == 0);
    CHECK(p50_cache_profile_request_from_env() == P50CacheProfileRequest::P29V1);
    CHECK(p50_select_cache_profile(CACHE_PROFILE_P29V1,
                                   P50CacheProfileRequest::P29V1) ==
          CACHE_PROFILE_P29V1);
    CHECK(p50_select_cache_profile(CACHE_PROFILE_ZSTD_TU,
                                   P50CacheProfileRequest::P29V1) == 0);

    CHECK(::setenv("ICECC_P50_PROFILE", "UNSUPPORTED_PROFILE", 1) == 0);
    CHECK(p50_cache_profile_request_from_env() ==
          P50CacheProfileRequest::Unsupported);
    CHECK(p50_select_cache_profile(advertised,
                                   P50CacheProfileRequest::Unsupported) == 0);
    CHECK(p50_select_cache_profile(advertised | UINT32_C(0x00000008),
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

void test_old_scheduler_gets_only_a_local_wire_identity() {
    CompileJob legacy;
    legacy.setJobID(33);
    legacy_entropy_mode = LegacyEntropyMode::Distinct;
    legacy_entropy_calls = 0;
    CHECK(bind_local_legacy_wire_identity_with_provider(legacy, legacy_entropy));
    CHECK(!legacy.hasAssignmentIdentity());
    CHECK(legacy.assignmentEpoch() == 0 && legacy.assignmentNonce() == 0);
    CHECK(legacy.hasCompileIdentity() && legacy.cGuid() != 0 && legacy.tuSeq() == 0);

    const uint64_t first_guid = legacy.cGuid();
    CompileJob another;
    another.setJobID(33);
    CHECK(bind_local_legacy_wire_identity_with_provider(another, legacy_entropy));
    CHECK(another.cGuid() != 0 && another.cGuid() != first_guid);

    CompileJob scheduler_owned;
    scheduler_owned.setJobID(33);
    scheduler_owned.setCompileIdentity(101, 7);
    CHECK(!bind_local_legacy_wire_identity_with_provider(
        scheduler_owned, legacy_entropy));
    CHECK(scheduler_owned.cGuid() == 101 && scheduler_owned.tuSeq() == 7);

    CompileJob fenced;
    fenced.setJobID(33);
    fenced.setAssignmentIdentity(101, 202);
    CHECK(!bind_local_legacy_wire_identity_with_provider(fenced, legacy_entropy));
    CHECK(!fenced.hasCompileIdentity());

    CompileJob partial;
    partial.setJobID(33);
    partial.setAssignmentIdentity(101, 0);
    CHECK(!bind_local_legacy_wire_identity_with_provider(partial, legacy_entropy));
    CHECK(!partial.hasCompileIdentity());

    legacy_entropy_mode = LegacyEntropyMode::Interrupted;
    legacy_entropy_calls = 0;
    CompileJob interrupted;
    interrupted.setJobID(34);
    CHECK(bind_local_legacy_wire_identity_with_provider(
        interrupted, legacy_entropy));
    CHECK(interrupted.hasCompileIdentity());

    for (const LegacyEntropyMode mode : {
             LegacyEntropyMode::Zero,
             LegacyEntropyMode::Short,
             LegacyEntropyMode::Error,
         }) {
        legacy_entropy_mode = mode;
        legacy_entropy_calls = 0;
        CompileJob rejected;
        rejected.setJobID(35);
        CHECK(!bind_local_legacy_wire_identity_with_provider(
            rejected, legacy_entropy));
        CHECK(!rejected.hasCompileIdentity());
    }
}

void test_old_scheduler_current_client_worker_compilefile_round_trip() {
    CompileJob legacy;
    legacy.setJobID(91);
    legacy.setCompilerName("g++");
    legacy.setLanguage(CompileJob::Lang_CXX);
    legacy.setEnvironmentVersion("legacy-scheduler-env");
    legacy.setTargetPlatform("x86_64");
    legacy.setInputFile("legacy.ii");
    legacy.setOutputFile("legacy.o");
    legacy_entropy_mode = LegacyEntropyMode::Distinct;
    legacy_entropy_calls = 0;
    CHECK(bind_local_legacy_wire_identity_with_provider(legacy, legacy_entropy));
    CHECK(!legacy.hasAssignmentIdentity());

    const P50LegacyWireIdentity client_identity{
        legacy.jobID(), legacy.assignmentEpoch(), legacy.assignmentNonce(),
        legacy.cGuid(), legacy.tuSeq()};
    CHECK(client_identity.valid());
    ChannelPair pair = make_channel_pair();
    pair.client->set_p50_legacy_wire_role(P50LegacyWireRole::C);
    pair.worker->set_p50_legacy_wire_role(P50LegacyWireRole::F);
    CHECK(pair.client->set_p50_legacy_wire_identity(client_identity));

    CompileFileMsg outbound(&legacy);
    CHECK(pair.client->send_msg(outbound));
    Msg* wire = pair.worker->get_msg(5, true);
    auto* inbound = dynamic_cast<CompileFileMsg*>(wire);
    CHECK(inbound != nullptr);
    P50LegacyWireIdentity worker_identity;
    CHECK(inbound != nullptr && inbound->legacy_wire_identity(worker_identity));
    CompileJob* decoded = inbound != nullptr ? inbound->takeJob() : nullptr;
    CHECK(decoded != nullptr);
    CHECK(decoded->jobID() == legacy.jobID());
    CHECK(!decoded->hasAssignmentIdentity());
    CHECK(decoded->hasCompileIdentity());
    CHECK(decoded->cGuid() == legacy.cGuid() && decoded->tuSeq() == 0);
    CHECK(worker_identity == client_identity);
    CHECK(pair.worker->set_p50_legacy_wire_identity(worker_identity));

    delete decoded;
    delete wire;
    delete pair.client;
    delete pair.worker;
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
    mutant.profile = static_cast<ProfileId>(4);
    CHECK(!bind_compile_input(job, guid, mutant));
    CHECK(!bind_compile_input(CompileJob{}, guid, transfer));
}

void test_authenticated_sidecar_result_binds_real_identity() {
    const CompileJob job = assigned_job();
    const CStoreGuid guid = Id128::from_u64(0xfeed);
    local::P50SourceTransferResult transfer;
    transfer.code = local::SourceTransferResultCode::Committed;
    transfer.c_store_guid = guid;
    transfer.tu_seq = 0;
    transfer.raw_bytes = 1234;
    transfer.raw_digest = icecc::digest128("sidecar source");
    transfer.attempts = 1;

    const auto identity = bind_compile_input(job, ProfileId::ZSTD_ROUTE, transfer);
    CHECK(identity.has_value());
    CHECK(identity->profile == CompileInputIdentity::ZstdRouteProfile);
    CHECK(identity->c_store_guid == guid.bytes);
    CHECK(identity->tu_seq == 0);
    CHECK(identity->raw_bytes == transfer.raw_bytes);
    CHECK(identity->raw_digest == transfer.raw_digest.bytes);
    CHECK(identity->attempt_id == job.assignmentNonce());
    CHECK(identity->request_id == job.assignmentNonce());

    const auto p29v1 = bind_compile_input(job, ProfileId::P29V1, transfer);
    CHECK(p29v1.has_value());
    CHECK(p29v1->profile == CompileInputIdentity::P29V1Profile);

    transfer.attempts = 3;
    CHECK(!bind_compile_input(job, static_cast<ProfileId>(4), transfer));
    transfer.attempts = 1;
    transfer.code = local::SourceTransferResultCode::Error;
    CHECK(!bind_compile_input(job, ProfileId::ZSTD_TU, transfer));
}

}  // namespace

int main() {
    test_exact_mode_admission();
    test_explicit_profile_selection();
    test_namespace_and_request_are_assignment_bound();
    test_old_scheduler_gets_only_a_local_wire_identity();
    test_old_scheduler_current_client_worker_compilefile_round_trip();
    test_only_exact_commit_binds_compile_selector();
    test_authenticated_sidecar_result_binds_real_identity();
}
