// Checkpoint-2 unit coverage for daemon/p50_platform_compat.{h,cpp}: the
// startup-time M1/M2/M3 kernel-3.10 compatibility decision module.
//
// Two independent things are exercised here, both required by review:
//   1. Detection with injected errno: every probe failure shape (ENOSYS,
//      a non-ENOSYS probe error, and -- for M2 -- a seal round-trip that
//      fails without an errno at all) is driven through TestHooks so the
//      resulting Backend/reason is asserted without depending on which
//      kernel this binary happens to run on.
//   2. Env-forced mode parsing and compat selection for the shorthand
//      (ICECC_P50_PLATFORM) and the three per-mitigation overrides
//      (ICECC_P50_PLATFORM_CHILD/_INPUT/_RANDOM), including the
//      per-mitigation-overrides-shorthand case and the invalid-value case.
//
// A handful of baseline cases also run the real, unhooked probes (no env
// vars, no TestHooks): on the modern-kernel host this binary runs on, M1,
// M2 and M3 are all expected to resolve Native. This is the only place that
// "native behavior on modern kernels is unchanged by default" is checked
// directly against the real syscalls rather than through TestHooks.
#include "../daemon/p50_platform_compat.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;
#define CHECK(expr, text) do { \
    if (!(expr)) { std::fprintf(stderr, "FAILED - %s\n", text); ++failures; } \
    else std::fprintf(stderr, "ok - %s\n", text); \
} while (0)

using icecc::p50::platform::Backend;
using icecc::p50::platform::Mode;
using icecc::p50::platform::PlatformDecision;
using icecc::p50::platform::TestHooks;

// Clears every ICECC_P50_PLATFORM* variable so each test case starts from a
// known, fully-Auto environment regardless of what an earlier case set.
void clear_platform_env()
{
    ::unsetenv("ICECC_P50_PLATFORM");
    ::unsetenv("ICECC_P50_PLATFORM_CHILD");
    ::unsetenv("ICECC_P50_PLATFORM_INPUT");
    ::unsetenv("ICECC_P50_PLATFORM_RANDOM");
}

struct EnvScope {
    EnvScope() { clear_platform_env(); icecc::p50::platform::reset_test_hooks(); }
    ~EnvScope() { clear_platform_env(); icecc::p50::platform::reset_test_hooks(); }
};

void test_parse_mode()
{
    using icecc::p50::platform::parse_mode;
    CHECK(parse_mode(nullptr) == Mode::Auto, "null value parses as Auto");
    CHECK(parse_mode("") == Mode::Auto, "empty value parses as Auto");
    CHECK(parse_mode("auto") == Mode::Auto, "'auto' parses as Auto");
    CHECK(parse_mode("native") == Mode::Native, "'native' parses as Native");
    CHECK(parse_mode("compat") == Mode::Compat, "'compat' parses as Compat");
    CHECK(parse_mode("Compat") == Mode::Invalid, "mode parsing is case-sensitive");
    CHECK(parse_mode("frobnicate") == Mode::Invalid, "unknown value parses as Invalid");
}

void test_detection_baseline_native()
{
    EnvScope scope;
    const PlatformDecision decision = icecc::p50::platform::decide_platform();
    CHECK(decision.child.backend == Backend::Native && decision.child.reason == "pidfd ok",
          "baseline (no env, no hooks): child resolves native pidfd ok");
    CHECK(decision.input.backend == Backend::Native && decision.input.reason == "seal round-trip ok",
          "baseline (no env, no hooks): input resolves native seal round-trip ok");
    CHECK(decision.random.backend == Backend::Native && decision.random.reason == "getrandom ok",
          "baseline (no env, no hooks): random resolves native getrandom ok");
    CHECK(!icecc::p50::platform::any_fail_closed(decision),
          "baseline (no env, no hooks): nothing is fail-closed");
}

void test_detection_enosys_goes_compat()
{
    EnvScope scope;
    TestHooks hooks;
    hooks.force_pidfd_open_errno = ENOSYS;
    hooks.force_memfd_create_errno = ENOSYS;
    hooks.force_getrandom_errno = ENOSYS;
    icecc::p50::platform::set_test_hooks(hooks);
    const PlatformDecision decision = icecc::p50::platform::decide_platform();
    CHECK(decision.child.backend == Backend::Compat && decision.child.reason == "ENOSYS",
          "pidfd_open ENOSYS under Auto resolves child compat");
    CHECK(decision.input.backend == Backend::Compat && decision.input.reason == "ENOSYS",
          "memfd_create ENOSYS under Auto resolves input compat");
    CHECK(decision.random.backend == Backend::Compat && decision.random.reason == "ENOSYS",
          "getrandom ENOSYS under Auto resolves random compat");
    CHECK(!icecc::p50::platform::any_fail_closed(decision),
          "all-ENOSYS-to-compat decision reports no fail-closed mitigation");
}

