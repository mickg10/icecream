#!/bin/sh
# Supplemental source-shape audit for the CACHE_SESSION handoff.
# The executable p50cachesession is the behavioral authority: these anchors
# only make accidental deletion/mutation obvious in a source distribution.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}

require_count() {
    expected=$1
    pattern=$2
    file=$3
    label=$4
    actual=$(grep -F -c "$pattern" "$src/$file" || true)
    if [ "$actual" -ne "$expected" ]; then
        echo "FAIL: $label (expected $expected anchor(s), found $actual)" >&2
        exit 1
    fi
    echo "ok - $label"
}

require_count 1 'CACHE_SESSION = 0x50f00000' services/comm.h \
    'collision-resistant Protocol-50 discriminator is present'
require_count 1 'CACHE_SESSION_READY_MAGIC = UINT32_C(0x50f00001)' services/comm.h \
    'fixed raw sidecar-ownership READY token is present'
require_count 1 'case Msg::CACHE_SESSION:' services/comm.cpp \
    'decoder has a dedicated CACHE_SESSION admission case'
require_count 1 'CacheSessionMsg()' services/comm.h \
    'CACHE_SESSION encoder has a dedicated message type'
require_count 1 'cache_session_release_armed = true;' services/comm.cpp \
    'only a successful CACHE_SESSION decode arms release'
require_count 1 'if (!cache_session_release_armed || fd < 0 || protocol != PROTOCOL_VERSION' \
    services/comm.cpp 'release is message-specific and exact-protocol gated'
require_count 1 '|| inofs != intogo || msgtogo != 0 || !pending_frame_ends.empty()) {' \
    services/comm.cpp 'buffered input and pending output barriers are explicit'
require_count 2 'const int released_fd = fd;' services/comm.cpp \
    'ownership is captured before both directional transfers'
require_count 4 '    fd = -1;' services/comm.cpp \
    'destructor and all transfers clear descriptor ownership explicitly'
require_count 4 'cache_session_release_armed = false;' services/comm.cpp \
    'construction, parser use, transfer, and outbound send clear the one-shot arm'
require_count 1 'const uint32_t ready = htonl(CACHE_SESSION_READY_MAGIC);' services/comm.cpp \
    'sidecar READY is encoded in exact network order'
require_count 1 'ntohl(ready) != CACHE_SESSION_READY_MAGIC' services/comm.cpp \
    'client validates the exact READY fixture before release'
require_count 1 'const bool armed = cache_session_send_release_armed;' services/comm.cpp \
    'client consumes the one-shot send arm on every READY attempt'
require_count 1 'channel->release_fd_after_cache_session_ready(limit)' \
    cache/p50_cache_service.cpp \
    'production source transfer waits for sidecar ownership before CacheWire'
require_count 1 'set_tcp_user_timeout_past_deadline(channel->fd, deadline)' \
    services/comm.cpp \
    'absolute-deadline channel prevents the ordinary TCP timeout from pre-empting its owner'
require_count 2 'send_cache_session_ready(adopted.get(), deadline)' \
    cache/p50_cache_service.cpp 'production sidecar publishes READY on the adopted descriptor'

if sed -n '/class CacheSessionMsg : public Msg/,/^};/p' "$src/services/comm.h" \
        | grep -E 'C_GUID|[Pp]ayload' >/dev/null 2>&1; then
    echo 'FAIL: ordinary CACHE_SESSION grew payload or C_GUID fields' >&2
    exit 1
fi
echo 'ok - ordinary discriminator has no CacheWire payload fields'
echo 'PASS: source-shape checks are supplemental to behavioral mutation gates'
