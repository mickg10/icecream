#pragma once

#include "p50_zstd_sender.h"
#include "cache/p50_control_operation.h"

#include "services/comm.h"
#include "services/job.h"

#include <cstdint>
#include <optional>

namespace icecc::p50 {

// Selects the cache source path only when the exact compiler relationship and
// the selected assignment agree on the frozen P50/ZSTD_TU capability.  The
// caller uses this same UseCS hostname for both the compiler and cache-session
// connections, keeping endpoint choice bound to the selected F.
[[nodiscard]] bool p50_zstd_compile_admissible(
    const UseCSMsg& assignment, int compiler_protocol) noexcept;

// Return the one runnable source profile selected by the assignment.  A
// capability mask is not a source selection: both bits, unknown bits, and
// zero are rejected rather than silently choosing a codec.
[[nodiscard]] std::optional<ProfileId> p50_zstd_selected_profile(
    const UseCSMsg& assignment, int compiler_protocol) noexcept;

// Pure assignment-bound namespace derivation.  Product callers supply a
// process/invocation nonce; tests can hold them fixed and mutate each assignment
// field independently.  This namespace remains immutable across the sender's
// one permitted replay.
[[nodiscard]] CStoreGuid derive_compile_c_store_guid(
    const CompileJob& job, uint64_t process_nonce,
    uint64_t invocation_nonce);

[[nodiscard]] PrepareRequestKey compile_prepare_request(
    const CompileJob& job) noexcept;

// Converts only an exact committed sender result into the P50 CompileFile
// selector.  ATTEMPT_ID remains compiler-owner metadata and never enters the
// InputRecordKey; REQUEST_ID is the nonzero assignment-bound local attachment
// request echoed by the authenticated F sidecar relationship.
[[nodiscard]] std::optional<CompileInputIdentity> bind_compile_input(
    const CompileJob& job, CStoreGuid expected_c_store_guid,
    const ZstdSourceTransferResult& transfer) noexcept;

// Bind the selector from the authenticated sidecar's typed result.  The
// sidecar is the C-store owner for this path, so the GUID is taken from the
// result itself rather than synthesized by the wrapper.
[[nodiscard]] std::optional<CompileInputIdentity> bind_compile_input(
    const CompileJob& job, ProfileId profile,
    const local::P50SourceTransferResult& transfer) noexcept;

}  // namespace icecc::p50
