#pragma once

// Protocol-50 cache sidecar service (S2 local-control skeleton).
//
// The service intentionally owns only its private AF_UNIX control listener.
// iceccd remains the public TCP listener owner and will hand clean-boundary
// cache-session descriptors to a later protocol slice.  No cache codec,
// store, remote frame, daemon, or advertisement code belongs here.

#include <cstdint>
#include <optional>
#include <string>

#include "p50_local_transport.h"

namespace icecc::p50::service {

struct Options {
    std::string socket_path;
    local::Identity identity{};
    local::CredentialExpectation expected_peer;
    std::optional<uint64_t> drop_uid;
    std::optional<uint64_t> drop_gid;
    int backlog = 1;
};

// Parses only the service's explicit options.  It never consults PATH,
// invokes a shell, or accepts an implicit/default socket or identity.
bool parse_options(int argc, char* const argv[], Options& options,
                   bool& show_help) noexcept;

// Runs the bounded service loop.  A zero return means clean stop; nonzero is
// a fail-closed startup or control error.  The function installs no process
// supervisor and does not alter daemon advertisement state.
int run(const Options& options) noexcept;

} // namespace icecc::p50::service
