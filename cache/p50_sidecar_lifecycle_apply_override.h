#pragma once

// Pure parse-and-clamp logic for the ICECC_P50_SIDECAR_LIFECYCLE_APPLY_MS
// diagnostic override (see cache/p50_cache_service.cpp, run()).
//
// This exists as a free function, with no dependency on SidecarRuntime's
// internals, specifically so a unit test can exercise every parse/clamp/
// reject outcome directly, without constructing a SidecarRuntime. It mirrors
// daemon/p50_lifecycle_timeout_override.h exactly (same rationale, same
// strtoll-based parse), just with the sidecar's own env var name, default,
// and clamp range.
//
// Context: InputLifecycle actions (e.g. CloseAcceptedJob) carry no
// daemon-supplied deadline on the wire. Before this diagnostic, the sidecar
// reused its 500ms pre-decode handshake deadline (kHandshakeMilliseconds) as
// the budget for the owner-apply + reply + ack sequence of these ops too, even
// though decode had already consumed part of that window. Under load this
// occasionally landed a few ms past 500ms and handle_connection silently
// closed the connection with no reply at all, which the daemon reads as EOF
// (status=Disconnected) -- a relationship-fatal, non-retriable outcome that
// retires the whole F sidecar for a single slow lifecycle reply.
//
// The fix is a *separate*, sidecar-local budget for this specific window,
// measured from decode (not from the handshake deadline), sized to match the
// daemon's own default budget for these ops
// (ICECC_P50_INPUT_LIFECYCLE_TIMEOUT_MS, daemon/p50_lifecycle_timeout_override.h,
// default 5000ms). The default (ICECC_P50_SIDECAR_LIFECYCLE_APPLY_MS unset)
// must be exactly 5000ms; this module never changes that default on its own.
// This is purely a sidecar-local timeout: nothing on the wire changes.

#include <chrono>
#include <string>

namespace icecc::p50::service_diag {

struct SidecarLifecycleApplyOverrideResult {
    // The value run() must assign to Options::sidecar_lifecycle_apply_budget.
    // Equal to default_value unless override_applied is true.
    std::chrono::milliseconds effective_value{0};
    // True iff env_value parsed as a plain base-10 integer in [500, 120000]
    // and effective_value was set from it.
    bool override_applied = false;
    // True iff env_value was non-null/non-empty but did not parse as above
    // (not a clean integer, trailing garbage, out of range, or empty after
    // whitespace). effective_value is still default_value in this case; the
    // caller must log rejection_reason and keep the default, never fail
    // closed on a malformed override.
    bool override_rejected = false;
    // Human-readable reason, populated iff override_rejected. Empty
    // otherwise.
    std::string rejection_reason;
};

// env_value is the raw ICECC_P50_SIDECAR_LIFECYCLE_APPLY_MS value (nullptr or
// "" both mean "unset" and always yield effective_value == default_value,
// override_applied == false, override_rejected == false). default_value is
// whatever run() would otherwise have assigned (today, always 5000ms, but
// this function does not assume that).
[[nodiscard]] SidecarLifecycleApplyOverrideResult
resolve_sidecar_lifecycle_apply_override(
    const char* env_value, std::chrono::milliseconds default_value) noexcept;

} // namespace icecc::p50::service_diag
