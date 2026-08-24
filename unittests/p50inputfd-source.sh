#!/bin/sh
set -eu

root=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
header=$root/cache/p50_input_fd_attachment.h
source=$root/cache/p50_input_fd_attachment.cpp
makefile=$root/cache/Makefile.am

grep -Fq 'InputRecordKey key' "$header"
grep -Fq 'uint64_t request_id' "$header"
grep -Fq 'for_endpoint' "$header"
grep -Fq 'connect_unix_until' "$source"
grep -Fq 'verify_peer_credentials' "$source"
grep -Fq 'validate_handshake' "$source"
grep -Fq 'FdHandoffSender' "$source"
grep -Fq 'FdHandoffReceiver' "$source"
grep -Fq 'F_SEAL_WRITE' "$source"
grep -Fq 'lseek(fd.get(), 0, SEEK_SET)' "$source"
grep -Fq 'libp50inputfd.a' "$makefile"

if grep -E -n 'FileChunkMsg|FileChunk' "$header" "$source" >/dev/null; then
    echo 'FAIL: input FD attachment acquired a FileChunk path' >&2
    exit 1
fi

# Deletion-sensitive checks: each of these gates a distinct fail-closed edge.
test "$(grep -F -c 'request.request_id != 0' "$source")" -eq 1
test "$(grep -F -c 'expected_peer.specified()' "$source")" -ge 1
test "$(grep -F -c 'make_sealed_memfd' "$source")" -ge 1
test "$(grep -F -c 'status_for_handoff' "$source")" -ge 1
echo 'PASS: p50 input FD attachment source/deletion gates'