void test_detection_waitid_enosys_goes_compat()
{
    // Distinct injection point from pidfd_open itself: waitid(P_PIDFD) can
    // be the thing that is missing even when pidfd_open() succeeds.
    EnvScope scope;
    TestHooks hooks;
    hooks.force_waitid_pidfd_errno = ENOSYS;
    icecc::p50::platform::set_test_hooks(hooks);
    const PlatformDecision decision = icecc::p50::platform::decide_platform();
    CHECK(decision.child.backend == Backend::Compat && decision.child.reason == "ENOSYS",
          "waitid(P_PIDFD) ENOSYS under Auto resolves child compat");
}

void test_detection_seal_roundtrip_failure_is_not_enosys()
{
    // force_seal_roundtrip_failure fails the probe without touching errno at
    // all: memfd_create() itself succeeds, only the seal round-trip check
    // fails. This must NOT be classified as "unsupported" (ENOSYS); under
    // Auto it must fail closed, not silently fall back to compat.
    EnvScope scope;
    TestHooks hooks;
    hooks.force_seal_roundtrip_failure = true;
    icecc::p50::platform::set_test_hooks(hooks);
    const PlatformDecision decision = icecc::p50::platform::decide_platform();
    CHECK(decision.input.backend == Backend::FailClosed &&
              decision.input.reason == "probe error (not ENOSYS)",
          "seal round-trip failure under Auto fails closed, not compat");
    CHECK(icecc::p50::platform::any_fail_closed(decision),
          "seal round-trip failure is visible via any_fail_closed");
}

void test_detection_non_enosys_probe_error_fails_closed()
{
    EnvScope scope;
    TestHooks hooks;
    hooks.force_pidfd_open_errno = EPERM;
    hooks.force_memfd_create_errno = EPERM;
    hooks.force_getrandom_errno = EPERM;
    icecc::p50::platform::set_test_hooks(hooks);
    const PlatformDecision decision = icecc::p50::platform::decide_platform();
    CHECK(decision.child.backend == Backend::FailClosed &&
              decision.child.reason == "probe error (not ENOSYS)",
          "non-ENOSYS pidfd_open error under Auto fails closed");
    CHECK(decision.input.backend == Backend::FailClosed &&
              decision.input.reason == "probe error (not ENOSYS)",
          "non-ENOSYS memfd_create error under Auto fails closed");
    CHECK(decision.random.backend == Backend::FailClosed &&
              decision.random.reason == "probe error (not ENOSYS)",
          "non-ENOSYS getrandom error under Auto fails closed");
    CHECK(icecc::p50::platform::any_fail_closed(decision),
          "all-probe-error decision reports fail-closed");
}

void test_detection_native_forced_with_enosys_fails_closed_no_compat()
{
    // Mode::Native must never silently fall back to compat, even on a clean
    // ENOSYS: an operator who forced native wants a loud failure, not a
    // quiet downgrade.
    EnvScope scope;
    ::setenv("ICECC_P50_PLATFORM_CHILD", "native", 1);
    TestHooks hooks;
    hooks.force_pidfd_open_errno = ENOSYS;
    icecc::p50::platform::set_test_hooks(hooks);
    const PlatformDecision decision = icecc::p50::platform::decide_platform();
    CHECK(decision.child.backend == Backend::FailClosed &&
              decision.child.reason == "ENOSYS (native forced, no compat)",
          "native-forced child with ENOSYS fails closed, does not fall back to compat");
}

void test_detection_native_forced_with_probe_error_fails_closed()
{
    EnvScope scope;
    ::setenv("ICECC_P50_PLATFORM_CHILD", "native", 1);
    TestHooks hooks;
    hooks.force_pidfd_open_errno = EPERM;
    icecc::p50::platform::set_test_hooks(hooks);
    const PlatformDecision decision = icecc::p50::platform::decide_platform();
    CHECK(decision.child.backend == Backend::FailClosed &&
              decision.child.reason == "probe error (native forced, no compat)",
          "native-forced child with a non-ENOSYS probe error fails closed");
}

