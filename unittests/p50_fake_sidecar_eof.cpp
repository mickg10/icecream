// Minimal fake sidecar fixture for the phase-3 auth-EOF regression test
// (p50_adapter_auth_eof_regression_test.cpp).
//
// This is NOT a reimplementation of icecc-cache-service.  It exists only to
// drive DaemonSidecarAdapter::outer_advance_authentication() into phase 3
// (Hello sent, waiting for a HelloAck) and then present a clean EOF on that
// connection, without ever writing a HelloAck.  Every value the real READY
// v2 frame needs (generation/attempt/F_STORE_GENERATION/DERIVATION_VERSION/
// C_STORE_GUID/F_STORE_GUID/PATH/DIGEST) is already pre-computed by the
// parent (DaemonSidecarAdapter::PrepareExec in
// cache/p50_daemon_sidecar_adapter.cpp) and handed to this process verbatim
// through ICECC_CACHE_SERVICE_EXPECTED_* environment variables; this fixture
// only has to supply pid= (its own) and DEV=/INO= (lstat of the inherited
// listener's bound pathname, exactly as icecc-cache-service's
// write_ready_lease()/capture_listener_identity() do in
// cache/p50_cache_service.cpp).
//
// This fixture never writes a HelloAck.  It accepts exactly one connection,
// drains whatever the daemon's Hello send produced, optionally drops a
// test-only marker file (ICECC_P50_TEST_EOF_MARKER) right before closing,
// closes the accepted connection (the EOF the regression test is timing),
// and then sleeps briefly before exiting so its own process exit/pidfd reap
// does not race with that EOF observation.

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

const char* require_env(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        std::fprintf(stderr, "p50_fake_sidecar_eof: missing %s\n", name);
        std::exit(2);
    }
    return value;
}

int require_fd_env(const char* name) {
    const char* text = require_env(name);
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0) {
        std::fprintf(stderr, "p50_fake_sidecar_eof: bad fd in %s=%s\n", name,
                     text);
        std::exit(2);
    }
    return static_cast<int>(value);
}

bool write_exact(int fd, const std::string& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written =
            ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

} // namespace

int main() {
    const int ready_fd = require_fd_env("ICECC_CACHE_SERVICE_READY_FD");
    const int listener_fd = require_fd_env("ICECC_CACHE_SERVICE_LISTENER_FD");
    const std::string generation =
        require_env("ICECC_CACHE_SERVICE_EXPECTED_GENERATION");
    const std::string attempt =
        require_env("ICECC_CACHE_SERVICE_EXPECTED_ATTEMPT");
    const std::string f_store_generation =
        require_env("ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GENERATION");
    const std::string derivation_version =
        require_env("ICECC_CACHE_SERVICE_EXPECTED_DERIVATION_VERSION");
    const std::string c_store_guid =
        require_env("ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID");
    const std::string f_store_guid =
        require_env("ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID");
    const std::string socket_path =
        require_env("ICECC_CACHE_SERVICE_EXPECTED_SOCKET");
    const std::string socket_digest =
        require_env("ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST");

    // Match cache/p50_cache_service.cpp's capture_listener_identity(): DEV/
    // INO are the bound pathname's identity, not necessarily the open fd's
    // (they coincide for a plain AF_UNIX pathname socket, but lstat on the
    // path is what the real service reports).
    struct stat pathname_info{};
    if (::lstat(socket_path.c_str(), &pathname_info) != 0) {
        std::fprintf(stderr, "p50_fake_sidecar_eof: lstat(%s) failed: %s\n",
                     socket_path.c_str(), std::strerror(errno));
        return 2;
    }

    const std::string ready_frame =
        "READY v2 generation=" + generation + " attempt=" + attempt +
        " F_STORE_GENERATION=" + f_store_generation +
        " DERIVATION_VERSION=" + derivation_version +
        " pid=" + std::to_string(static_cast<long long>(::getpid())) +
        " C_STORE_GUID=" + c_store_guid + " F_STORE_GUID=" + f_store_guid +
        " PATH=" + socket_path + " DIGEST=" + socket_digest +
        " DEV=" +
        std::to_string(static_cast<unsigned long long>(pathname_info.st_dev)) +
        " INO=" +
        std::to_string(static_cast<unsigned long long>(pathname_info.st_ino)) +
        "\n";

    if (!write_exact(ready_fd, ready_frame)) {
        std::fprintf(stderr, "p50_fake_sidecar_eof: READY write failed: %s\n",
                     std::strerror(errno));
        return 2;
    }
    ::close(ready_fd);

    int accepted = -1;
    for (;;) {
        accepted = ::accept(listener_fd, nullptr, nullptr);
        if (accepted >= 0)
            break;
        if (errno == EINTR)
            continue;
        std::fprintf(stderr, "p50_fake_sidecar_eof: accept failed: %s\n",
                     std::strerror(errno));
        return 2;
    }

    // Drain whatever the daemon's Hello send produced.  Its exact content is
    // irrelevant: this fixture never validates or replies to it.  One
    // bounded read is enough; there is no protocol reason to loop here.
    std::array<char, 256> drain{};
    ssize_t drained = 0;
    do {
        drained = ::recv(accepted, drain.data(), drain.size(), 0);
    } while (drained < 0 && errno == EINTR);

    const char* marker_path = std::getenv("ICECC_P50_TEST_EOF_MARKER");
    if (marker_path != nullptr && *marker_path != '\0') {
        FILE* marker = std::fopen(marker_path, "w");
        if (marker != nullptr) {
            std::fprintf(marker, "pid=%lld\n",
                         static_cast<long long>(::getpid()));
            std::fclose(marker);
        }
    }

    // This is the EOF under test: close the accepted connection without
    // ever having sent a HelloAck.
    ::close(accepted);

    // Give the regression test's poll-driven turn loop a clear window to
    // observe the EOF before this process's own exit/pidfd reap becomes
    // visible on the same outer turn.
    ::usleep(500 * 1000);
    return 0;
}
