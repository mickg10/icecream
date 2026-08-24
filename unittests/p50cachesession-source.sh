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
require_count 1 'if (protocol == PROTOCOL_VERSION) {' services/comm.cpp \
    'decoder admits CACHE_SESSION only at exactly Protocol 50'
require_count 1 'return negotiated_protocol == PROTOCOL_VERSION;' services/comm.h \
    'encoder admission is Protocol-50-only'
require_count 1 'cache_session_release_armed = true;' services/comm.cpp \
    'only a successful CACHE_SESSION decode arms release'
require_count 1 'if (!cache_session_release_armed || fd < 0 || protocol != PROTOCOL_VERSION' \
    services/comm.cpp 'release is message-specific and exact-protocol gated'
require_count 1 '|| inofs != intogo || msgtogo != 0 || !pending_frame_ends.empty()) {' \
    services/comm.cpp 'buffered input and pending output barriers are explicit'
require_count 1 'const int released_fd = fd;' services/comm.cpp \
    'ownership is captured before transfer'
require_count 1 'const ssize_t result = recv(fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);' \
    services/comm.cpp 'kernel-queued input is checked without consumption'
require_count 2 '    fd = -1;' services/comm.cpp \
    'ownership is cleared after transfer so the destructor cannot close it'
require_count 4 'cache_session_release_armed = false;' services/comm.cpp \
    'construction, parser use, transfer, and outbound send clear the one-shot arm'

if grep -n 'CACHE_SESSION.*[Pp]ayload\|C_GUID' "$src/services/comm.h" \
        | grep -v 'CacheWire' >/dev/null 2>&1; then
    echo 'FAIL: ordinary CACHE_SESSION grew payload or C_GUID fields' >&2
    exit 1
fi
echo 'ok - ordinary discriminator has no CacheWire payload fields'
echo 'PASS: source-shape checks are supplemental to behavioral mutation gates'