void test_detection_native_forced_succeeds_on_this_host()
{
    // Forcing native with no fault injection must take the real syscall
    // path and succeed on the modern-kernel host this binary runs on.
    EnvScope scope;
    ::setenv("ICECC_P50_PLATFORM_CHILD", "native", 1);
    ::setenv("ICECC_P50_PLATFORM_INPUT", "native", 1);
    ::setenv("ICECC_P50_PLATFORM_RANDOM", "native", 1);
    const PlatformDecision decision = icecc::p50::platform::decide_platform();
    CHECK(decision.child.backend == Backend::Native, "native-forced child succeeds on this host");
    CHECK(decision.input.backend == Backend::Native, "native-forced input succeeds on this host");
    CHECK(decision.random.backend == Backend::Native, "native-forced random succeeds on this host");
}

void test_env_shorthand_forces_compat_for_all_three()
{
    EnvScope scope;
    ::setenv("ICECC_P50_PLATFORM", "compat", 1);
    const PlatformDecision decision = icecc::p50::platform::decide_platform();
    CHECK(decision.child.backend == Backend::Compat && decision.child.reason == "forced compat",
          "shorthand compat forces child compat");
    CHECK(decision.input.backend == Backend::Compat && decision.input.reason == "forced compat",
          "shorthand compat forces input compat");
    CHECK(decision.random.backend == Backend::Compat && decision.random.reason == "forced compat",
          "shorthand compat forces random compat");
}

void test_env_per_mitigation_overrides_shorthand()
{
    // ICECC_P50_PLATFORM_CHILD, when set, wins over the shorthand entirely
    // for that one mitigation; the other two still fall back to it.
    EnvScope scope;
    ::setenv("ICECC_P50_PLATFORM", "compat", 1);
    ::setenv("ICECC_P50_PLATFORM_CHILD", "native", 1);
    TestHooks hooks;
    hooks.force_pidfd_open_errno = ENOSYS;
    icecc::p50::platform::set_test_hooks(hooks);
    const PlatformDecision decision = icecc::p50::platform::decide_platform();
    CHECK(decision.child.backend == Backend::FailClosed &&
              decision.child.reason == "ENOSYS (native forced, no compat)",
          "per-mitigation override (native) wins over shorthand (compat)");
    CHECK(decision.input.backend == Backend::Compat && decision.input.reason == "forced compat",
          "input with no override still falls back to shorthand compat");
    CHECK(decision.random.backend == Backend::Compat && decision.random.reason == "forced compat",
          "random with no override still falls back to shorthand compat");
}

void test_env_invalid_per_mitigation_value_fails_closed()
{
    EnvScope scope;
    ::setenv("ICECC_P50_PLATFORM_INPUT", "bogus", 1);
    const PlatformDecision decision = icecc::p50::platform::decide_platform();
    CHECK(decision.input.backend == Backend::FailClosed &&
              decision.input.reason == "invalid value 'bogus'",
          "invalid per-mitigation value fails closed with the raw value quoted");
}

void test_env_invalid_shorthand_fails_all_three_closed()
{
    EnvScope scope;
    ::setenv("ICECC_P50_PLATFORM", "frobnicate", 1);
    const PlatformDecision decision = icecc::p50::platform::decide_platform();
    CHECK(decision.child.backend == Backend::FailClosed &&
              decision.child.reason == "invalid value 'frobnicate'",
          "invalid shorthand fails child closed");
    CHECK(decision.input.backend == Backend::FailClosed &&
              decision.input.reason == "invalid value 'frobnicate'",
          "invalid shorthand fails input closed");
    CHECK(decision.random.backend == Backend::FailClosed &&
              decision.random.reason == "invalid value 'frobnicate'",
          "invalid shorthand fails random closed");
    CHECK(icecc::p50::platform::any_fail_closed(decision),
          "invalid shorthand is visible via any_fail_closed");
}

void test_format_platform_log_line()
{
    PlatformDecision decision;
    decision.child.backend = Backend::Native;
    decision.child.reason = "pidfd ok";
    decision.input.backend = Backend::Compat;
    decision.input.reason = "forced compat";
    decision.random.backend = Backend::FailClosed;
    decision.random.reason = "probe error (not ENOSYS)";
    const std::string line = icecc::p50::platform::format_platform_log_line(decision);
    CHECK(line ==
              "p50 platform: child=native(pidfd ok) input=compat(forced compat) "
              "random=fail-closed(probe error (not ENOSYS))",
          "format_platform_log_line renders all three mitigations exactly");
}

