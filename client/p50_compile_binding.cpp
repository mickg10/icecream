#include "p50_compile_binding.h"

#include "services/digest128.h"

#include <algorithm>
#include <string_view>

namespace icecc::p50 {
namespace {

bool nonzero(CStoreGuid value) noexcept {
    return std::any_of(value.bytes.begin(), value.bytes.end(),
                       [](uint8_t byte) { return byte != 0; });
}

}  // namespace

bool p50_zstd_compile_admissible(const UseCSMsg& assignment,
                                 int compiler_protocol) noexcept {
    return compiler_protocol == PROTOCOL_VERSION_CACHE_ADVERTISEMENT &&
           !assignment.hostname.empty() && assignment.port != 0 &&
           usecs_cache_handoff_admissible(assignment) &&
           assignment.cache_protocol == CACHE_WIRE_PROTOCOL_V1 &&
           (assignment.cache_profile_mask & CACHE_PROFILE_ZSTD_TU) != 0;
}

CStoreGuid derive_compile_c_store_guid(const CompileJob& job,
                                       uint64_t process_nonce,
                                       uint64_t invocation_nonce) {
    icecc::Digest128Builder digest;
    digest.append(std::string_view{"icecc-p50-compile-c-store-guid-v1"});
    digest.append_u64(job.assignmentEpoch());
    digest.append_u64(job.assignmentNonce());
    digest.append_u32(job.jobID());
    digest.append_u64(process_nonce);
    digest.append_u64(invocation_nonce);
    CStoreGuid result;
    result.bytes = digest.finish().bytes;
    // Digest128 is not expected to yield zero, but zero is the protocol's
    // absence sentinel and must be impossible by construction.
    if (!nonzero(result))
        result.bytes.back() = 1;
    return result;
}

PrepareRequestKey compile_prepare_request(const CompileJob& job) noexcept {
    return PrepareRequestKey{job.assignmentEpoch(), job.assignmentNonce()};
}

std::optional<CompileInputIdentity> bind_compile_input(
    const CompileJob& job, CStoreGuid expected_c_store_guid,
    const ZstdSourceTransferResult& transfer) noexcept {
    if (!job.hasAssignmentIdentity() || job.jobID() == 0 ||
        !nonzero(expected_c_store_guid) ||
        transfer.status != ZstdSourceTransferStatus::Committed ||
        !transfer.committed_input.has_value() ||
        transfer.committed_input->c_store_guid != expected_c_store_guid ||
        transfer.attempts == 0 || transfer.attempts > 2)
        return std::nullopt;

    CompileInputIdentity identity;
    identity.profile = CompileInputIdentity::ZstdTuProfile;
    identity.c_store_guid = transfer.committed_input->c_store_guid.bytes;
    identity.tu_seq = transfer.committed_input->tu_seq.value;
    identity.raw_bytes = transfer.raw_bytes;
    identity.raw_digest = transfer.raw_digest.bytes;
    identity.attempt_id = job.assignmentNonce();
    identity.request_id = job.assignmentNonce();
    if (!identity.validPresent())
        return std::nullopt;
    return identity;
}

}  // namespace icecc::p50
