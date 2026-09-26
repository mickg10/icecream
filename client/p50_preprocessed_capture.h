/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ICECC_P50_PREPROCESSED_CAPTURE_H
#define ICECC_P50_PREPROCESSED_CAPTURE_H

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace icecc::client::detail {

inline bool regular_file(int fd, struct stat *result) noexcept
{
    return ::fstat(fd, result) == 0 && S_ISREG(result->st_mode) && result->st_size >= 0;
}

inline bool identical_regular_files(int source, int capture) noexcept
{
    struct stat source_stat{};
    struct stat capture_stat{};
    if (!regular_file(source, &source_stat) || !regular_file(capture, &capture_stat) ||
        source_stat.st_size != capture_stat.st_size)
        return false;

    char source_bytes[64 * 1024];
    char capture_bytes[64 * 1024];
    off_t offset = 0;
    while (offset < source_stat.st_size) {
        const size_t wanted = static_cast<size_t>(std::min<off_t>(
            static_cast<off_t>(sizeof(source_bytes)), source_stat.st_size - offset));
        size_t source_total = 0;
        size_t capture_total = 0;
        while (source_total < wanted) {
            const ssize_t count = ::pread(source, source_bytes + source_total,
                                          wanted - source_total,
                                          offset + static_cast<off_t>(source_total));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            source_total += static_cast<size_t>(count);
        }
        while (capture_total < wanted) {
            const ssize_t count = ::pread(capture, capture_bytes + capture_total,
                                          wanted - capture_total,
                                          offset + static_cast<off_t>(capture_total));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            capture_total += static_cast<size_t>(count);
        }
        if (std::memcmp(source_bytes, capture_bytes, wanted) != 0)
            return false;
        offset += static_cast<off_t>(wanted);
    }
    char extra_source = 0;
    char extra_capture = 0;
    ssize_t source_extra;
    do {
        source_extra = ::pread(source, &extra_source, 1, offset);
    } while (source_extra < 0 && errno == EINTR);
    ssize_t capture_extra;
    do {
        capture_extra = ::pread(capture, &extra_capture, 1, offset);
    } while (capture_extra < 0 && errno == EINTR);
    return source_extra == 0 && capture_extra == 0;
}

/* A configured capture is an opt-in test observer.  A retry may revisit the
   same immutable source; accept an existing capture only after an exact,
   read-only comparison.  Never truncate or replace a prior capture. */
inline bool retain_preprocessed_capture(const char *source_path,
                                        const char *capture_path) noexcept
{
    if (capture_path == nullptr)
        return true;
    if (source_path == nullptr || *capture_path == '\0' || *capture_path != '/')
        return false;

    const int source = ::open(source_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (source < 0)
        return false;
    struct stat source_stat{};
    if (!regular_file(source, &source_stat)) {
        (void)::close(source);
        return false;
    }

    const int output = ::open(capture_path, O_WRONLY | O_CREAT | O_EXCL |
                              O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
        const int open_error = errno;
        if (open_error != EEXIST) {
            (void)::close(source);
            return false;
        }
        const int existing = ::open(capture_path,
                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (existing < 0) {
            (void)::close(source);
            return false;
        }
        const bool identical = identical_regular_files(source, existing);
        const bool source_close_ok = ::close(source) == 0;
        const bool existing_close_ok = ::close(existing) == 0;
        return identical && source_close_ok && existing_close_ok;
    }

    bool success = true;
    char buffer[64 * 1024];
    for (;;) {
        ssize_t read_bytes;
        do {
            read_bytes = ::read(source, buffer, sizeof(buffer));
        } while (read_bytes < 0 && errno == EINTR);
        if (read_bytes == 0)
            break;
        if (read_bytes < 0) {
            success = false;
            break;
        }
        ssize_t written_total = 0;
        while (written_total < read_bytes) {
            const ssize_t written = ::write(output, buffer + written_total,
                                            static_cast<size_t>(read_bytes - written_total));
            if (written > 0) {
                written_total += written;
                continue;
            }
            if (written < 0 && errno == EINTR)
                continue;
            success = false;
            break;
        }
        if (!success)
            break;
    }
    if (success && ::fsync(output) != 0)
        success = false;
    if (::close(source) != 0)
        success = false;
    if (::close(output) != 0)
        success = false;
    if (!success)
        (void)::unlink(capture_path);
    return success;
}

} // namespace icecc::client::detail

#endif
