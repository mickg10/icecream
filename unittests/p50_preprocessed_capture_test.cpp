#include "../client/p50_preprocessed_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int failures = 0;

void require(bool condition, const char *message)
{
    if (condition) {
        std::fprintf(stderr, "ok - %s\n", message);
    } else {
        std::fprintf(stderr, "FAILED - %s\n", message);
        ++failures;
    }
}

bool write_file(const char *path, std::string_view bytes)
{
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t written_total = 0;
    while (written_total < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + written_total,
                                        bytes.size() - written_total);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            (void)::close(fd);
            return false;
        }
        written_total += static_cast<size_t>(written);
    }
    return ::close(fd) == 0;
}

std::string read_file(const char *path)
{
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return {};
    char bytes[256];
    const ssize_t count = ::read(fd, bytes, sizeof(bytes));
    (void)::close(fd);
    if (count < 0) return {};
    return std::string(bytes, static_cast<size_t>(count));
}

} // namespace

int main()
{
    const char *temporary_root = std::getenv("ICEFARM_TMPDIR");
    if (temporary_root == nullptr || *temporary_root == '\0')
        temporary_root = std::getenv("TMPDIR");
    if (temporary_root == nullptr || *temporary_root == '\0')
        temporary_root = "/tmp";
    std::string directory_template = std::string(temporary_root) +
        (temporary_root[std::strlen(temporary_root) - 1] == '/' ? "" : "/") +
        "icecc-p50-capture-test.XXXXXX";
    if (::mkdtemp(directory_template.data()) == nullptr) {
        std::perror("mkdtemp");
        return 2;
    }
    const std::string directory = directory_template;
    auto path = [&](const char *leaf) {
        return directory + "/" + leaf;
    };
    const std::string source = path("source.ii");
    const std::string capture = path("capture.ii");
    const std::string changed = path("changed.ii");
    const std::string short_source = path("short.ii");
    const std::string symlink = path("symlink.ii");
    const std::string directory_capture = path("directory");
    const std::string fifo_capture = path("fifo");
    const char *original = "exact preprocessed input\nwith a second line\n";

    require(write_file(source.c_str(), original), "create source fixture");
    require(icecc::client::detail::retain_preprocessed_capture(source.c_str(), nullptr),
            "absent capture setting preserves normal path");
    require(icecc::client::detail::retain_preprocessed_capture(source.c_str(), capture.c_str()),
            "fresh capture created exclusively");
    struct stat before{};
    require(::stat(capture.c_str(), &before) == 0 && S_ISREG(before.st_mode) &&
                read_file(capture.c_str()) == original,
            "fresh capture contains exact bytes");
    require(icecc::client::detail::retain_preprocessed_capture(source.c_str(), capture.c_str()),
            "identical retry reuses capture");
    struct stat after{};
    require(::stat(capture.c_str(), &after) == 0 && before.st_ino == after.st_ino &&
                before.st_size == after.st_size && read_file(capture.c_str()) == original,
            "identical retry leaves capture unchanged");

    require(write_file(changed.c_str(), "exact preprocessed input\nwith a second LINE\n") &&
                !icecc::client::detail::retain_preprocessed_capture(changed.c_str(), capture.c_str()) &&
                read_file(capture.c_str()) == original,
            "same-length different bytes are rejected without overwrite");
    require(write_file(short_source.c_str(), "short\n") &&
                !icecc::client::detail::retain_preprocessed_capture(short_source.c_str(), capture.c_str()) &&
                read_file(capture.c_str()) == original,
            "different-length retry is rejected without overwrite");

    require(::symlink(source.c_str(), symlink.c_str()) == 0 &&
                !icecc::client::detail::retain_preprocessed_capture(source.c_str(), symlink.c_str()),
            "symlink capture is rejected");
    require(::mkdir(directory_capture.c_str(), 0700) == 0 &&
                !icecc::client::detail::retain_preprocessed_capture(source.c_str(), directory_capture.c_str()),
            "directory capture is rejected");
    require(::mkfifo(fifo_capture.c_str(), 0600) == 0 &&
                !icecc::client::detail::retain_preprocessed_capture(source.c_str(), fifo_capture.c_str()),
            "FIFO capture is rejected without blocking");

    const std::string large_source = path("large-source.ii");
    const std::string large_capture = path("large-capture.ii");
    const std::string large_changed = path("large-changed.ii");
    std::string large_bytes(150000, 'a');
    std::string large_different = large_bytes;
    large_different.back() = 'b';
    require(write_file(large_source.c_str(), large_bytes) &&
                icecc::client::detail::retain_preprocessed_capture(
                    large_source.c_str(), large_capture.c_str()) &&
                icecc::client::detail::retain_preprocessed_capture(
                    large_source.c_str(), large_capture.c_str()),
            "large capture compares across bounded read chunks");
    require(write_file(large_changed.c_str(), large_different) &&
                !icecc::client::detail::retain_preprocessed_capture(
                    large_changed.c_str(), large_capture.c_str()),
            "late large-file mismatch is rejected");
    struct stat large_capture_stat{};
    require(::stat(large_capture.c_str(), &large_capture_stat) == 0 &&
                large_capture_stat.st_size == static_cast<off_t>(large_bytes.size()),
            "large retry leaves existing capture length unchanged");

    const std::string empty_source = path("empty-source.ii");
    const std::string empty_capture = path("empty-capture.ii");
    require(write_file(empty_source.c_str(), "") &&
                icecc::client::detail::retain_preprocessed_capture(
                    empty_source.c_str(), empty_capture.c_str()) &&
                icecc::client::detail::retain_preprocessed_capture(
                    empty_source.c_str(), empty_capture.c_str()),
            "empty capture and identical retry are accepted");

    (void)::unlink(fifo_capture.c_str());
    (void)::rmdir(directory_capture.c_str());
    (void)::unlink(symlink.c_str());
    (void)::unlink(short_source.c_str());
    (void)::unlink(changed.c_str());
    (void)::unlink(capture.c_str());
    (void)::unlink(source.c_str());
    (void)::unlink(large_source.c_str());
    (void)::unlink(large_capture.c_str());
    (void)::unlink(large_changed.c_str());
    (void)::unlink(empty_source.c_str());
    (void)::unlink(empty_capture.c_str());
    (void)::rmdir(directory.c_str());
    return failures == 0 ? 0 : 1;
}