void test_compat_store_identity_entropy()
{
    CHECK(icecc::p50::platform::compat_store_identity_entropy(nullptr, 32, 0) == -1,
          "compat_store_identity_entropy rejects a null buffer");
    unsigned char buffer[32];
    std::memset(buffer, 0, sizeof(buffer));
    const ssize_t got = icecc::p50::platform::compat_store_identity_entropy(buffer, sizeof(buffer), 0);
    CHECK(got == static_cast<ssize_t>(sizeof(buffer)),
          "compat_store_identity_entropy fills the full requested length");
    bool any_nonzero = false;
    for (unsigned char byte : buffer) {
        if (byte != 0) { any_nonzero = true; break; }
    }
    CHECK(any_nonzero, "compat_store_identity_entropy actually wrote entropy, not an untouched zero buffer");
}

// Creates an unlinked, regular, read/write temp file containing exactly
// `content`, with the file offset left at 0 (as copy_compat_input_from_sidecar
// expects of a sidecar-handed-off source: it reads from the current offset,
// it does not seek first).
int make_source_fd(const char* content)
{
    char tmpl[] = "/tmp/p50_platform_compat_test_src.XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd < 0)
        return -1;
    ::unlink(tmpl);
    const size_t len = std::strlen(content);
    size_t written = 0;
    while (written < len) {
        const ssize_t result = ::write(fd, content + written, len - written);
        if (result <= 0) {
            ::close(fd);
            return -1;
        }
        written += static_cast<size_t>(result);
    }
    if (::lseek(fd, 0, SEEK_SET) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void test_copy_compat_input_from_sidecar()
{
    CHECK(!icecc::p50::platform::copy_compat_input_from_sidecar(-1).ok,
          "copy_compat_input_from_sidecar rejects a negative fd");

    int pipe_fds[2] = {-1, -1};
    CHECK(::pipe(pipe_fds) == 0, "pipe() created for the non-regular-file rejection case");
    const auto pipe_result = icecc::p50::platform::copy_compat_input_from_sidecar(pipe_fds[0]);
    CHECK(!pipe_result.ok, "copy_compat_input_from_sidecar rejects a non-regular-file fd (a pipe)");
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);

    const char* payload = "hello compat world, this is the sidecar's input artifact";
    const int source_fd = make_source_fd(payload);
    CHECK(source_fd >= 0, "source fd for the happy-path copy was created");
    if (source_fd >= 0) {
        const auto result = icecc::p50::platform::copy_compat_input_from_sidecar(source_fd);
        CHECK(result.ok && result.fd >= 0,
              "copy_compat_input_from_sidecar succeeds on a well-formed regular-file source");
        if (result.ok) {
            struct stat info{};
            CHECK(::fstat(result.fd, &info) == 0 && info.st_nlink == 0,
                  "the daemon's copy is unlinked (nlink == 0)");
            CHECK((info.st_mode & 0777) == 0400, "the daemon's copy is mode 0400");
            CHECK(static_cast<size_t>(info.st_size) == std::strlen(payload),
                  "the daemon's copy has the exact source size");
            std::vector<char> readback(std::strlen(payload));
            CHECK(::lseek(result.fd, 0, SEEK_SET) == 0, "daemon copy fd is seekable back to 0");
            const ssize_t got = ::read(result.fd, readback.data(), readback.size());
            CHECK(got == static_cast<ssize_t>(readback.size()) &&
                      std::memcmp(readback.data(), payload, readback.size()) == 0,
                  "the daemon's copy reads back byte-for-byte identical to the source");
            ::close(result.fd);
        }
        ::close(source_fd);
    }
}

} // namespace

int main()
{
    test_parse_mode();
    test_detection_baseline_native();
    test_detection_enosys_goes_compat();
    test_detection_waitid_enosys_goes_compat();
    test_detection_seal_roundtrip_failure_is_not_enosys();
    test_detection_non_enosys_probe_error_fails_closed();
    test_detection_native_forced_with_enosys_fails_closed_no_compat();
    test_detection_native_forced_with_probe_error_fails_closed();
    test_detection_native_forced_succeeds_on_this_host();
    test_env_shorthand_forces_compat_for_all_three();
    test_env_per_mitigation_overrides_shorthand();
    test_env_invalid_per_mitigation_value_fails_closed();
    test_env_invalid_shorthand_fails_all_three_closed();
    test_format_platform_log_line();
    test_compat_store_identity_entropy();
    test_copy_compat_input_from_sidecar();
    return failures == 0 ? 0 : 1;
}
