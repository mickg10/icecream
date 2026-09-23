#!/bin/sh
# Deletion-sensitive source gate for the bounded pre-bound listener/two-role slice.
set -eu
src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
supervisor="$src/unittests/support/p50_sidecar_supervisor.cpp"
service="$src/cache/p50_cache_service.cpp"
roles="$src/unittests/support/p50_role_owner.cpp"
header="$src/unittests/support/p50_role_owner.h"

for pattern in \
    'kListenerFdEnvironment' 'local::listen_unix(lease.socket_path' \
    'set_cloexec(inherited_listener_fd, false)' 'close_if_open(pending_listener_fd_)' \
    'ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID' 'C_STORE_GUID=' \
    'parse_listener_fd' 'capture_prebound_listener_identity' \
    'const bool prebound = structured_launch.active'; do
    grep -F "$pattern" "$supervisor" "$service" >/dev/null
done

for pattern in \
    'enum class RoleDiscriminator' 'class ClientRoleOwner' \
    'class ServerRoleOwner' 'rejected_wrong_role' 'rejected_namespace' \
    'role != expected' 'RoleOwnedFd' 'shared_ptr<RoleLiveState>' \
    'live_counter.reset()'; do
    grep -F "$pattern" "$roles" "$header" >/dev/null
done

# The structured service may not bind a pathname.  The only remaining bind is
# the explicitly unstructured compatibility path.
grep -F 'if (prebound)' "$service" >/dev/null
grep -F 'return;' "$service" >/dev/null

mutant=$(mktemp "${TMPDIR:-/tmp}/p50tworole-mutant.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM
sed 's/const bool prebound = structured_launch.active;/const bool prebound = false;/' \
    "$service" >"$mutant"
if grep -F 'const bool prebound = structured_launch.active;' "$mutant" >/dev/null; then
    echo 'FAIL: pre-bound deletion mutant was not formed' >&2
    exit 1
fi
echo 'ok - pre-bound/two-role source gates hold'
