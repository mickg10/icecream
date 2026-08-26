#pragma once

// Linux exposes a race-free-enough snapshot of the calling process's live
// task directory.  iceccd uses it immediately before its long-lived compile
// worker fork: a second task would make arbitrary libc/C++ state in the child
// unsafe.  Absence means the platform cannot provide the Linux proof.

#include <cstddef>
#include <optional>

[[nodiscard]] std::optional<std::size_t> iceccd_task_count() noexcept;

