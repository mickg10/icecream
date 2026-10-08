#pragma once

// Pure parse-and-clamp logic for the ICECC_P50_INPUT_LIFECYCLE_TIMEOUT_MS
// diagnostic override (see daemon/main.cpp, Daemon::configure_cache_adapter()).
//
// This exists as a free function, with no dependency on Daemon's internals,
// specifically so a unit test can exercise every parse/clamp/reject outcome
// directly, without constructing a Daemon or a real cache adapter. It is
// extracted the same way daemon/p50_platform_compat.h's
// apply_platform_decision_to_config() is extracted, and for the same reason.
//
// This override is diagnostic-only: it changes how long the daemon waits for
// an input-lifecycle RPC reply before giving up, which is the exact budget
// implicated in the sidecar input-lifecycle retirement investigation
// (reason=protocol action=CloseAcceptedJob status=Timeout). The default
// (ICECC_P50_INPUT_LIFECYCLE_TIMEOUT_MS unset) must remain bit-for-bit
// identical to today's hardcoded 5000ms; this module never changes that
// default on its own.

#include <chrono>
#include <string>

namespace icecc::p50::daemon_diag {

struct LifecycleTimeoutOverrideResult {
    // The value Daemon::configure_cache_adapter() must assign to
    // config.input_lifecycle_timeout. Equal to default_value unless
    // override_applied is true.
    std::chrono::milliseconds effective_value{0};
    // True iff env_value parsed as a plain base-10 integer in [1000, 120000]
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

// env_value is the raw ICECC_P50_INPUT_LIFECYCLE_TIMEOUT_MS value (nullptr or
// "" both mean "unset" and always yield effective_value == default_value,
// override_applied == false, override_rejected == false). default_value is
// whatever Daemon::configure_cache_adapter() would otherwise have assigned
// (today, always 5000ms, but this function does not assume that).
[[nodiscard]] LifecycleTimeoutOverrideResult resolve_input_lifecycle_timeout_override(
    const char* env_value, std::chrono::milliseconds default_value) noexcept;

} // namespace icecc::p50::daemon_diag
