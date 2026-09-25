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
require_count 1 'if (!cache_session_release_armed || fd < 0 ||' \
    services/comm.cpp 'release requires the successful message-specific arm'
release_fd_slice=$(sed -n \
    '/^int MsgChannel::release_fd_if_input_empty()/,/^int MsgChannel::release_fd_after_p51_link_session_ready(/p' \
    "$src/services/comm.cpp")
if ! printf '%s\n' "$release_fd_slice" \
        | grep -F '!protocol_supports_p50_r1_bridge(protocol)' >/dev/null; then
    echo 'FAIL: release is not gated to the R1 bridge protocol range' >&2
    exit 1
fi
echo 'ok - release is gated to the R1 bridge protocol range'
r1_protocol_slice=$(sed -n \
    '/^inline constexpr bool protocol_supports_p50_r1_bridge(/,/^inline constexpr bool protocol_supports_result_disposition(/p' \
    "$src/services/comm.h")
if ! printf '%s\n' "$r1_protocol_slice" \
        | grep -F 'protocol >= PROTOCOL_VERSION_P50_CACHE_SESSION_R1 &&' >/dev/null || \
   ! printf '%s\n' "$r1_protocol_slice" \
        | grep -F 'protocol <= PROTOCOL_VERSION_SUPPORTED_MAX' >/dev/null; then
    echo 'FAIL: R1 bridge protocol helper must contain explicit lower and upper bounds' >&2
    exit 1
fi
echo 'ok - R1 bridge protocol lower and upper bounds are explicit'
require_count 1 '|| inofs != intogo || msgtogo != 0 || !pending_frame_ends.empty()) {' \
    services/comm.cpp 'buffered input and pending output barriers are explicit'
r1_release_slices=$( {
    sed -n \
        '/^int MsgChannel::release_fd_if_input_empty()/,/^int MsgChannel::release_fd_after_p51_link_session_ready(/p' \
        "$src/services/comm.cpp"
    sed -n \
        '/^int MsgChannel::release_fd_after_cache_session_ready(/,/^bool MsgChannel::send_msg(/p' \
        "$src/services/comm.cpp"
} )
released_fd_count=$(printf '%s\n' "$r1_release_slices" \
    | grep -F -c 'const int released_fd = fd;' || true)
if [ "$released_fd_count" -ne 2 ]; then
    echo "FAIL: both R1 ownership-transfer directions must capture the descriptor first (found $released_fd_count)" >&2
    exit 1
fi
echo 'ok - both R1 ownership-transfer directions capture the descriptor first'
released_fd_clear_count=$(printf '%s\n' "$r1_release_slices" \
    | grep -F -c '    fd = -1;' || true)
if [ "$released_fd_clear_count" -ne 2 ]; then
    echo "FAIL: both R1 transfers must clear channel ownership after capture (found $released_fd_clear_count)" >&2
    exit 1
fi
echo 'ok - both R1 transfers clear channel ownership after capture'
msgchannel_destructor_slice=$(sed -n \
    '/^MsgChannel::~MsgChannel()/,/^string MsgChannel::dump() const/p' \
    "$src/services/comm.cpp")
destructor_clear_count=$(printf '%s\n' "$msgchannel_destructor_slice" \
    | grep -F -c '    fd = -1;' || true)
if [ "$destructor_clear_count" -ne 1 ]; then
    echo "FAIL: MsgChannel destructor must clear its descriptor ownership exactly once (found $destructor_clear_count)" >&2
    exit 1
fi
echo 'ok - MsgChannel destructor clears its descriptor ownership'
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
require_count 2 'send_cache_session_ready(adopted.get(), deadline)' \
    cache/p50_cache_service.cpp 'production sidecar publishes READY on the adopted descriptor'

create_until_slice=$(sed -n \
    '/^MsgChannel \*Service::createChannelUntil(/,/^MsgChannel \*Service::createChannelRetryUntil(/p' \
    "$src/services/comm.cpp")
create_until_timeout_count=$(printf '%s\n' "$create_until_slice" \
    | grep -F -c 'set_tcp_user_timeout_past_deadline(channel->fd, deadline)' || true)
if [ "$create_until_timeout_count" -ne 1 ]; then
    echo "FAIL: absolute-deadline channel must install its owner timeout exactly once (found $create_until_timeout_count)" >&2
    exit 1
fi
echo 'ok - absolute-deadline channel prevents the ordinary TCP timeout from pre-empting its owner'

create_until_close_count=$(printf '%s\n' "$create_until_slice" \
    | grep -F -c '(void)close(remote_fd);' || true)
if [ "$create_until_close_count" -ne 1 ]; then
    echo "FAIL: post-prepare expired deadline must close its descriptor exactly once (found $create_until_close_count)" >&2
    exit 1
fi
echo 'ok - post-prepare expired deadline closes its unowned descriptor'

create_retry_slice=$(sed -n \
    '/^MsgChannel \*Service::createChannelRetryUntil(/,/^MsgChannel \*Service::createChannel(const string &socket_path)/p' \
    "$src/services/comm.cpp")
create_retry_timeout_count=$(printf '%s\n' "$create_retry_slice" \
    | grep -F -c 'channel->setTcpUserTimeoutUntil(deadline)' || true)
if [ "$create_retry_timeout_count" -ne 1 ]; then
    echo "FAIL: successful sliced retry must restore its outer TCP timeout exactly once (found $create_retry_timeout_count)" >&2
    exit 1
fi
echo 'ok - successful sliced retry restores the unchanged outer TCP timeout'

require_count 2 'test_same_endpoint_retry_after_complete_slice()' \
    unittests/p50cachesession.cpp \
    'real-TCP expired-slice retry regression remains registered'
require_count 2 'test_same_endpoint_retry_immediate_success_owns_outer_timeout()' \
    unittests/p50cachesession.cpp \
    'real-TCP immediate-success outer-timeout regression remains registered'
require_count 3 'timeout_msec > 15000' unittests/p50cachesession.cpp \
    'absolute and both retry success shapes directly inspect the owned socket timeout'

if sed -n '/class CacheSessionMsg : public Msg/,/^};/p' "$src/services/comm.h" \
        | grep -E 'C_GUID|[Pp]ayload' >/dev/null 2>&1; then
    echo 'FAIL: ordinary CACHE_SESSION grew payload or C_GUID fields' >&2
    exit 1
fi
echo 'ok - ordinary discriminator has no CacheWire payload fields'
echo 'PASS: source-shape checks are supplemental to behavioral mutation gates'
