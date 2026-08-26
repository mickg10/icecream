#pragma once

// Exact descriptor ownership for a long-lived compiler child.  This is a
// first-fork contract: a child may proceed only after it has proved that the
// two legacy channels (result pipe and client socket), and optionally the one
// authenticated P50 source descriptor, are valid and distinct and that every
// other inherited descriptor was really closed.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace icecc::p50::forkfd {

enum class Failure : uint8_t {
    None = 0,
    InvalidKeepSet,
    OwnershipFailure,
    TypeFailure,
    EnumerationFailure,
    ParseFailure,
    CloseFailure,
    BackendUnavailable,
};

struct AcceptedSource {
    int fd = -1;
    // This is the exact InputFdRequest::request_id which authorized fd.  Zero
    // is never a valid P50 delivery identity.
    uint64_t delivery_id = 0;
};

struct KeepSet {
    int stat_pipe_fd = -1;
    int client_fd = -1;
    std::optional<AcceptedSource> source;
};

struct Result {
    Failure failure = Failure::None;
    size_t closed_count = 0;

    [[nodiscard]] bool ok() const noexcept { return failure == Failure::None; }
};

[[nodiscard]] Result sweep(const KeepSet& keep) noexcept;
[[nodiscard]] const char* failure_name(Failure failure) noexcept;

#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
// These hooks are deliberately test-only.  They inject failures at the
// backend boundary without replacing libc globally, so deletion-sensitive
// tests exercise the exact production decision tree.
struct TestHooks {
    bool force_close_range_unsupported = false;
    bool force_close_range_failure = false;
    bool force_proc_failure = false;
    bool force_parse_failure = false;
    int fail_close_fd = -1;
};

void set_test_hooks(TestHooks hooks) noexcept;
void reset_test_hooks() noexcept;
#endif

} // namespace icecc::p50::forkfd
