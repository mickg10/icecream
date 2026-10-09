// Unit coverage for cache/p50_sidecar_lifecycle_apply_override.{h,cpp}: the
// ICECC_P50_SIDECAR_LIFECYCLE_APPLY_MS parse-and-clamp logic extracted out of
// cache/p50_cache_service.cpp's run() specifically so it is directly testable
// (same rationale as daemon/p50_lifecycle_timeout_override.h, see
// unittests/p50_lifecycle_timeout_override_test.cpp -- this test mirrors that
// one's structure exactly, adjusted for this override's own env var name,
// default, and clamp range).
//
// This exercises resolve_sidecar_lifecycle_apply_override() directly: no
// SidecarRuntime, no control socket, no owner thread. It is a pure function
// of (env_value, default_value), so every outcome (unset, valid, malformed,
// out-of-range, boundary) is checked without any process/env global state
// beyond the two explicit arguments.
#include "../cache/p50_sidecar_lifecycle_apply_override.h"

#include <cstdio>

namespace {

int failures = 0;
#define CHECK(expr, text) do { \
    if (!(expr)) { std::fprintf(stderr, "FAILED - %s\n", text); ++failures; } \
    else std::fprintf(stderr, "ok - %s\n", text); \
} while (0)

using icecc::p50::service_diag::resolve_sidecar_lifecycle_apply_override;

constexpr std::chrono::milliseconds kDefault{5000};

void test_unset_and_empty_keep_default()
{
    {
        const auto r = resolve_sidecar_lifecycle_apply_override(nullptr, kDefault);
        CHECK(r.effective_value == kDefault, "null env keeps default effective_value");
        CHECK(!r.override_applied, "null env does not set override_applied");
        CHECK(!r.override_rejected, "null env does not set override_rejected");
        CHECK(r.rejection_reason.empty(), "null env leaves rejection_reason empty");
    }
    {
        const auto r = resolve_sidecar_lifecycle_apply_override("", kDefault);
        CHECK(r.effective_value == kDefault, "empty env keeps default effective_value");
        CHECK(!r.override_applied, "empty env does not set override_applied");
        CHECK(!r.override_rejected, "empty env does not set override_rejected");
    }
}

void test_valid_value_applies()
{
    const auto r = resolve_sidecar_lifecycle_apply_override("15000", kDefault);
    CHECK(r.effective_value == std::chrono::milliseconds(15000),
          "valid '15000' becomes effective_value 15000ms");
    CHECK(r.override_applied, "valid '15000' sets override_applied");
    CHECK(!r.override_rejected, "valid '15000' does not set override_rejected");
}

void test_boundary_values_are_inclusive()
{
    {
        const auto r = resolve_sidecar_lifecycle_apply_override("500", kDefault);
        CHECK(r.effective_value == std::chrono::milliseconds(500) && r.override_applied,
              "lower boundary '500' is accepted (inclusive)");
    }
    {
        const auto r = resolve_sidecar_lifecycle_apply_override("120000", kDefault);
        CHECK(r.effective_value == std::chrono::milliseconds(120000) && r.override_applied,
              "upper boundary '120000' is accepted (inclusive)");
    }
    {
        const auto r = resolve_sidecar_lifecycle_apply_override("499", kDefault);
        CHECK(r.effective_value == kDefault && !r.override_applied && r.override_rejected,
              "'499' (just below floor) is rejected, default kept");
    }
    {
        const auto r = resolve_sidecar_lifecycle_apply_override("120001", kDefault);
        CHECK(r.effective_value == kDefault && !r.override_applied && r.override_rejected,
              "'120001' (just above ceiling) is rejected, default kept");
    }
}

void test_malformed_values_are_rejected_and_default_kept()
{
    // NOTE: a *leading*-whitespace value (e.g. " 15000") is intentionally not
    // included here: strtoll() skips leading whitespace per the C standard,
    // so resolve_sidecar_lifecycle_apply_override() legitimately accepts it
    // as a full match. That is correct, unchanged production behavior, not a
    // malformed case; see test_leading_whitespace_is_tolerated() below.
    const char* bad_values[] = {
        "not-a-number", "15000x", "15000 ", "15.5", "-1", "0",
        "0x1388", "+", "",
    };
    for (const char* bad : bad_values) {
        if (*bad == '\0')
            continue; // empty is covered by test_unset_and_empty_keep_default; not malformed.
        const auto r = resolve_sidecar_lifecycle_apply_override(bad, kDefault);
        CHECK(r.effective_value == kDefault, "malformed value keeps default effective_value");
        CHECK(!r.override_applied, "malformed value does not set override_applied");
        CHECK(r.override_rejected, "malformed value sets override_rejected");
        CHECK(!r.rejection_reason.empty(), "malformed value populates rejection_reason");
    }
}

void test_leading_whitespace_is_tolerated()
{
    // strtoll() skips leading whitespace per the C standard; the resolver
    // relies on strtoll() directly and does not add its own stricter check,
    // so a leading-whitespace value is accepted exactly like its trimmed
    // form. This is documented, pre-existing (not a behavior change made by
    // this diagnostic work) parsing behavior, called out explicitly so it is
    // not mistaken for an oversight.
    const auto r = resolve_sidecar_lifecycle_apply_override(" 15000", kDefault);
    CHECK(r.effective_value == std::chrono::milliseconds(15000),
          "leading-whitespace '  15000' is accepted via strtoll's own whitespace skip");
    CHECK(r.override_applied, "leading-whitespace value sets override_applied");
    CHECK(!r.override_rejected, "leading-whitespace value does not set override_rejected");
}

void test_rejection_reason_mentions_the_offending_value()
{
    const auto r = resolve_sidecar_lifecycle_apply_override("frobnicate", kDefault);
    CHECK(r.override_rejected, "'frobnicate' is rejected");
    CHECK(r.rejection_reason.find("frobnicate") != std::string::npos,
          "rejection_reason echoes the offending env value");
    CHECK(r.rejection_reason.find("ICECC_P50_SIDECAR_LIFECYCLE_APPLY_MS") != std::string::npos,
          "rejection_reason names the env var");
}

void test_default_value_is_passed_through_unmodified()
{
    // The function must not assume the production 5000ms default; whatever
    // default_value the caller passes is exactly what comes back on
    // unset/malformed.
    const auto r_unset = resolve_sidecar_lifecycle_apply_override(nullptr, std::chrono::milliseconds(7777));
    CHECK(r_unset.effective_value == std::chrono::milliseconds(7777),
          "unset env returns whatever default_value the caller passed, not a hardcoded 5000ms");
    const auto r_bad = resolve_sidecar_lifecycle_apply_override("bogus", std::chrono::milliseconds(7777));
    CHECK(r_bad.effective_value == std::chrono::milliseconds(7777),
          "malformed env also returns the caller's default_value, not a hardcoded 5000ms");
}

} // namespace

int main()
{
    test_unset_and_empty_keep_default();
    test_valid_value_applies();
    test_boundary_values_are_inclusive();
    test_malformed_values_are_rejected_and_default_kept();
    test_leading_whitespace_is_tolerated();
    test_rejection_reason_mentions_the_offending_value();
    test_default_value_is_passed_through_unmodified();
    return failures == 0 ? 0 : 1;
}
