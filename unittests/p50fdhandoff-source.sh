#!/bin/sh
set -eu

test_srcdir=${srcdir:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
source_file="$test_srcdir/../cache/p50_fd_handoff.cpp"
test_file="$test_srcdir/p50fdhandoff.cpp"

gate() {
    file=$1
    grep -Fq 'SCM_RIGHTS' "$file" &&
        grep -Fq 'MSG_CMSG_CLOEXEC' "$file" &&
        grep -Fq 'if ((message.msg_flags & MSG_TRUNC) != 0)' "$file" &&
        grep -Fq 'if ((message.msg_flags & MSG_CTRUNC) != 0)' "$file" &&
        grep -Fq 'cmsg->cmsg_len >= CMSG_LEN(0)' "$file" &&
        grep -Fq 'if (count_fds != 1)' "$file" &&
        grep -Fq 'while (offset != received.wire.size()' "$file" &&
        grep -Fq 'bool rights_sent = false' "$file" &&
        grep -Fq 'received.status = reject_queued_trailing_byte(connection_fd)' "$file" &&
        grep -Fq '(pfd.revents & events) != 0' "$file" &&
        grep -Fq 'peer_credentials_verified()' "$file" &&
        grep -Fq 'std::chrono::steady_clock::time_point deadline' "$file" &&
        grep -Fq 'FD_CLOEXEC' "$file" &&
        grep -Fq 'actual.identity.generation != expected.identity.generation' "$file"
}

gate "$source_file"
grep -Fq 'test_fragmented_and_overlong_request' "$test_file"
grep -Fq 'test_fragmented_rights_and_error_cleanup' "$test_file"
grep -Fq 'test_fragmented_and_overlong_ack' "$test_file"
grep -Fq 'test_queued_request_survives_half_close' "$test_file"
grep -Fq 'fd_handoff_test_set_max_send_chunk(7)' "$test_file"
mutant_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50fd-source.XXXXXX")
trap 'rm -rf "$mutant_dir"' EXIT HUP INT TERM

for pattern in \
    'message.msg_flags & MSG_TRUNC' \
    'message.msg_flags & MSG_CTRUNC' \
    'if (count_fds != 1)' \
    'while (offset != received.wire.size()' \
    'bool rights_sent = false' \
    'received.status = reject_queued_trailing_byte(connection_fd)' \
    '(pfd.revents & events) != 0' \
    'actual.identity.generation != expected.identity.generation'; do
    mutant="$mutant_dir/mutant.cpp"
    sed "/$pattern/d" "$source_file" >"$mutant"
    if gate "$mutant"; then
        echo "deletion mutant survived: $pattern" >&2
        exit 1
    fi
done

exit 0
