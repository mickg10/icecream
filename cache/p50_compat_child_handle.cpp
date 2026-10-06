#include "p50_compat_child_handle.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <time.h>
#  include <unistd.h>
#endif

namespace icecc::p50::platform {

namespace {

// Reads /proc/<pid>/stat and extracts starttime (field 22). The comm field
// (field 2) is parenthesized and may itself contain spaces or parens, so
// parsing starts after the *last* ')' on the line rather than splitting
// naively on whitespace.
bool read_proc_stat_starttime(long pid, unsigned long long* out) noexcept {
    if (pid <= 0 || out == nullptr)
        return false;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    FILE* file = std::fopen(path, "r");
    if (file == nullptr)
        return false;
    char buffer[4096];
    const size_t read_bytes = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    if (read_bytes == 0)
        return false;
    buffer[read_bytes] = '\0';
    const char* close_paren = std::strrchr(buffer, ')');
    if (close_paren == nullptr)
        return false;
    const char* cursor = close_paren + 1;
    int field_index = 2; // the parenthesized comm is field 2
    while (*cursor != '\0') {
        while (*cursor == ' ')
            ++cursor;
        if (*cursor == '\0')
            break;
        const char* token_start = cursor;
        while (*cursor != '\0' && *cursor != ' ')
            ++cursor;
        ++field_index;
        if (field_index == 22) {
            *out = std::strtoull(token_start, nullptr, 10);
            return true;
        }
    }
    return false;
}

} // namespace

bool read_starttime_ticks(long pid, unsigned long long* out) noexcept {
    return read_proc_stat_starttime(pid, out);
}

bool CompatChildHandle::arm(long pid) noexcept {
    unsigned long long starttime = 0;
    if (!read_proc_stat_starttime(pid, &starttime))
        return false;
    pid_ = pid;
    starttime_ticks_ = starttime;
    return true;
}

bool CompatChildHandle::still_same_process() const noexcept {
    if (pid_ <= 0)
        return false;
    unsigned long long starttime = 0;
    if (!read_proc_stat_starttime(pid_, &starttime))
        return false;
    return starttime == starttime_ticks_;
}

bool compat_wait_bounded(long pid, const CompatChildHandle& handle, int timeout_ms,
                         int* exit_status) noexcept {
#if defined(__linux__) && defined(WEXITED) && defined(WNOHANG) && defined(WNOWAIT)
    if (pid <= 0 || !handle.armed() || handle.pid() != pid || timeout_ms < 0)
        return false;
    const int poll_interval_ms = 20;
    int elapsed_ms = 0;
    for (;;) {
        if (!handle.still_same_process())
            return false; // fail closed: pid_max wrap, or reaped elsewhere
        siginfo_t info{};
        info.si_pid = 0;
        const int result =
            ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT);
        if (result == 0 && info.si_pid == pid) {
            siginfo_t reaped{};
            while (::waitid(P_PID, static_cast<id_t>(pid), &reaped, WEXITED) < 0 &&
                  errno == EINTR) {}
            if (exit_status != nullptr)
                *exit_status = (reaped.si_code == CLD_EXITED) ? reaped.si_status
                                                               : -reaped.si_status;
            return true;
        }
        if (elapsed_ms >= timeout_ms)
            return false;
        struct timespec sleep_time{0, static_cast<long>(poll_interval_ms) * 1000000L};
        ::nanosleep(&sleep_time, nullptr);
        elapsed_ms += poll_interval_ms;
    }
#else
    (void)pid;
    (void)handle;
    (void)timeout_ms;
    (void)exit_status;
    return false;
#endif
}

} // namespace icecc::p50::platform
