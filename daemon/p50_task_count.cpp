#include "p50_task_count.h"

#ifdef __linux__
#include <dirent.h>
#endif

#include <cerrno>
#include <cctype>

std::optional<std::size_t> iceccd_task_count() noexcept
{
#ifdef __linux__
    DIR *directory = ::opendir("/proc/self/task");
    if (directory == nullptr)
        return std::nullopt;

    std::size_t count = 0;
    bool scan_failed = false;
    for (;;) {
        // POSIX requires errno to be inspected when readdir() returns null in
        // order to distinguish end-of-directory from a partial failed scan.
        // A partial count of one must never authorize the subsequent fork.
        errno = 0;
        const dirent *entry = ::readdir(directory);
        if (entry == nullptr) {
            scan_failed = errno != 0;
            break;
        }
        const unsigned char first = static_cast<unsigned char>(entry->d_name[0]);
        if (!std::isdigit(first))
            continue;
        bool numeric = true;
        for (const char *cursor = entry->d_name + 1; *cursor != '\0'; ++cursor) {
            if (!std::isdigit(static_cast<unsigned char>(*cursor))) {
                numeric = false;
                break;
            }
        }
        if (numeric)
            ++count;
    }
    const int close_result = ::closedir(directory);
    if (scan_failed || close_result != 0 || count == 0)
        return std::nullopt;
    return count;
#else
    return std::nullopt;
#endif
}
