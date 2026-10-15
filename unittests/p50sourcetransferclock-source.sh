#!/bin/sh
# Source gate for the patience-only SourceTransfer clock wire: the client
# never names a clock or time namespace, and the sidecar's SourceTransfer
# handler never re-derives one from the operation.
set -eu

root=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
remote="$root/client/remote.cpp"
control="$root/cache/p50_control_operation.cpp"
service="$root/cache/p50_cache_service.cpp"
for file in "$remote" "$control" "$service"; do
    if [ ! -f "$file" ]; then
        echo "FAIL: missing source file $file" >&2
        exit 1
    fi
done

# No client code may bind a transfer to a host clock identity: patience is a
# duration, anchored by the receiver.
for needle in 'process_monotonic_clock_identity' 'AbsoluteMonotonicDeadline' \
              '/proc/self/ns/time'; do
    if grep -rnF "$needle" "$root/client" --include='*.cpp' --include='*.h' \
        >/dev/null 2>&1; then
        echo "FAIL: client sources mention $needle" >&2
        grep -rnF "$needle" "$root/client" --include='*.cpp' --include='*.h' >&2
        exit 1
    fi
done

# The encode must write the patience word itself, big-endian, at 408.
if ! grep -Fq 'control_put_u32(wire.data() + 408, operation.source_patience_ms)' \
    "$control"; then
    echo 'FAIL: SourceTransfer encode does not put source_patience_ms at 408' >&2
    exit 1
fi

# The sidecar's SourceTransfer branch must not read a clock identity back
# out of the decoded operation: its own steady clock is the only anchor.
# The region ends at the next branch boundary (InputFdAttachment), not at
# any other kind mention: a nested kind reference inside the branch must not
# silently shrink what is grepped, and the known tail needles must stay in
# the region so truncation fails instead of checking less.
region=$(awk '
    /ControlOperationKind::SourceTransfer\) \{/ { inside = 1 }
    inside && /ControlOperationKind::InputFdAttachment/ { exit }
    inside { print }
' "$service")
if [ -z "$region" ]; then
    echo 'FAIL: SourceTransfer handler region not found in p50_cache_service.cpp' >&2
    exit 1
fi
for needle in 'transfer_source_on_owner' 'encode_control_operation'; do
    if ! printf '%s' "$region" | grep -Fq "$needle"; then
        echo "FAIL: SourceTransfer handler region lost its tail needle $needle" >&2
        exit 1
    fi
done
for needle in 'process_monotonic_clock_identity' 'matches_clock'; do
    if printf '%s' "$region" | grep -Fq "$needle"; then
        echo "FAIL: sidecar SourceTransfer handler mentions $needle" >&2
        exit 1
    fi
done

echo 'PASS: SourceTransfer patience clock source gates hold'
