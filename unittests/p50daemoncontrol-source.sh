#!/bin/sh
set -eu
src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
impl="$src/cache/p50_daemon_control.cpp"
header="$src/cache/p50_daemon_control.h"
makefile="$src/cache/Makefile.am"
test="$src/unittests/p50_daemon_control_test.cpp"
for file in "$impl" "$header" "$makefile" "$test"; do test -f "$file"; done
for needle in \
    'begin_connected' 'desired_events' 'DaemonControlStatus advance' \
    'SOCK_NONBLOCK' 'getsockopt(fd_, SOL_SOCKET, SO_ERROR' \
    'query_peer_credentials(fd_)' 'syscalls_per_turn' 'bytes_per_turn' \
    'sendmsg' 'recvmsg' 'SCM_RIGHTS' 'MSG_CTRUNC' 'CMSG_NXTHDR' \
    'rights_sent_' 'if (attach_rights) rights_sent_ = true' \
    'offset_ == wire_.size()' 'POLLERR | POLLHUP | POLLNVAL' \
    'DaemonControlPollAdapter' 'cursor_' 'p50_daemon_control.cpp'; do
    grep -F "$needle" "$impl" "$header" "$makefile" >/dev/null
done
if grep -Eq 'wait_for_io|connect_unix_until|FdHandoffSender|Connection::send_until|Connection::receive_until' "$impl"; then
    echo 'FAIL: daemon incremental engine calls a synchronous transport helper' >&2
    exit 1
fi
if sed -n '/begin_connected/,/DaemonControlStatus DaemonControlOperation::advance/p' "$impl" | grep -F 'F_SETFL' >/dev/null; then
    echo 'FAIL: shared connected OFD has its flags mutated' >&2
    exit 1
fi
grep -F 'while (!receiver.done())' "$test" >/dev/null
grep -F 'DaemonControlLimits{2, 7}' "$test" >/dev/null
echo 'PASS: daemon incremental control source contract'
