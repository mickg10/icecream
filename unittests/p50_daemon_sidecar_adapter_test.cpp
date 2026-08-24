#include "cache/p50_daemon_sidecar_adapter.h"
#include "services/comm.h"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace icecc::p50::local {
__attribute__((weak)) Connection connect_unix_until(
    const std::string&, std::chrono::steady_clock::time_point, Status* status) noexcept
{
    if (status != nullptr)
        *status = Status::Timeout;
    return Connection(-1);
}
} // namespace icecc::p50::local

// The configuration-focused test never dispatches an ordinary MsgChannel;
// keep it independently linkable while the full daemon build supplies the
// production comm.cpp definition.
__attribute__((weak)) int MsgChannel::release_fd_if_input_empty()
{
    return -1;
}

int main()
{
    char directory[] = "/tmp/icecc-p50-adapter-test-XXXXXX";
    if (::mkdtemp(directory) == nullptr)
        return 2;
    (void)::chmod(directory, 0700);

    icecc::p50::daemon::DaemonSidecarAdapter::Config config;
    config.executable = "/bin/true";
    config.runtime_directory = directory;
    config.generation = 1;
    config.expected_daemon_uid = static_cast<uint64_t>(::geteuid());
    config.expected_daemon_gid = static_cast<uint64_t>(::getegid());
    config.expected_service_uid = config.expected_daemon_uid;
    config.expected_service_gid = config.expected_daemon_gid;
    config.public_listener_port = 10245;

    auto invalid = config;
    invalid.public_listener_port = 0;
    if (icecc::p50::daemon::DaemonSidecarAdapter::valid_config(invalid))
        return 3;
    invalid = config;
    invalid.generation = 0;
    if (icecc::p50::daemon::DaemonSidecarAdapter::valid_config(invalid))
        return 4;
    invalid = config;
    invalid.drop_uid = 1;
    if (icecc::p50::daemon::DaemonSidecarAdapter::valid_config(invalid))
        return 5;

    icecc::p50::daemon::DaemonSidecarAdapter adapter(config);
    adapter.observe_public_listener(true, config.public_listener_port);
    if (adapter.start() || adapter.last_error() == icecc::p50::daemon::AdapterError::None)
        return 6;
    adapter.shutdown();
    adapter.shutdown();
    if (adapter.state() != icecc::p50::daemon::AdapterState::Stopped)
        return 7;
    (void)::rmdir(directory);
    std::puts("p50 daemon sidecar adapter: ok");
    return 0;
}
