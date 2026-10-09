#include "p50_sidecar_lifecycle_apply_override.h"

#include <cerrno>
#include <cstdlib>

namespace icecc::p50::service_diag {

SidecarLifecycleApplyOverrideResult resolve_sidecar_lifecycle_apply_override(
    const char* env_value, std::chrono::milliseconds default_value) noexcept {
    SidecarLifecycleApplyOverrideResult result;
    result.effective_value = default_value;

    if (env_value == nullptr || *env_value == '\0')
        return result;

    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(env_value, &end, 10);
    const bool full_match = (end != env_value) && (*end == '\0');
    const bool in_range = parsed >= 500 && parsed <= 120000;

    if (full_match && errno == 0 && in_range) {
        result.override_applied = true;
        result.effective_value = std::chrono::milliseconds(parsed);
        return result;
    }

    result.override_rejected = true;
    result.rejection_reason =
        std::string("malformed/out-of-range ICECC_P50_SIDECAR_LIFECYCLE_APPLY_MS='") +
        env_value + "' (must be an integer in [500, 120000])";
    return result;
}

} // namespace icecc::p50::service_diag
